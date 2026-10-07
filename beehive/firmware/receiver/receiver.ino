/* ESP32 + E32: receptor USB para app.py e coletar_bancada.py.
   Versao reconstruida em 2026-10-07 a partir do receiver local inicial.
   Nao e uma copia exata do firmware anteriormente gravado na bancada.
   Compativel com colmeiaData do sender enviado: exatamente 43 bytes.
   Biblioteca: LoRa_E32 de Renzo Mischianti (xreef).
   Sem Wi-Fi, HTTP, NTP ou armazenamento na ESP32.
   E32 TXD -> ESP GPIO16 (RX); E32 RXD -> ESP GPIO17 (TX).
   AUX -> GPIO5; M0 -> GPIO18; M1 -> GPIO19; GND comum.
   Endereco receiver 0:3; sender 0:1; canal 13; UART 9600/8N1.
   JSON pela USB a 115200 baud; persistencia feita pelo programa no PC.
*/
#include <Arduino.h>
#include <HardwareSerial.h>
#include <LoRa_E32.h>
#include <math.h>
#include <string.h>

HardwareSerial radioSerial(2);
LoRa_E32 radio(&radioSerial, 5, 18, 19); // AUX, M0, M1
const uint8_t HEADER[8] = {'c','o','l','m','e','i','a',0};
uint8_t packet[43];
static_assert(sizeof(float) == 4 && sizeof(int32_t) == 4, "Layout requer valores de 4 bytes");
size_t used = 0;
unsigned long lastByte = 0;

void stopWithError(const char* text) {
  Serial.print("# ERRO: "); Serial.println(text);
  while (true) delay(1000);
}
float readFloat(size_t offset) {
  float value;
  memcpy(&value, packet + offset, 4);
  return value;
}
void printNumber(float value) {
  if (isfinite(value)) Serial.print(value, 2);
  else Serial.print("null");
}
void processPacket() {
  float temp = readFloat(14), humidity = readFloat(18);
  // Validacao basica: o protocolo original nao possui checksum de aplicacao.
  if (!isfinite(temp) || !isfinite(humidity) || temp < -40 || temp > 85 ||
      humidity < 0 || humidity > 100 || packet[42] > 1) {
    Serial.println("# Pacote invalido: verifique sender, sensores e configuracao.");
    return;
  }
  char mac[18];
  snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
           packet[8],packet[9],packet[10],packet[11],packet[12],packet[13]);
  int32_t counter;
  memcpy(&counter, packet + 38, 4);
  float rawPressure = readFloat(26);
  // Sender antigo usa Pa com caixa aberta e hPa com caixa fechada.
  float pressure = rawPressure > 2000 ? rawPressure / 100.0f : rawPressure;
  if (!isfinite(pressure) || pressure < 300 || pressure > 1100 || counter < 1) {
    Serial.println("# Pacote invalido: pressao ou contador fora da faixa.");
    return;
  }
  float battery = readFloat(34);

  // ACK significa recepcao pelo radio, nao gravacao confirmada no computador.
  ResponseStatus ack = radio.sendFixedMessage(0, 1, 13, String(mac));
  Serial.print("{\"type\":\"colmeia\",\"mac\":\""); Serial.print(mac);
  Serial.print("\",\"temperature\":"); printNumber(temp);
  Serial.print(",\"humidity\":"); printNumber(humidity);
  Serial.print(",\"altitude\":"); printNumber(readFloat(22));
  Serial.print(",\"pressure\":"); printNumber(pressure);
  Serial.print(",\"pressure_raw\":"); printNumber(rawPressure);
  Serial.print(",\"weight\":"); printNumber(readFloat(30));
  Serial.print(",\"battery\":");
  printNumber((battery >= 0 && battery <= 100) ? battery : NAN);
  Serial.print(",\"battery_raw\":"); printNumber(battery);
  Serial.print(",\"counter\":"); Serial.print(counter);
  Serial.print(",\"is_open\":"); Serial.print(packet[42] ? "true" : "false");
  Serial.print(",\"ack_ok\":"); Serial.print(ack.code == 1 ? "true" : "false");
  Serial.println("}");
}
void setup() {
  Serial.begin(115200);
  pinMode(18, OUTPUT); digitalWrite(18, LOW);
  pinMode(19, OUTPUT); digitalWrite(19, LOW);
  pinMode(5, INPUT);
  radioSerial.begin(9600, SERIAL_8N1, 16, 17);
  delay(500);
  Serial.print("# Estado de AUX: "); Serial.println(digitalRead(5));
  if (!radio.begin()) stopWithError("Falha em radio.begin(); confira AUX e alimentacao.");
  ResponseStructContainer response = radio.getConfiguration();
  if (response.status.code != 1) {
    Serial.print("# Falha ao consultar E32: ");
    Serial.println(response.status.getResponseDescription());
    // Em algumas versoes da biblioteca, data nao e inicializado em falha.
    // Nao acessar nem liberar o ponteiro nesse caminho. Para ate reset.
    stopWithError("Confira AUX, M0, M1, TX, RX e alimentacao do radio.");
  }
  if (response.data == nullptr) stopWithError("Consulta do E32 retornou sem dados.");
  Configuration cfg = *(Configuration*)response.data;
  response.close();
  if (cfg.HEAD != 0xC0 && cfg.HEAD != 0xC2) stopWithError("Cabecalho da configuracao invalido.");
  cfg.ADDH = 0; cfg.ADDL = 3; cfg.CHAN = 13;
  cfg.SPED.uartBaudRate = UART_BPS_9600;
  cfg.SPED.uartParity = MODE_00_8N1;
  cfg.SPED.airDataRate = AIR_DATA_RATE_010_24;
  cfg.OPTION.fixedTransmission = FT_FIXED_TRANSMISSION;
  cfg.OPTION.fec = FEC_0_OFF;
  cfg.OPTION.ioDriveMode = IO_D_MODE_PUSH_PULLS_PULL_UPS;
  cfg.OPTION.wirelessWakeupTime = WAKE_UP_250;
  cfg.OPTION.transmissionPower = POWER_20;
  // Aplica em RAM a cada boot, sem regravar memoria persistente do radio.
  ResponseStatus status = radio.setConfiguration(cfg, WRITE_CFG_PWR_DWN_LOSE);
  if (status.code != 1) stopWithError("Falha ao configurar o E32.");
  delay(200);
  while (radioSerial.available()) radioSerial.read();
  ResponseStructContainer check = radio.getConfiguration();
  if (check.status.code != 1) stopWithError("Falha ao verificar configuracao do radio.");
  if (check.data == nullptr) stopWithError("Verificacao retornou sem dados.");
  Configuration readback = *(Configuration*)check.data;
  check.close();
  if (readback.ADDH != 0 || readback.ADDL != 3 || readback.CHAN != 13 ||
      readback.SPED.uartBaudRate != UART_BPS_9600 ||
      readback.SPED.uartParity != MODE_00_8N1 ||
      readback.SPED.airDataRate != AIR_DATA_RATE_010_24 ||
      readback.OPTION.fixedTransmission != FT_FIXED_TRANSMISSION ||
      readback.OPTION.fec != FEC_0_OFF) stopWithError("Configuracao relida diverge da solicitada.");
  if (radio.setMode(MODE_0_NORMAL) != 1) stopWithError("Falha ao entrar no modo normal.");
  Serial.println("# Pronto: endereco 0:3, canal 13, USB 115200. Aguardando colmeia.");
}
void loop() {
  if (used && millis() - lastByte > 1000) {
    used = 0;
    Serial.println("# Pacote incompleto descartado.");
  }
  while (radioSerial.available()) {
    packet[used++] = (uint8_t)radioSerial.read();
    lastByte = millis();
    // Busca o cabecalho exato, inclusive o byte zero. Nunca converte binario em String.
    if (used <= sizeof(HEADER)) {
      while (used && memcmp(packet, HEADER, used) != 0) {
        memmove(packet, packet + 1, used - 1);
        --used;
      }
    }
    if (used == sizeof(packet)) {
      processPacket();
      used = 0;
    }
  }
  delay(1);
}
