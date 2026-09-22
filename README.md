# SCoRe 2026 — Energy Experiments

Repositório destinado aos experimentos de eficiência energética em sistemas embarcados e IoT para dois trabalhos científicos candidatos ao SCoRe 2026.

## Artigo 1 — Energy-Aware IoT for Precision Beekeeping

### Título provisório

**Energy-Aware IoT for Precision Beekeeping: An Experimental Evaluation of Sensing and Communication Strategies**

### Objetivo

Avaliar experimentalmente como estratégias de sensoriamento, comunicação e gerenciamento de energia afetam o consumo de um nó IoT destinado ao monitoramento de colmeias.

### Questões de pesquisa

**RQ1.** Como a energia consumida pelo nó se distribui entre sensoriamento, processamento, conexão Wi-Fi, transmissão e espera/deep sleep?

**RQ2.** Como diferentes intervalos de aquisição e transmissão afetam energia por ciclo, consumo diário e autonomia estimada?

**RQ3.** Uma estratégia adaptativa de aquisição/transmissão pode reduzir o consumo energético mantendo as alterações relevantes presentes nos dados monitorados?

### Condições experimentais previstas

- operação contínua;
- intervalo de 1 minuto;
- intervalo de 5 minutos;
- intervalo de 15 minutos;
- intervalo de 30 minutos;
- intervalo de 60 minutos;
- estratégia adaptativa.

### Principais métricas

- tensão;
- corrente;
- potência;
- energia por ciclo;
- energia de sensoriamento;
- energia de conexão Wi-Fi;
- energia de transmissão;
- energia em deep sleep;
- tempo ativo;
- consumo diário estimado;
- autonomia estimada;
- número de transmissões;
- preservação da informação.

---

## Artigo 2 — Compute or Communicate?

### Título provisório

**Compute or Communicate? Energy and Performance Trade-offs of Tiny Language Model Inference on ESP32-S3**

### Objetivo

Caracterizar experimentalmente o custo energético e temporal da execução local do Needle 2 em ESP32-S3 e comparar, do ponto de vista do dispositivo Edge, a inferência local com o envio da mesma tarefa para processamento remoto.

### Questões de pesquisa

**RQ1.** Qual é o custo energético e temporal da inferência local do Needle 2 no ESP32-S3?

**RQ2.** Como características da entrada influenciam latência e energia consumida?

**RQ3.** Qual é o trade-off de energia e latência entre computação local e comunicação para processamento remoto?

### Cenários

#### LOCAL

Prompt → ESP32-S3 → Needle 2 → Tool Call

#### REMOTO

Prompt → ESP32-S3 → Wi-Fi → Serviço remoto → Resposta → ESP32-S3

A comparação energética do cenário remoto considera somente o consumo observado no dispositivo Edge. O consumo energético da infraestrutura remota não será estimado como parte dessa comparação.

### Principais métricas

- latência;
- tensão;
- corrente média;
- pico de corrente observado;
- potência;
- energia por inferência;
- Energy-Delay Product (EDP);
- tamanho da entrada;
- tamanho da saída;
- RSSI;
- bytes transmitidos e recebidos, quando disponíveis;
- sucesso da execução.

---

# Instrumentação

A medição será realizada com:

- INA219 ou INA226;
- microcontrolador auxiliar dedicado à aquisição;
- fonte de alimentação controlada;
- computador para armazenamento dos dados.

Arquitetura básica:

Fonte → INA219/INA226 → Dispositivo sob teste

O INA219/INA226 será lido preferencialmente por um microcontrolador independente para evitar que a instrumentação altere a carga computacional do dispositivo avaliado.

---

# Metodologia geral

## Piloto

Nenhuma coleta oficial deverá começar antes da validação do sistema de medição.

Inicialmente serão realizadas 5 repetições de cada condição.

Os dados piloto serão avaliados quanto a:

- estabilidade;
- sincronização;
- valores ausentes;
- frequência real de aquisição;
- repetibilidade;
- valores fisicamente plausíveis.

Somente depois da validação serão realizadas as séries experimentais oficiais.

## Repetições

A configuração inicial prevista é de 30 repetições válidas por condição experimental.

Esse número poderá ser revisado após análise dos experimentos piloto.

---

# Organização do repositório

    hardware/
        wiring/
        photos/
        specifications/

    energy-meter/
        firmware/
        calibration/

    beehive/
        firmware/
        raw/
        processed/
        logs/
        scripts/

    needle2/
        firmware/
        prompts/
        local/
            raw/
            logs/
        remote/
            raw/
            logs/

    analysis/
        common/
        beehive/
        needle2/

    docs/
        experiment_log.md

---

# Regras de integridade experimental

1. Arquivos armazenados em `raw/` são imutáveis.
2. Nunca editar manualmente dados brutos.
3. Nunca apagar uma execução por apresentar resultado inesperado.
4. Execuções inválidas devem ser mantidas e acompanhadas da justificativa.
5. Dados tratados devem ser gravados em `processed/`.
6. Alterações de firmware devem gerar novo commit.
7. Registrar hardware, alimentação e configuração utilizada.
8. Não alterar a montagem durante uma série experimental sem documentar.
9. Registrar problemas e decisões no diário experimental.
10. Preservar os logs e saídas originais.
11. Fotografar a montagem utilizada nos experimentos.
12. Registrar a versão/commit dos códigos utilizados na coleta.

---

# Status

- [x] Definição inicial das duas propostas
- [x] Definição das questões de pesquisa
- [x] Estrutura inicial do repositório
- [ ] Montagem do medidor de energia
- [ ] Firmware do INA219/INA226
- [ ] Validação do sistema de medição
- [ ] Piloto Needle 2 local
- [ ] Piloto Needle 2 remoto
- [ ] Piloto BeeWEB
- [ ] Validação dos pilotos
- [ ] Coleta oficial
- [ ] Análise estatística
- [ ] Figuras e tabelas
- [ ] Redação dos artigos
