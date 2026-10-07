/*
  Sender modos v2 - ESP32 DevModule classica.
  Bibliotecas: LoRa_E32 (xreef), Adafruit BME280, Adafruit Unified Sensor.
  E32 RXD -> GPIO16; TXD -> GPIO17; AUX -> 5; M0 -> 18; M1 -> 19.
  BME280 SDA -> 21; SCL -> 22. MC-38 entre GPIO4 e GND.
  Alimentacao USB; sem leitura de bateria. INA219 em OUTRA ESP32.

  Ciclos periodicos numa grade de 60 s (relogio RTC, sem NTP).
  Boot e inicializacao introduzem atraso: os instantes efetivos sao logados.
  Por padrao, tampa apenas medida: sem ciclos extras por abertura/fechamento.
  COM_DEEP_SLEEP dorme entre ciclos; SEM_DEEP_SLEEP aguarda em loop/delay(1).
  Radio e sensor ficam em repouso em AMBOS os modos. Nao compara radio ligado.
  Se habilitados, eventos de tampa nao deslocam a grade periodica.
  Primeiro boot/reset configura e verifica o E32; ciclos seguintes
  reutilizam a configuracao. Nao desligue o radio separadamente nesse periodo.
  Primeiro ciclo inclui configuracao: separar dos ciclos regulares na analise.

  Pacote legado de 43 bytes. Counter agora identifica uma nova leitura;
  retransmissoes usam o mesmo contador. Preservado em deep sleep, reinicia
  em reset/queda de energia. ACK legado contem somente MAC: nao identifica
  pacote e nao prova gravacao no PC. Receiver pode guardar duplicatas.
  Logs de etapas sao locais, nao sincronizam automaticamente com o INA219.
  Manter LOGS igual em toda serie. Nao iniciar Wi-Fi/Bluetooth.
*/
#include <Arduino.h>
#include <HardwareSerial.h>
#include <LoRa_E32.h>
#include <Wire.h>
#include <Adafruit_BME280.h>
#include <Adafruit_Sensor.h>
#include <esp_mac.h>
#include <esp_sleep.h>
#include <esp_err.h>
#include <driver/rtc_io.h>
#include <driver/gpio.h>
#include <sys/time.h>
#include <math.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>

// Altere APENAS esta opcao entre as duas condicoes do experimento.
enum ModoExperimento { COM_DEEP_SLEEP, SEM_DEEP_SLEEP };
const ModoExperimento MODO_EXPERIMENTO = SEM_DEEP_SLEEP;
const uint32_t INTERVALO_SEGUNDOS = 60;
// Comparacao periodica: tampa continua sendo medida, mas nao gera ciclos extras.
const bool ACORDAR_PELA_TAMPA = false;
const bool LOGS = true;
const uint32_t TEMPO_ACK_MS = 3000;
const int MAX_TENTATIVAS = 3;
const int PIN_PORTA = 4;
// Polaridade da montagem observada: LOW=aberta, HIGH=fechada.
const int NIVEL_TAMPA_ABERTA = LOW;
const uint8_t CANAL = 13;
const uint8_t ENDERECO_SENDER = 1;
const uint8_t ENDERECO_RECEIVER = 3;
static_assert(INTERVALO_SEGUNDOS >= 30, "Use intervalo de pelo menos 30 s neste piloto");

HardwareSerial SerialPort(2);
LoRa_E32 radio(&SerialPort, 5, 18, 19);
Adafruit_BME280 bme;
RTC_DATA_ATTR int32_t contador = 0;
RTC_DATA_ATTR int64_t proximoCiclo = 0;
RTC_DATA_ATTR bool radioConfigurado = false;
RTC_DATA_ATTR bool ultimaPorta = false;
char mac[18];
const int64_t INTERVALO_US = (int64_t)INTERVALO_SEGUNDOS * 1000000LL;
bool bmePronto = false;
bool uartPronta = false;
const char* nomeModo() {
  return MODO_EXPERIMENTO == COM_DEEP_SLEEP ? "com_deep_sleep" : "sem_deep_sleep";
}

// Arrays de bytes preservam exatamente o layout do sender original.
struct ColmeiaData {
  char type[8] = "colmeia";
  uint8_t id_colmeia[6];
  uint8_t temperature[4];
  uint8_t humidity[4];
  uint8_t altitude[4];
  uint8_t pressure[4];
  uint8_t peso[4];
  uint8_t battery[4];
  uint8_t counter[4];
  bool status;
} data;
static_assert(sizeof(float) == 4, "Float deve ter 4 bytes");
static_assert(sizeof(ColmeiaData) == 43, "Pacote deve ter 43 bytes");
static_assert(offsetof(ColmeiaData, temperature) == 14, "Offset de temperatura incorreto");
static_assert(offsetof(ColmeiaData, counter) == 38, "Offset de contador incorreto");
static_assert(offsetof(ColmeiaData, status) == 42, "Offset de status incorreto");


int64_t agoraUs() {
  timeval t;
  gettimeofday(&t, nullptr);
  return (int64_t)t.tv_sec * 1000000LL + t.tv_usec;
}
void etapa(const char* nome) {
  if (LOGS) Serial.printf("ETAPA,%lld,%s\n", (long long)agoraUs(), nome);
}
bool portaAberta() {
  bool estado = digitalRead(PIN_PORTA) == NIVEL_TAMPA_ABERTA;
  // Estabilidade por 30 ms, com limite para contato ruidoso.
  uint32_t inicio = millis(), estavel = inicio;
  while (millis() - inicio < 200) {
    bool atual = digitalRead(PIN_PORTA) == NIVEL_TAMPA_ABERTA;
    if (atual != estado) { estado = atual; estavel = millis(); }
    if (millis() - estavel >= 30) return estado;
    delay(1);
  }
  return estado;
}
void gravarFloat(uint8_t* destino, float valor) {
  memcpy(destino, &valor, sizeof(valor));
}
void dormir(bool observarTampa) {
  etapa("PREPARAR_REPOUSO");
  if (bmePronto) bme.setSampling(Adafruit_BME280::MODE_SLEEP);
  // E32 modo 3: M0=M1=HIGH. Aguarda AUX, com limite, antes do sono.
  if (uartPronta) SerialPort.flush();
  digitalWrite(18, HIGH);
  digitalWrite(19, HIGH);
  delay(5);
  uint32_t inicio = millis();
  while (digitalRead(5) == LOW && millis() - inicio < 1000) delay(1);
  if (digitalRead(5) == LOW) {
    radioConfigurado = false;
    Serial.println("ERRO: AUX baixo ao preparar sono; verificar radio.");
  }
  // Mantem os pinos de modo e TX em niveis definidos no deep sleep.
  if (uartPronta) SerialPort.end();
  uartPronta = false;
  pinMode(16, OUTPUT);
  digitalWrite(16, HIGH);
  // A mesma grade absoluta e a mesma politica de atraso nos dois modos.
  int64_t restante = proximoCiclo - agoraUs();
  if (restante <= 0) {
    int64_t pulados = (-restante) / INTERVALO_US + 1;
    proximoCiclo += pulados * INTERVALO_US;
    if (LOGS) Serial.printf("HORARIOS_PULADOS,%lld\n", (long long)pulados);
  }
  if (MODO_EXPERIMENTO == SEM_DEEP_SLEEP) {
    etapa("ESPERA_ACORDADO");
    if (LOGS) Serial.printf("PROXIMO_CICLO_US,%lld\n", (long long)proximoCiclo);
    Serial.flush();
    return; // loop() aguarda; nao chama deep sleep nem light sleep.
  }
  gpio_hold_en(GPIO_NUM_16);
  gpio_hold_en(GPIO_NUM_18);
  gpio_hold_en(GPIO_NUM_19);
  gpio_deep_sleep_hold_en();

  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  if (observarTampa && ACORDAR_PELA_TAMPA) {
    rtc_gpio_pullup_en(GPIO_NUM_4);
    rtc_gpio_pulldown_dis(GPIO_NUM_4);
    // Espera o estado OPOSTO ao ultimo processado: evita acordar
    // continuamente enquanto a tampa permanece aberta.
    const int nivelFechada = NIVEL_TAMPA_ABERTA == HIGH ? LOW : HIGH;
    const int nivelDespertar = ultimaPorta ? nivelFechada : NIVEL_TAMPA_ABERTA;
    ESP_ERROR_CHECK(esp_sleep_enable_ext0_wakeup(GPIO_NUM_4, nivelDespertar));
  }
  etapa("DEEP_SLEEP");
  if (LOGS) Serial.printf("PROXIMO_CICLO_US,%lld\n", (long long)proximoCiclo);
  Serial.flush();
  restante = proximoCiclo - agoraUs();
  if (restante < 1000) restante = 1000;
  ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup((uint64_t)restante));
  esp_deep_sleep_start();
}
void parar(const char* mensagem) {
  Serial.print("ERRO: "); Serial.println(mensagem);
  // Falhas nao mantem a ESP acordada indefinidamente. Tenta no timer.
  dormir(false);
  // No modo acordado, uma falha fatal fica explicita e reinicia no prazo.
  // O reset invalida essa execucao para comparacao automatica.
  while (agoraUs() < proximoCiclo) delay(1);
  ESP.restart();
  while (true) delay(1000);
}
Configuration consultarRadio() {
  ResponseStructContainer resposta = radio.getConfiguration();
  Serial.print("Consulta do radio: ");
  Serial.println(resposta.status.getResponseDescription());
  if (resposta.status.code != 1) {
    // Algumas versoes retornam data nao inicializado em falha precoce.
    // Para aqui sem acessar/free do ponteiro; nao ha repeticao nem vazamento acumulado.
    parar("Falha na consulta. Confira TX/RX, AUX, M0/M1 e alimentacao do E32.");
  }
  if (resposta.data == nullptr) parar("Consulta retornou sem dados.");
  Configuration cfg = *(Configuration*)resposta.data;
  resposta.close();
  if (cfg.HEAD != 0xC0 && cfg.HEAD != 0xC2) parar("Cabecalho de configuracao invalido.");
  return cfg;
}
void configurarRadio() {
  if (!radio.begin()) parar("Falha em radio.begin(). Confira AUX e alimentacao.");
  Serial.print("AUX: "); Serial.println(digitalRead(5));
  Configuration cfg = consultarRadio();
  cfg.ADDH = 0;
  cfg.ADDL = ENDERECO_SENDER;
  cfg.CHAN = CANAL;
  cfg.SPED.uartBaudRate = UART_BPS_9600;
  cfg.SPED.uartParity = MODE_00_8N1;
  cfg.SPED.airDataRate = AIR_DATA_RATE_010_24;
  cfg.OPTION.wirelessWakeupTime = WAKE_UP_250;
  cfg.OPTION.ioDriveMode = IO_D_MODE_PUSH_PULLS_PULL_UPS;
  cfg.OPTION.fec = FEC_0_OFF;
  cfg.OPTION.transmissionPower = POWER_20;
  cfg.OPTION.fixedTransmission = FT_FIXED_TRANSMISSION;
  ResponseStatus resultado = radio.setConfiguration(cfg, WRITE_CFG_PWR_DWN_LOSE);
  Serial.print("Aplicacao da configuracao: ");
  Serial.println(resultado.getResponseDescription());
  if (resultado.code != 1) parar("Nao foi possivel configurar o E32.");

  // Deixa a resposta da escrita terminar antes da consulta de verificacao.
  delay(200);
  while (SerialPort.available()) SerialPort.read();
  Configuration lida = consultarRadio();
  if (lida.ADDH != 0 || lida.ADDL != ENDERECO_SENDER || lida.CHAN != CANAL ||
      lida.SPED.uartBaudRate != UART_BPS_9600 || lida.SPED.uartParity != MODE_00_8N1 ||
      lida.SPED.airDataRate != AIR_DATA_RATE_010_24 ||
      lida.OPTION.fixedTransmission != FT_FIXED_TRANSMISSION ||
      lida.OPTION.fec != FEC_0_OFF) {
    parar("Configuracao relida difere da solicitada.");
  }
  Serial.printf("Configuracao LIDA: HEAD=%02X ADDH=%u ADDL=%u CANAL=%u\n",
                lida.HEAD, lida.ADDH, lida.ADDL, lida.CHAN);
  Serial.println("UART=9600/8N1, ar=2.4kbps, transmissao fixa, FEC desligado.");
  if (radio.setMode(MODE_0_NORMAL) != 1) parar("Falha ao entrar no modo normal.");
  delay(100);
  while (SerialPort.available()) SerialPort.read();
}


bool prepararDados(bool aberta) {
  etapa("SENSOR_INICIO");
  bmePronto = bme.begin(0x76) || bme.begin(0x77);
  if (!bmePronto) {
    Serial.println("ERRO: BME280 nao encontrado. Nenhum dado ficticio sera enviado.");
    etapa("SENSOR_FALHA");
    return false;
  }
  bme.setSampling(Adafruit_BME280::MODE_FORCED,
                  Adafruit_BME280::SAMPLING_X1,
                  Adafruit_BME280::SAMPLING_X1,
                  Adafruit_BME280::SAMPLING_X1,
                  Adafruit_BME280::FILTER_OFF);
  if (!bme.takeForcedMeasurement()) {
    etapa("SENSOR_FALHA");
    return false;
  }
  float temp = bme.readTemperature();
  float umid = bme.readHumidity();
  float pressao = bme.readPressure() / 100.0f;
  // Derivada da mesma amostra de pressao; referencia fixa, nao altitude calibrada.
  float altitude = 44330.0f * (1.0f - powf(pressao / 1013.25f, 0.1903f));
  bme.setSampling(Adafruit_BME280::MODE_SLEEP);
  etapa("SENSOR_FIM");
  if (!isfinite(temp) || !isfinite(umid) || !isfinite(pressao) ||
      !isfinite(altitude) || temp < -40 || temp > 85 ||
      umid < 0 || umid > 100 || pressao < 300 || pressao > 1100) {
    Serial.println("ERRO: leitura invalida; envio cancelado neste ciclo.");
    return false;
  }
  contador = contador == INT32_MAX ? 1 : contador + 1;
  gravarFloat(data.temperature, temp);
  gravarFloat(data.humidity, umid);
  gravarFloat(data.altitude, altitude);
  gravarFloat(data.pressure, pressao);
  gravarFloat(data.peso, 0.0f); // Campo legado sem balanca.
  gravarFloat(data.battery, NAN); // USB: bateria indisponivel.
  memcpy(data.counter, &contador, 4);
  data.status = aberta;
  if (LOGS) Serial.printf("DADOS,mac=%s,seq=%ld,temp=%.2f,umid=%.2f,hPa=%.2f,tampa=%s\n",
    mac, (long)contador, temp, umid, pressao, aberta ? "aberta" : "fechada");
  return true;
}
bool esperarACK() {
  // Resposta esperada: os 17 caracteres do MAC, sem newline.
  char janela[18] = {};
  size_t n = 0;
  uint32_t inicio = millis();
  while (millis() - inicio < TEMPO_ACK_MS) {
    while (SerialPort.available()) {
      char ch = (char)SerialPort.read();
      if (n == 17) { memmove(janela, janela + 1, 16); n = 16; }
      janela[n++] = ch;
      janela[n] = '\0';
      if (n == 17 && strcmp(janela, mac) == 0) return true;
    }
    delay(1);
  }
  return false;
}

void enviarDados() {
  bool confirmado = false;
  int tentativas = 0;
  for (int t = 1; t <= MAX_TENTATIVAS; ++t) {
    tentativas = t;
    if (radio.setMode(MODE_0_NORMAL) != 1) {
      radioConfigurado = false;
      Serial.println("ERRO: radio nao entrou em modo normal.");
      break;
    }
    while (SerialPort.available()) SerialPort.read();
    if (LOGS) Serial.printf("TENTATIVA,seq=%ld,n=%d\n", (long)contador, t);
    etapa("ENVIO_INICIO");
    ResponseStatus r = radio.sendFixedMessage(0, ENDERECO_RECEIVER, CANAL, &data, sizeof(data));
    etapa("ENVIO_FIM");
    if (r.code == 1) {
      etapa("ACK_ESPERA");
      confirmado = esperarACK();
      etapa(confirmado ? "ACK_RECEBIDO" : "ACK_TIMEOUT");
    } else {
      Serial.print("ERRO_ENVIO: "); Serial.println(r.getResponseDescription());
    }
    if (confirmado) break;
    if (t < MAX_TENTATIVAS) delay(200);
  }
  if (LOGS) Serial.printf("RESULTADO,seq=%ld,tentativas=%d,ack=%d\n",
                          (long)contador, tentativas, confirmado);
}
void executarCiclo(bool retomada, int causa) {
  // Define os niveis antes de liberar os holds do sono anterior.
  pinMode(16, OUTPUT); digitalWrite(16, HIGH);
  pinMode(18, OUTPUT); digitalWrite(18, HIGH);
  pinMode(19, OUTPUT); digitalWrite(19, HIGH);
  gpio_deep_sleep_hold_dis();
  gpio_hold_dis(GPIO_NUM_16);
  gpio_hold_dis(GPIO_NUM_18);
  gpio_hold_dis(GPIO_NUM_19);
  rtc_gpio_deinit(GPIO_NUM_4);
  pinMode(PIN_PORTA, INPUT_PULLUP);
  pinMode(5, INPUT);
  bool aberta = portaAberta();
  bool mudou = ACORDAR_PELA_TAMPA && retomada && aberta != ultimaPorta;
  bool periodico = agoraUs() >= proximoCiclo;
  if (!retomada || periodico || mudou) {
    etapa("CICLO_INICIO");
    if (LOGS) Serial.printf("CONFIG,intervalo_s=%lu,tampa_wakeup=%d,causa=%d,modo=%s,firmware=sender_modos_v2,regular=%d\n",
      (unsigned long)INTERVALO_SEGUNDOS, ACORDAR_PELA_TAMPA, causa, nomeModo(), retomada);
    if (periodico) {
      // Avanca a grade sem usar fim do envio como inicio do intervalo.
      proximoCiclo += ((agoraUs() - proximoCiclo) / INTERVALO_US + 1) * INTERVALO_US;
    }
    ultimaPorta = aberta;
    etapa(mudou ? "CICLO_TAMPA" : "CICLO_PERIODICO");
    SerialPort.begin(9600, SERIAL_8N1, 17, 16); // RX ESP=17, TX ESP=16.
    uartPronta = true;
    if (!radioConfigurado) {
      etapa("RADIO_CONFIG_INICIO");
      configurarRadio();
      radioConfigurado = true;
      etapa("RADIO_CONFIG_FIM");
    } else if (!radio.begin()) {
      radioConfigurado = false;
      parar("Falha ao iniciar radio apos despertar.");
    }
    // Radio em repouso durante inicializacao e leitura do sensor.
    if (radio.setMode(MODE_3_SLEEP) != 1) {
      radioConfigurado = false;
      parar("Falha ao colocar radio em repouso.");
    }
    Wire.begin(21, 22);
    if (esp_read_mac(data.id_colmeia, ESP_MAC_WIFI_STA) != ESP_OK) parar("Falha ao ler MAC.");
    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
      data.id_colmeia[0], data.id_colmeia[1], data.id_colmeia[2],
      data.id_colmeia[3], data.id_colmeia[4], data.id_colmeia[5]);
    if (prepararDados(aberta)) enviarDados();
    etapa("CICLO_FIM");
  }
  dormir(true);
}
void setup() {
  Serial.begin(115200);
  esp_sleep_wakeup_cause_t causa = esp_sleep_get_wakeup_cause();
  bool retomada = MODO_EXPERIMENTO == COM_DEEP_SLEEP &&
    (causa == ESP_SLEEP_WAKEUP_TIMER || causa == ESP_SLEEP_WAKEUP_EXT0);
  if (!retomada) {
    contador = 0;
    proximoCiclo = agoraUs();
    radioConfigurado = false;
  }
  etapa("BOOT");
  executarCiclo(retomada, (int)causa);
}
void loop() {
  if (MODO_EXPERIMENTO != SEM_DEEP_SLEEP) { delay(1); return; }
  bool mudou = ACORDAR_PELA_TAMPA &&
    ((digitalRead(PIN_PORTA) == NIVEL_TAMPA_ABERTA) != ultimaPorta);
  if (agoraUs() >= proximoCiclo || mudou) {
    // -1 = ciclo em ESP ja ligada; nao e causa de despertar do ESP-IDF.
    executarCiclo(true, -1);
  }
  delay(1);
}
