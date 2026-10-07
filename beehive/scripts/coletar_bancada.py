#!/usr/bin/env python3
"""Grava INA219, sender e receiver. Python 3.10+ e pyserial 3.5.
Relogios dos dispositivos NAO sao sincronizados por este coletor.
"""
import argparse
import csv
import json
import math
import os
import re
import sys
import time
import hashlib
import threading
from contextlib import ExitStack
from datetime import datetime
from pathlib import Path
from uuid import uuid4

FIELDS = ('sessao_medidor,amostra_id,tempo_us,dt_us,inicio_us,fim_us,'
          'tensao_V,corrente_mA,potencia_mW,shunt_mV,shunt_raw,bus_raw,'
          'status_leitura,slots_perdidos_total,falhas_total').split(',')
STATUS = {'OK', 'FORA_FAIXA', 'ERRO_I2C', 'ERRO_CONFIG', 'TIMEOUT'}


def now():
    return datetime.now().astimezone().isoformat(timespec='milliseconds')


def parse_line(text):
    row = next(csv.reader([text], strict=True))
    if len(row) != len(FIELDS):
        raise ValueError(f'campos={len(row)}, esperado=15')
    if not re.fullmatch(r'[0-9A-Fa-f]{12}-[0-9A-Fa-f]{8}', row[0]):
        raise ValueError('sessao invalida')
    if row[12] not in STATUS:
        raise ValueError('status desconhecido')
    for i in (1, 2, 4, 5, 13, 14):
        if not row[i].isdigit():
            raise ValueError(f'inteiro invalido: {FIELDS[i]}')
    if row[3] and not row[3].isdigit():
        raise ValueError('dt invalido')
    if not int(row[4]) <= int(row[2]) <= int(row[5]):
        raise ValueError('janela temporal inconsistente')
    if row[12] in ('OK', 'FORA_FAIXA'):
        for i in range(6, 10):
            if not math.isfinite(float(row[i])):
                raise ValueError('valor eletrico nao finito')
        if not re.fullmatch(r'-?\d+', row[10]) or not row[11].isdigit():
            raise ValueError('registrador invalido')
        if not -32768 <= int(row[10]) <= 32767 or not 0 <= int(row[11]) <= 65535:
            raise ValueError('registrador fora de 16 bits')
    elif any(row[6:12]):
        raise ValueError('campos eletricos preenchidos em falha')
    return row


def capture(role, dev, folder, run, origin, stop, result):
    """Um leitor por porta; arquivos independentes preservam ordem e bytes."""
    pending = bytearray()
    offset = 0
    previous = {}
    seen_csv = False
    last_cmd = -math.inf
    result.update(bytes=0, linhas=0, registros=0, rejeitadas=0, falhas_sensor=0,
                  sequencias_sinalizadas=0, sessoes=[], erro=None)
    try:
        with ExitStack() as stack:
            raw = stack.enter_context((folder / f'{role}_raw.bin').open('xb'))
            log = stack.enter_context((folder / f'{role}.log').open('x', encoding='utf-8'))
            events = stack.enter_context((folder / f'{role}_eventos.jsonl').open('x', encoding='utf-8'))
            files = [raw, log, events]
            output = stack.enter_context((folder / ('ina219.csv' if role == 'ina219' else f'{role}_registros.jsonl')).open('x', encoding='utf-8', newline=''))
            files.append(output)
            writer = csv.writer(output) if role == 'ina219' else None
            if writer:
                writer.writerow(['execucao_id', 'recebido_pc_iso8601', 'offset_raw', 'flags_coletor', 'pc_monotonic_ns'] + FIELDS)

            def event(kind, **kw):
                events.write(json.dumps(dict(evento=kind, horario_pc=now(), pc_monotonic_ns=time.monotonic_ns()-origin, **kw), ensure_ascii=False) + '\n')

            def flush():
                for f in files:
                    f.flush()
                    os.fsync(f.fileno())

            event('PORTA_ABERTA', porta=dev.port)
            last_flush = time.monotonic()
            try:
                while not stop.is_set():
                    if role == 'ina219' and not seen_csv and time.monotonic()-last_cmd >= 1:
                        dev.write(b'cs\n')
                        last_cmd = time.monotonic()
                        event('COMANDO', texto='cs')
                    chunk = dev.read(min(max(dev.in_waiting, 1), 8192))
                    received, mono = now(), time.monotonic_ns()-origin
                    if chunk:
                        raw.write(chunk)
                        result['bytes'] += len(chunk)
                        pending.extend(chunk)
                    while b'\n' in pending:
                        size = pending.index(b'\n')+1
                        bline = bytes(pending[:size])
                        del pending[:size]
                        here = offset
                        offset += size
                        text = bline.decode('utf-8', errors='replace').rstrip('\r\n')
                        log.write(f'{received} | {text}\n')
                        result['linhas'] += 1
                        if role == 'ina219':
                            try:
                                line = bline.decode('ascii').strip()
                                if line == ','.join(FIELDS):
                                    seen_csv = True
                                    event('CABECALHO_CSV', offset_raw=here)
                                    continue
                                if line.startswith('=== INA219'):
                                    seen_csv = False
                                if not line or line.startswith('#'):
                                    event('METADADO', offset_raw=here, texto=line)
                                    continue
                                row = parse_line(line)
                            except (ValueError, csv.Error) as exc:
                                result['rejeitadas'] += 1
                                event('LINHA_REJEITADA', offset_raw=here, bytes=size, motivo=str(exc))
                                continue
                            seen_csv = True
                            session, seq, stamp = row[0], int(row[1]), int(row[2])
                            flags = []
                            prev = previous.get(session)
                            if prev:
                                if seq != prev[0]+1: flags.append('SALTO_OU_REPETICAO_SEQ')
                                if stamp <= prev[1]: flags.append('TEMPO_NAO_CRESCENTE')
                                if row[3] and int(row[3]) != stamp-prev[1]: flags.append('DT_DIVERGE')
                            elif previous:
                                flags.append('NOVA_SESSAO_MEDIDOR')
                            previous[session] = (seq, stamp)
                            result['sessoes'] = list(previous)
                            if flags:
                                result['sequencias_sinalizadas'] += 1
                                event('SEQUENCIA', offset_raw=here, flags=flags)
                            writer.writerow([run, received, here, '|'.join(flags), mono] + row)
                            result['registros'] += 1
                            result['falhas_sensor'] += row[12] != 'OK'
                        else:
                            record = dict(execucao_id=run, recebido_pc_iso8601=received,
                                          pc_monotonic_ns=mono, offset_raw=here, texto=text)
                            try:
                                clean = bline.decode('utf-8').strip()
                                if role == 'receiver' and clean.startswith('{'):
                                    payload = json.loads(clean, parse_constant=lambda x: (_ for _ in ()).throw(ValueError(x)))
                                    if not isinstance(payload, dict) or payload.get('type') != 'colmeia':
                                        raise ValueError('JSON nao e pacote colmeia')
                                    if not re.fullmatch(r'(?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}', str(payload.get('mac', ''))):
                                        raise ValueError('MAC invalido')
                                    if type(payload.get('counter')) is not int or type(payload.get('is_open')) is not bool:
                                        raise ValueError('Contador/estado invalido')
                                    record['pacote'] = payload
                                    result['registros'] += 1  # Inclui duplicatas, preservadas.
                                elif role == 'sender' and clean.startswith(('ETAPA,', 'DADOS,', 'RESULTADO,', 'TENTATIVA,', 'CONFIG,', 'PROXIMO_CICLO_US,')):
                                    record['tipo'] = clean.split(',', 1)[0]
                                    result['registros'] += 1
                            except (ValueError, UnicodeError) as exc:
                                record['erro_decodificacao'] = str(exc)
                                result['rejeitadas'] += 1
                            output.write(json.dumps(record, ensure_ascii=False) + '\n')
                    if len(pending) > 65536:
                        event('FRAGMENTO_SEM_NEWLINE', offset_raw=offset, bytes=len(pending))
                        offset += len(pending)
                        pending.clear()
                    if time.monotonic()-last_flush >= 1:
                        flush()
                        last_flush = time.monotonic()
            finally:
                if pending:
                    event('FRAGMENTO_FINAL', offset_raw=offset, bytes=len(pending))
                event('FIM_LEITOR')
                flush()
    except Exception as exc:
        result['erro'] = str(exc)
        stop.set()
        print(f'ERRO em {role}: {exc}', file=sys.stderr, flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--receiver', default='/dev/ttyUSB0')
    ap.add_argument('--sender', default='/dev/ttyUSB1')
    ap.add_argument('--ina', default='/dev/ttyUSB2')
    ap.add_argument('--duracao', type=float, default=130, help='Segundos apos abrir as tres portas; 0 = ate Ctrl+C')
    ap.add_argument('--saida', type=Path, default=Path('beehive/raw'))
    ap.add_argument('--condicao', default='validacao_60s_tampa_fechada')
    ap.add_argument('--repeticao', type=int, default=1)
    ap.add_argument('--etapa', choices=['validacao','oficial'], default='validacao')
    ap.add_argument('--listar-portas', action='store_true')
    args = ap.parse_args()
    if not math.isfinite(args.duracao) or args.duracao < 0 or args.repeticao < 1:
        ap.error('Duracao deve ser finita e >= 0; repeticao >= 1')
    try:
        import serial
        from serial.tools import list_ports
    except ImportError:
        print('Instale: python -m pip install pyserial', file=sys.stderr)
        return 1
    if args.listar_portas:
        for p in list_ports.comports():
            if 'ttyS' not in p.device:
                print(f'{p.device} | {p.description} | serial={p.serial_number}')
        return 0
    ports = dict(receiver=args.receiver, sender=args.sender, ina219=args.ina)
    if len({os.path.realpath(p) for p in ports.values()}) != 3:
        ap.error('As tres portas precisam ser diferentes')
    run = datetime.now().strftime('%Y%m%d_%H%M%S')+'_'+uuid4().hex[:8]
    folder = (args.saida/run).resolve()
    folder.mkdir(parents=True, exist_ok=False)
    origin = time.monotonic_ns()
    meta = dict(execucao_id=run, inicio=now(), coletor='bancada_1.1', portas=ports,
                baud=115200, duracao_solicitada_s=args.duracao, condicao=args.condicao,
                repeticao=args.repeticao, calibracao='nominal, sem referencia externa',
                sincronizado_sender=False, etapa=args.etapa,
                observacao='Tempo PC = chegada USB, nao instante de medicao. Duplicatas preservadas. Sem checksum no protocolo. Analise e preenchimento da planilha separados.',
                script_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest())
    def manifest():
        tmp = folder/'manifesto.tmp'
        tmp.write_text(json.dumps(meta, indent=2, ensure_ascii=False), encoding='utf-8')
        tmp.replace(folder/'manifesto.json')
    manifest()
    devices, threads, results = {}, [], {r: {} for r in ports}
    stop = threading.Event()
    reason = 'concluida'
    start = None
    print(f'Pasta: {folder}', flush=True)
    try:
        for role, port in ports.items():
            dev = serial.Serial(port=None, baudrate=115200, timeout=0.1, write_timeout=1,
                                **({'exclusive': True} if os.name == 'posix' else {}))
            devices[role] = dev
            dev.dtr = False
            dev.rts = False
            dev.port = port
            dev.open()
        start = time.monotonic()
        meta['inicio_captura'] = now()
        manifest()
        for role, dev in devices.items():
            thread = threading.Thread(target=capture, args=(role,dev,folder,run,origin,stop,results[role]))
            threads.append(thread)
            thread.start()
        print('GRAVANDO as tres portas. Mantenha as placas conectadas. Ctrl+C encerra.', flush=True)
        while not stop.wait(1):
            elapsed = time.monotonic()-start
            if args.duracao and elapsed >= args.duracao:
                break
            if int(elapsed) % 5 == 0:
                print(f'{elapsed:.0f}s | INA {results["ina219"].get("registros",0)} amostras | sender {results["sender"].get("registros",0)} eventos | receiver {results["receiver"].get("registros",0)} pacotes (inclui repetidos)', flush=True)
    except KeyboardInterrupt:
        reason = 'interrompida_pelo_usuario'
    except Exception as exc:
        reason = 'erro'
        meta['erro'] = str(exc)
        print(f'Erro: {exc}. Feche app.py, coletores e monitores seriais.', file=sys.stderr)
    finally:
        stop.set()
        for thread in threads:
            thread.join()
        for dev in devices.values():
            if dev.is_open:
                dev.close()
        if any(r.get('erro') for r in results.values()):
            reason = 'erro'
        meta.update(fim=now(), motivo_fim=reason, duracao_captura_s=round(time.monotonic()-start,3) if start else 0,
                    resultados=results, fontes_sem_registros=[role for role,r in results.items() if not r.get('registros')])
        manifest()
    print(f'Arquivos salvos em: {folder}', flush=True)
    if meta['fontes_sem_registros']:
        print('ATENCAO: sem registros reconhecidos em '+', '.join(meta['fontes_sem_registros']))
    return 1 if reason == 'erro' or meta['fontes_sem_registros'] else 0


if __name__ == '__main__':
    raise SystemExit(main())
