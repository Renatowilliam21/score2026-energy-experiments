/* INA219 piloto v4 - ESP32 auxiliar dedicada, sem bibliotecas externas.
   VCC=3V3; GND comum; SDA=21; SCL=22. VIN+ fonte USB; VIN- USB sender.
   Auxiliar alimentada por USB separado, fora do trecho medido.
   Serial 115200 baud; CSV com virgula e ponto decimal. # indica metadados.
   p pausa, s retoma, h ajuda; c=CSV completo; l=legivel (padrao).
   Modo legivel mostra so a ultima leitura a cada 500 ms, sem guardar
   as intermediarias. Para coleta de dados use c antes de iniciar a serie.
   R100=0,1 ohm: confirmar SHUNT_OHMS. Calibracao apenas NOMINAL.
   I(mA)=shunt(mV)/ohm; P(mW)=V*I(mA). Negativos sao preservados.
   Conversao disparada: 12 bits shunt + barramento, 532 us tipicos cada,
   sequenciais, sem medias adicionais. Meta 50 Hz, validar no piloto.
   tempo_us: meio da janela de software inicio/fim, nao instante exato do ADC.
   Relogio auxiliar de 64 bits, reinicia no boot. Nao sincroniza com sender.
   Erros: campos vazios, nao zeros. FORA_FAIXA preserva brutos mas e invalido.
   Coletor deve adicionar execucao_id/horario PC e preservar linhas originais.
   Nao grava arquivo no PC, nao preenche planilha e nao integra energia aqui.
   Nao integrar atraves de erros, pausas, resets ou lacunas excessivas.
   Referencia: https://www.ti.com/lit/ds/symlink/ina219.pdf (Rev G).
*/
#include <Arduino.h>
#include <Wire.h>
#include <esp_timer.h>
#include <esp_system.h>
#include <string.h>

const uint8_t INA_ADDR = 0x40;
const uint32_t SERIAL_BAUD = 115200;
const uint32_t PERIODO_US = 20000; // 20 ms -> meta 50 Hz
constexpr float SHUNT_OHMS = 0.1f;     // R100
const uint32_t TIMEOUT_US = 8000;
// BRNG=0 (16V), PGA=3 (+/-320mV), BADC=3, SADC=3, MODE=3 (trigger).
const uint16_t CONFIG_TRIGGER = (3u<<11) | (3u<<7) | (3u<<3) | 3u;
static_assert(CONFIG_TRIGGER == 0x199B, "Bits incorretos");
static_assert(SHUNT_OHMS > 0, "Shunt deve ser positivo");
static_assert(PERIODO_US >= 5000, "Validar taxa antes de reduzir periodo");
char sessao[40];
uint64_t amostra=0, slotsPerdidos=0, falhas=0;
int64_t proxima=0, ultimoTempo=-1;
bool coletando=true;
bool modoCSV=false;
int64_t ultimaTela=-1;

bool escreverRegistro(uint8_t reg, uint16_t value) {
  Wire.beginTransmission(INA_ADDR);
  Wire.write(reg); Wire.write((uint8_t)(value>>8)); Wire.write((uint8_t)value);
  return Wire.endTransmission()==0;
}
bool lerRegistro(uint8_t reg, uint16_t &value) {
  Wire.beginTransmission(INA_ADDR); Wire.write(reg);
  if (Wire.endTransmission(false)!=0) return false;
  if (Wire.requestFrom(INA_ADDR,(uint8_t)2)!=2) {
    while (Wire.available()) Wire.read();
    return false;
  }
  uint8_t hi=Wire.read(), lo=Wire.read();
  value=((uint16_t)hi<<8)|lo;
  return true;
}
void cabecalho() {
  if (!modoCSV) {
    Serial.println("\n=== INA219 - MONITOR DE ENERGIA v4 ===");
    Serial.printf("Sessao: %s\n", sessao);
    Serial.println("Amostragem: meta 50 Hz | Tela: ultima leitura a cada 0,5 s");
    Serial.println("p = pausar | s = retomar | c = CSV completo | l = legivel | h = ajuda");
    Serial.println("Modo legivel NAO exporta todas as amostras. Use c para coleta.");
    return;
  }
  Serial.printf("# firmware=ina219_piloto_v4,sessao=%s\n",sessao);
  Serial.printf("# periodo_us=%lu,shunt_ohm=%.6f,i2c_hz=400000,config=0x%04X\n",
    (unsigned long)PERIODO_US,SHUNT_OHMS,CONFIG_TRIGGER);
  Serial.println("# corrente=shunt_mV/shunt_ohm; potencia=tensao_V*corrente_mA; calibracao=nominal");
  Serial.println("# tempo desde boot auxiliar; sender NAO sincronizado; vazio=indisponivel");
  Serial.println("sessao_medidor,amostra_id,tempo_us,dt_us,inicio_us,fim_us,tensao_V,corrente_mA,potencia_mW,shunt_mV,shunt_raw,bus_raw,status_leitura,slots_perdidos_total,falhas_total");
}
const char* medir(uint16_t &bus, uint16_t &shunt) {
  // Novo disparo limpa CNVR e impede reutilizar amostra antiga.
  if (!escreverRegistro(0x00,CONFIG_TRIGGER)) return "ERRO_I2C";
  int64_t disparo=esp_timer_get_time();
  uint16_t config;
  if (!lerRegistro(0x00,config)) return "ERRO_I2C";
  if (config!=CONFIG_TRIGGER) return "ERRO_CONFIG";
  while (true) {
    if (!lerRegistro(0x02,bus)) return "ERRO_I2C";
    if (esp_timer_get_time()-disparo>TIMEOUT_US) return "TIMEOUT";
    if (bus&2) break; // CNVR: conversoes concluidas.
    delayMicroseconds(150);
  }
  if (!lerRegistro(0x01,shunt)) return "ERRO_I2C";
  int32_t raw=shunt<0x8000?shunt:(int32_t)shunt-65536;
  // Registros internos de corrente/potencia nao sao usados; calculo no software.
  if (raw<=-32000 || raw>=32000 || (bus>>3)>=4000) return "FORA_FAIXA";
  return "OK";
}
void setup() {
  Serial.begin(SERIAL_BAUD); delay(1000);
  uint64_t mac=ESP.getEfuseMac();
  snprintf(sessao,sizeof(sessao),"%04X%08lX-%08lX",(unsigned)((mac>>32)&0xFFFF),
    (unsigned long)(mac&0xFFFFFFFF),(unsigned long)esp_random());
  Wire.begin(21,22); Wire.setClock(400000); Wire.setTimeOut(10);
  if (!escreverRegistro(0x00,0x8000)) Serial.println("# aviso=INA219_nao_respondeu_ao_reset");
  delay(2); cabecalho(); proxima=esp_timer_get_time();
}
void loop() {
  while (Serial.available()) {
    char c=Serial.read();
    if (c=='p'||c=='P') {
      coletando=false; Serial.printf("# PAUSA,%lld\n",(long long)esp_timer_get_time());
    } else if (c=='s'||c=='S') {
      if (!coletando) {
        coletando=true; proxima=esp_timer_get_time(); ultimoTempo=-1;
        Serial.printf("# RETOMADA,%lld\n",(long long)proxima);
      }
    } else if (c=='c'||c=='C'||c=='l'||c=='L') {
      modoCSV=(c=='c'||c=='C');
      ultimoTempo=-1; ultimaTela=-1;
      Serial.printf("# MODO,%lld,%s\n",(long long)esp_timer_get_time(),modoCSV?"CSV":"LEGIVEL");
      cabecalho();
    } else if (c=='h'||c=='H') cabecalho();
  }
  if (!coletando) { delay(10); return; }
  int64_t agora=esp_timer_get_time();
  if (agora<proxima) {
    if (proxima-agora>1500) delay(1); else delayMicroseconds(100);
    return;
  }
  uint64_t pulados=(uint64_t)(agora-proxima)/PERIODO_US;
  slotsPerdidos+=pulados;
  proxima+=(int64_t)(pulados+1)*PERIODO_US;
  uint16_t bus=0,shunt=0;
  int64_t inicio=esp_timer_get_time();
  const char* status=medir(bus,shunt);
  int64_t fim=esp_timer_get_time(),tempo=inicio+(fim-inicio)/2;
  bool temValores=strcmp(status,"OK")==0||strcmp(status,"FORA_FAIXA")==0;
  if (strcmp(status,"OK")!=0) ++falhas;
  ++amostra;
  if (!modoCSV) {
    int64_t dt=ultimoTempo<0?-1:tempo-ultimoTempo;
    ultimoTempo=tempo;
    if (ultimaTela>=0 && tempo-ultimaTela<500000) return;
    ultimaTela=tempo;
    Serial.printf("\nTempo: %.2f s | Amostra: %llu | Status: %s\n",
      tempo/1000000.0,(unsigned long long)amostra,status);
    if (temValores) {
      int32_t raw=shunt<0x8000?shunt:(int32_t)shunt-65536;
      float shunt_mV=raw*0.01f,volts=(bus>>3)*0.004f;
      float mA=shunt_mV/SHUNT_OHMS;
      Serial.printf("Tensao:   %.3f V\nCorrente: %.3f mA\nPotencia: %.3f mW\nShunt:    %.3f mV\n",
        volts,mA,volts*mA,shunt_mV);
    } else Serial.println("Leitura eletrica indisponivel nesta amostra.");
    if (dt>=0) Serial.printf("Intervalo entre amostras: %.3f ms\n",dt/1000.0);
    Serial.printf("Falhas acumuladas: %llu | Intervalos perdidos: %llu\n",
      (unsigned long long)falhas,(unsigned long long)slotsPerdidos);
    return;
  }
  Serial.printf("%s,%llu,%lld,",sessao,(unsigned long long)amostra,(long long)tempo);
  if (ultimoTempo>=0) Serial.printf("%lld",(long long)(tempo-ultimoTempo));
  Serial.printf(",%lld,%lld,",(long long)inicio,(long long)fim); ultimoTempo=tempo;
  if (temValores) {
    int32_t raw=shunt<0x8000?shunt:(int32_t)shunt-65536;
    float shunt_mV=raw*0.01f,volts=(bus>>3)*0.004f;
    float mA=shunt_mV/SHUNT_OHMS,mW=volts*mA;
    Serial.printf("%.3f,%.3f,%.3f,%.3f,%ld,%u,",volts,mA,mW,shunt_mV,(long)raw,(unsigned)bus);
  } else Serial.print(",,,,,,"); // Seis campos vazios.
  Serial.printf("%s,%llu,%llu\n",status,(unsigned long long)slotsPerdidos,(unsigned long long)falhas);
}
