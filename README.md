# STR M3 – Huffman: tempo de resposta, HWM e (m,k)-firme (ESP-IDF / ESP32)

Mede o tempo de resposta da tarefa de **codificação de Huffman** em quatro escopos
de teste com estresse crescente e produz todas as análises do enunciado.

```
main/huffman.[ch]     tarefa sob teste (C puro, sem malloc, valida a saída por round-trip)
main/main.c           firmware: etapas 1-4, instrumentação com clock_gettime(CLOCK_MONOTONIC)
host/                 Experimento A: casos de teste + cobertura gcov (roda no PC)
tools/capture.py      captura a serial e gera data/etapaN.csv (ativacao,tempo_us) + meta.json
analysis/analyze.py   tabela-síntese, histogramas, gráfico temporal, convergência, janela (m,k), exp. B/C/D
```

## Reprodução

Requisitos: ESP-IDF 5.x (testado contra a API 5.x), uma placa ESP32 (dual-core), Python 3 com `pip install -r tools/requirements.txt`.

```bash
# 1) Firmware (sdkconfig.defaults já fixa 240 MHz, tick 1 ms, sem PM, UART 921600)
idf.py set-target esp32
idf.py build flash

# 2) Captura (reinicia a placa e grava data/etapa1..4.csv; leva ~6 min)
python tools/capture.py --port /dev/ttyUSB0 --baud 921600 --out data
#    alternativa: idf.py monitor -b 921600 | tee log.txt   e depois:
#    python tools/capture.py --log log.txt --out data

# 3) Experimento A (PC): cobertura de linha/desvio com gcov
./host/run_coverage.sh

# 4) Análises e figuras
python analysis/analyze.py --data data --out results --cov-line 100 --cov-branch 100 --mk-required 18,20
```

Parâmetros (tamanho do array, semente, nº de ativações, períodos, prioridades) ficam no
topo de `main/main.c`. **Valores atuais:** array = 2048 bytes, semente = 123456789,
D = ⌈HWM(100%) da etapa 1 × 1,2⌉, ativações = 10000 / 2000 / 5000 / 10000 (etapas 1–4).

## Como cada etapa mede

Tempo de resposta = conclusão − liberação. Nas etapas 2–4 a liberação é carimbada
**dentro da ISR** de um timer de hardware (`esp_timer` com `ESP_TIMER_ISR`), então o
valor inclui latência de interrupção, espera por tarefas de maior prioridade, bloqueio em
mutex e execução. Na etapa 1 mede-se só a execução da função, como no exemplo da aula.
Todas as tarefas de tempo real rodam no núcleo 0 (para forçar disputa).

| Etapa | Caso de teste | Procedimento | Critério |
|---|---|---|---|
| 1 Tarefa | Entrada: 2048 bytes aleatórios novos a cada ativação (xorshift32, semente fixa). Restrição: sistema ocioso entre ativações (5 ms). Esperado: decodificação == entrada. | Task isolada: espera → `clock_gettime` → encode+decode+compara → `clock_gettime`. 10000 ativações. | Validação 100% OK (`huff_fail=0`); HWM(100%) define **D = 1,2 × HWM**. |
| 2 Comportamental | Um evento externo por vez (ISR de timer a cada 10 ms, bem maior que a execução, logo sem sobreposição). | ISR carimba o instante e acorda a Huffman; resposta = fim − carimbo da ISR. 2000 eventos. | Resposta ≤ D; mede o custo de latência de ISR + escalonamento. |
| 3 Intertarefas | `sensor` (prio 7, 7 ms), `logger` (prio 6, 13 ms), `huffman` (prio 5, 20 ms) com mutex (herança de prioridade) e taxas de dados diferentes: sensor escreve, logger lê e a Huffman copia 64 B do buffer sob o mesmo mutex. | Períodos 7/13/20 ms (coincidem raramente: pior caso é o instante crítico). Resposta da Huffman, 5000 ativações. | Resposta ≤ D; quantificar perdas, S e (m,k). |
| 4 Sistema | Etapa 3 + carga externa (abaixo). | Mesmo procedimento, 10000 ativações. | Resposta ≤ D; o critério é tentar fazer o sistema perder o prazo. |

### Cenário de estresse da etapa 4 (justificativa)
O pior caso de um STR vem de **concorrência de hardware e interrupções**, não de caminhos de código. Por isso a carga ataca recursos distintos:

1. **Tempestade de interrupções** – ISR de 40 µs a cada 700 µs no núcleo 0 (~6%): rouba CPU até da tarefa mais prioritária.
2. **Tráfego de memória/flash** – tarefa no núcleo 1 varrendo 128 KB de flash (cache miss, barramento SPI) e copiando 32 KB de RAM: disputa de barramento/cache com o núcleo 0.
3. **Rajadas de CPU** – tarefa de prioridade 6 que consome 0,3–2,5 ms em instantes aleatórios: interferência não periódica acima da Huffman.
4. **I/O na UART** – linha a cada 25 ms (prefixo `#`, ignorada na captura): disputa o lock do console e gera interrupções de UART.

## Experimentos
- **A (cobertura × HWM)**: `host/run_coverage.sh` compila `huffman.c` com `--coverage`, roda 9 casos dirigidos (vazio, 1 símbolo, 2 símbolos, 256 iguais, Fibonacci, aleatório, overflow, decode truncado, injeção de falha) e mostra gcov. Em seguida roda 10000 execuções aleatórias e imprime o HWM acumulado: **a cobertura satura em 100% e o HWM continua subindo**. Compare com o HWM das etapas 2–4 (`results/resultados.md`). A cobertura é medida no PC porque gcov no ESP32 exige app-trace; o código-fonte medido é o mesmo `huffman.c` do firmware.
- **B**: HWM acumulado × amostras (10, 20, 100, 1000, 10000) – tabela e `fig_convergencia_hwm.png`.
- **C**: margens de 10/25/50% sobre o HWM da etapa 1 contra o HWM de cada etapa.
- **D**: 3 valores de D nas etapas 3 e 4 (HWM(100%) etapa 1, HWM(99%) etapa 4 e o ponto médio), com % no prazo, S, (m,k) k=20, perdas isoladas × rajadas, e menor D que satisfaz `--mk-required`.

## Limitações (para a seção "Ameaças à validade")
- O (m,k) medido descreve o comportamento **observado**; não é garantia formal. Outra carga, temperatura ou coincidência de liberações não exercitada pode produzir janela pior.
- Plataforma de propósito geral: caches, flash compartilhada entre núcleos, ISRs do SDK e estado de energia afetam as medidas. `print_platform()` registra clock, núcleos, tick, governor e o offset entre `clock_gettime` e `esp_timer` (deve ser ~0).
- A instrumentação (`clock_gettime`, escrita do vetor) perturba pouco, mas não é zero; a impressão do CSV ocorre só **depois** de cada etapa.
- Nas ISRs usa-se `esp_timer_get_time()` (mesma base do `CLOCK_MONOTONIC` do IDF) por ser segura em interrupção.
- Resolução do relógio: tempos gravados em µs inteiros.
- Para a opcional segunda plataforma, basta recompilar com outro target (`idf.py set-target esp32s3`/`esp32c3`; em single-core, tudo roda no núcleo 0) ou habilitar outro SO.
