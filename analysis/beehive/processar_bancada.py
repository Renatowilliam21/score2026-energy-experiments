#!/usr/bin/env python3
"""Processa sender_modos_v2, com ou sem deep sleep e tampa fixa.
Uso: python processar_bancada.py beehive/raw/ID
Nunca altera raw/. Saida nova em beehive/processed/ID_PROCESSAMENTO/.
Limites: CICLO_PERIODICO ate o seguinte, chegada USB alinhada ao INA por offset
mediano. Inclui espera e um boot por periodo com sono. Exclui inicializacao.
Limites estimados: nao mede energia por fase com sincronizacao fisica.
"""
import argparse
import bisect
import csv
import hashlib
import json
import math
import re
import statistics
from datetime import datetime
from pathlib import Path
from uuid import uuid4

VERSION = 'processador_2.0'

def require(ok, message):
    if not ok:
        raise ValueError(message)

def seconds(text):
    value = datetime.fromisoformat(text)
    require(value.tzinfo is not None, 'Horario sem fuso')
    return value.timestamp()

def loglines(path):
    for line in path.read_text(encoding='utf-8').splitlines():
        stamp, sep, body = line.partition(' | ')
        if sep:
            yield seconds(stamp), body

def sender_cycles(path):
    cycles, current, config = [], None, None
    pending_boot = False
    for pc, text in loglines(path):
        if re.fullmatch(r'ETAPA,\d+,BOOT', text):
            pending_boot = True
        elif text.startswith('CONFIG,'):
            new = dict(item.split('=', 1) for item in text.split(',')[1:] if '=' in item)
            if config:
                require(all(new.get(k)==config.get(k) for k in ('modo','intervalo_s','tampa_wakeup','firmware')), 'Configuracao mudou durante a coleta')
            config = new
        elif re.fullmatch(r'ETAPA,\d+,CICLO_PERIODICO', text):
            require(config is not None, 'Falta CONFIG antes do ciclo')
            mode = config.get('modo')
            regular = bool(cycles) and ((mode=='sem_deep_sleep' and not pending_boot) or
                       (mode=='com_deep_sleep' and pending_boot and config.get('causa')=='4'))
            current = dict(pc=pc, dados=[], tentativas=[], resultados=[], repouso=None,
                           config=dict(config), regular=regular, erros=[], fim=False)
            cycles.append(current)
            pending_boot = False
        elif current is not None:
            if text.startswith(('DADOS,', 'TENTATIVA,', 'RESULTADO,')):
                kind, rest = text.split(',', 1)
                d = dict(item.split('=', 1) for item in rest.split(',') if '=' in item)
                if kind == 'DADOS': current['dados'].append(d)
                elif kind == 'TENTATIVA': current['tentativas'].append(d)
                else: current['resultados'].append(d)
            elif re.fullmatch(r'ETAPA,\d+,(DEEP_SLEEP|ESPERA_ACORDADO)', text):
                current['repouso'] = text.rsplit(',',1)[1]
            elif re.fullmatch(r'ETAPA,\d+,CICLO_FIM', text):
                current['fim'] = True
            elif ('ERRO' in text or 'FALHA' in text or 'HORARIOS_PULADOS' in text or
                  re.fullmatch(r'ETAPA,\d+,CICLO_TAMPA', text)):
                current['erros'].append(text)
    return cycles

def integrate(rows, start, end):
    """Interpolacao linear nas bordas; integra P, I, V por trapezios."""
    stamps = [r['t'] for r in rows]
    require(stamps[0]<=start<end<=stamps[-1], 'Janela de integracao fora das amostras')
    def point(t):
        k = bisect.bisect_right(stamps, t)-1
        if stamps[k] == t: return rows[k]
        a, b = rows[k], rows[k+1]
        f = (t-a['t'])/(b['t']-a['t'])
        return dict(t=t, **{c:a[c]+f*(b[c]-a[c]) for c in ('P','I','V')})
    points = [point(start)] + [r for r in rows if start < r['t'] < end] + [point(end)]
    return {c:sum((a[c]+b[c])/2*(b['t']-a['t']) for a,b in zip(points,points[1:])) for c in ('P','I','V')}

def analyze(folder, invert=False, limit=None):
    paths = [folder/n for n in ('manifesto.json','ina219.csv','sender.log','receiver.log')]
    for p in paths: require(p.is_file(), f'Arquivo ausente: {p}')
    meta = json.loads(paths[0].read_text(encoding='utf-8'))
    require(meta.get('motivo_fim') == 'concluida', 'Coleta nao concluida; revisar antes de processar automaticamente')
    raw = list(csv.DictReader(paths[1].open(encoding='utf-8')))
    require(len(raw)>10, 'Poucas amostras')
    require({r['execucao_id'] for r in raw} == {meta['execucao_id']}, 'CSV e manifesto de execucoes diferentes')
    require(len({r['sessao_medidor'] for r in raw})==1, 'Medidor reiniciou; analise por sessoes necessaria')
    require(all(r['status_leitura']=='OK' for r in raw), 'Existem falhas de medicao; revisar sem apagar os dados')
    rows=[]
    for r in raw:
        a=dict(t=int(r['tempo_us'])/1e6, pc=seconds(r['recebido_pc_iso8601']),
               seq=int(r['amostra_id']), P=float(r['potencia_mW']), I=float(r['corrente_mA']), V=float(r['tensao_V']))
        require(all(math.isfinite(a[c]) for c in ('P','I','V')), 'Valor nao finito')
        rows.append(a)
    require(all(b['t']>a['t'] and b['seq']>a['seq'] for a,b in zip(rows,rows[1:])), 'Tempo/sequencia nao crescente')
    require(all(b['pc']>=a['pc'] for a,b in zip(rows,rows[1:])), 'Relogio PC retrocedeu; alinhamento invalido')
    period=statistics.median(b['t']-a['t'] for a,b in zip(rows,rows[1:]))
    require(.018<=period<=.022, 'Esta versao espera coleta nominal de 50 Hz')
    offset=statistics.median(r['pc']-r['t'] for r in rows)
    jitter=sorted(abs((r['pc']-r['t'])-offset) for r in rows)[int(.95*(len(rows)-1))]
    require(jitter<.1, 'Variacao da chegada USB elevada; associacao temporal precisa de revisao')
    sender_path, receiver_path = (paths[3],paths[2]) if invert else (paths[2],paths[3])
    boots=sender_cycles(sender_path)
    require(len(boots)>=3, 'Precisa do ciclo inicial e dois CICLO_PERIODICO regulares; confira as portas')
    packets=[]
    for pc,body in loglines(receiver_path):
        if body.startswith('{'):
            p=json.loads(body)
            if p.get('type')=='colmeia': packets.append((pc,p))
    first_regular=next((i for i,b in enumerate(boots) if b['regular']),None)
    require(first_regular is not None, 'Nenhum ciclo regular apos inicializacao')
    regulars=boots[first_regular:]
    require(len(regulars)>=2, 'Faltam duas bordas regulares completas')
    require(all(b['regular'] for b in regulars), 'Reset ou despertar inesperado durante a serie')
    edges=[(b['pc']-offset,b) for b in regulars]
    require(all(rows[0]['t']<=t<=rows[-1]['t'] for t,_ in edges), 'INA nao cobre todas as bordas regulares')
    configs=[b.get('config',{}) for _,b in edges]
    mode=configs[0].get('modo')
    require(mode in ('com_deep_sleep','sem_deep_sleep'), 'Modo ausente ou desconhecido')
    require(all(c.get('tampa_wakeup')=='0' and c.get('firmware')=='sender_modos_v2' for c in configs), 'Requer firmware sender_modos_v2, sem eventos extras da tampa')
    intervals={int(c['intervalo_s']) for c in configs}
    require(len(intervals)==1, 'Intervalo mudou durante a execucao')
    interval=intervals.pop()
    require(interval>0, 'Intervalo invalido')
    output=[]
    used_keys=set()
    lid_states=set()
    for (start,boot),(end,nextboot) in zip(edges,edges[1:]):
        require(.9*interval<=end-start<=1.1*interval, 'Intervalo entre bordas inesperado; pode haver ciclo nao detectado')
        expected_rest='DEEP_SLEEP' if mode=='com_deep_sleep' else 'ESPERA_ACORDADO'
        require(not boot['erros'] and boot['fim'] and boot['repouso']==expected_rest and len(boot['dados'])==1 and len(boot['resultados'])==1, 'Log incompleto ou ciclo com falha; exige revisao')
        data,result=boot['dados'][0],boot['resultados'][0]
        key=(data['mac'].upper(),int(data['seq']))
        require(re.fullmatch(r'(?:[0-9A-F]{2}:){5}[0-9A-F]{2}',key[0]) is not None, 'MAC invalido')
        require(data['tampa'] in ('aberta','fechada'), 'Tampa invalida')
        require(key not in used_keys, 'Contador repetido entre ciclos; sessao ambigua')
        used_keys.add(key)
        require(result['seq']==data['seq'], 'RESULTADO de outro pacote')
        require(all(x['seq']==data['seq'] for x in boot['tentativas']), 'Tentativas de outro pacote')
        attempts=int(result['tentativas'])
        require(1<=attempts<=3 and result['ack'] in ('0','1'), 'RESULTADO invalido')
        require(len(boot['tentativas'])==attempts, 'Log de tentativas incompleto')
        require([int(x['n']) for x in boot['tentativas']]==list(range(1,attempts+1)), 'Sequencia de tentativas inconsistente')
        lid_states.add(data['tampa'])
        inside=[r for r in rows if start<=r['t']<end]
        require(all(4.5<=r['V']<=5.5 for r in inside), 'Tensao fora da faixa de analise proposta (4,5 a 5,5 V)')
        relevant=[(a,b) for a,b in zip(rows,rows[1:]) if b['t']>start and a['t']<end]
        missing=0
        for a,b in relevant:
            gap=b['t']-a['t']
            require(gap<=.0401, 'Lacuna maior que 40 ms na janela; revisar ciclo')
            if b['seq']-a['seq']>1 or gap>.03:
                require(b['seq']-a['seq']==2 and .035<=gap<=.0401, 'Perda maior que uma amostra isolada')
                local=[r for r in rows if a['t']-.1<=r['t']<=b['t']+.1]
                # Mesma regra para os modos: uma perda isolada apenas em
                # corrente estavel e longe da rotina de leitura/envio.
                rest_pc=next((pc for pc,txt in loglines(sender_path) if boot['pc']<=pc<nextboot['pc'] and txt.endswith(','+expected_rest)),None)
                require(rest_pc is not None and a['t']+offset>rest_pc+.5 and b['t']<end-.5 and
                        max(r['I'] for r in local)-min(r['I'] for r in local)<=2,
                        'Lacuna em atividade/transicao; nao interpolar automaticamente')
                missing+=1
        require(missing<=1, 'Mais de uma perda por ciclo; revisar')
        sums=integrate(rows,start,end)
        received=[p for pc,p in packets if boot['pc']<=pc<nextboot['pc'] and (p.get('mac','').upper(),p.get('counter'))==key]
        for p in received:
            require(p.get('is_open')==(data['tampa']=='aberta'), 'Tampa divergente no receiver')
            for a,b in [('temperature','temp'),('humidity','umid'),('pressure','hPa')]:
                require(isinstance(p.get(a),(int,float)) and abs(p[a]-float(data[b]))<=.021, 'Payload divergente no receiver')
        # Sensibilidade ilustrativa a deslocamentos independentes de +/-100 ms.
        # Nao representa intervalo de confianca nem erro certificado.
        sensitivities=[integrate(rows,start+a,end+b)['P']/1000 for a in (-.1,.1) for b in (-.1,.1)
                       if rows[0]['t']<=start+a<end+b<=rows[-1]['t']]
        output.append(dict(seq=key[1],mac=key[0],modo=mode,inicio_medidor_s=start,fim_medidor_s=end,
                           duracao_s=end-start,energia_J=sums['P']/1000,
                           potencia_media_mW=sums['P']/(end-start),corrente_media_mA=sums['I']/(end-start),
                           tensao_media_V=sums['V']/(end-start),pico_observado_mA=max(r['I'] for r in inside),
                           pacotes_gerados=1,pacotes_unicos_recebidos=int(bool(received)),duplicatas=max(0,len(received)-1),
                           tentativas=attempts,ack=int(result['ack']),amostras=len(inside),ausentes=missing,
                           sensibilidade_100ms_min_J=min(sensitivities) if sensitivities else None,
                           sensibilidade_100ms_max_J=max(sensitivities) if sensitivities else None,
                           maior_lacuna_ms=max(b['t']-a['t'] for a,b in relevant)*1000))
    require(len(lid_states)==1, 'Tampa mudou; requer analise de eventos')
    total_cycles=len(output)
    if limit is not None:
        require(limit>0 and total_cycles>=limit, f'Necessarios {limit} ciclos completos; encontrados {total_cycles}')
        output=output[:limit]
    duration=sum(c['duracao_s'] for c in output);energy=sum(c['energia_J'] for c in output)
    n=len(output);rx=sum(c['pacotes_unicos_recebidos'] for c in output)
    samples=sum(c['amostras'] for c in output);missing=sum(c['ausentes'] for c in output)
    warnings=[]
    if invert: warnings.append('Logs sender/receiver invertidos por opcao explicita; originais preservados')
    if mode not in meta.get('condicao',''): warnings.append('Condicao do manifesto diverge do modo do firmware; modo obtido do CONFIG')
    summary={'Execução':meta['execucao_id'],'Condição':mode+'_'+str(interval)+'s','Repetição':meta.get('repeticao',''),
             'Intervalo programado (s)':interval,'Tampa':next(iter(lid_states)),'Configuração':meta['execucao_id']+'/manifesto.json',
             'Pasta original':str(folder.resolve()),'Duração analisada (s)':duration,'Ciclos completos':n,'Energia total (J)':energy,
             'Energia por ciclo (J)':energy/n,'Potência média (mW)':energy/duration*1000,
             'Corrente média (mA)':sum(c['corrente_media_mA']*c['duracao_s'] for c in output)/duration,
             'Pico observado (mA)':max(c['pico_observado_mA'] for c in output),
             'Tensão média (V)':sum(c['tensao_media_V']*c['duracao_s'] for c in output)/duration,
             'Ativo por ciclo (s)':'','Corrente no sono (mA)':'','Pacotes gerados':n,'Pacotes únicos recebidos':rx,
             'Entrega (%)':rx/n,'Tentativas totais':sum(c['tentativas'] for c in output),
             'Amostras válidas (%)':samples/(samples+missing),'Maior lacuna (ms)':max(c['maior_lacuna_ms'] for c in output),
             'Situação':'Validacao' if meta.get('etapa')=='validacao' else 'Pendente',
             'Observações':f"{meta.get('etapa','nao informada')}; calibracao nominal; bordas estimadas por logs USB; {missing} amostras interpoladas. "+' / '.join(warnings)}
    report=dict(versao=VERSION,execucao_id=meta['execucao_id'],etapa=meta.get('etapa'),
                script_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                metodo='CICLO_PERIODICO ate o proximo CICLO_PERIODICO; chegada USB menos mediana(chegada INA - tempo INA). Integracao trapezoidal nas amostras INA. Mesmo criterio nos dois modos.',
                modo=mode,condicao_original=meta.get('condicao'),avisos=warnings,logs_invertidos=invert,
                ciclos_disponiveis=total_cycles,ciclos_selecionados=len(output),
                ciclos_iniciais_excluidos=first_regular,
                periodo_mediano_s=period,variacao_usb_p95_s=jitter,
                observacao_temporal='Variacao USB nao e limite certificado de erro. Sensibilidade +/-100 ms e apenas cenario ilustrativo; nao IC. Sem energia por fase ou corrente no sono estimada automaticamente.',
                exclusoes='Inicializacao, ciclos sem duas bordas completas e extremidades incompletas nao entram. Falhas encontradas dentro da janela interrompem a analise automatica.',
                resumo=summary,ciclos=output,
                fontes_sha256={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in paths})
    return report

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('pasta',type=Path)
    ap.add_argument('--saida',type=Path,default=Path('beehive/processed'))
    ap.add_argument('--inverter-logs',action='store_true',help='Somente para execucao com sender/receiver comprovadamente trocados')
    ap.add_argument('--ciclos',type=int,help='Usar exatamente os primeiros N ciclos regulares completos; falha se faltarem')
    args=ap.parse_args()
    try:
        report=analyze(args.pasta,invert=args.inverter_logs,limit=args.ciclos)
    except (ValueError,KeyError,OSError,TypeError,IndexError) as e:
        print('REVISAO NECESSARIA:',e)
        return 1
    target=args.saida/(report['execucao_id']+'_'+uuid4().hex[:8])
    require(not target.resolve().is_relative_to(args.pasta.resolve()), 'Saida nao pode ficar na pasta original')
    target.mkdir(parents=True,exist_ok=False)
    (target/'analise.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
    for name,records in [('resumo_execucao.csv',[report['resumo']]),('ciclos.csv',report['ciclos'])]:
        with (target/name).open('x',encoding='utf-8-sig',newline='') as f:
            w=csv.DictWriter(f,fieldnames=list(records[0]));w.writeheader();w.writerows(records)
    s=report['resumo']
    print(f"{s['Ciclos completos']} ciclo(s); {s['Duração analisada (s)']:.3f} s; {s['Energia por ciclo (J)']:.4f} J/ciclo; {s['Potência média (mW)']:.3f} mW")
    print('Resultado:',target.resolve())
    print('Percentuais no CSV sao fracoes: 1 = 100%. Validacao nao vira coleta oficial automaticamente.')
    return 0

if __name__=='__main__':
    raise SystemExit(main())
