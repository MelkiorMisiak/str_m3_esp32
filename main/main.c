/*
 * main.c - STR M3: "Quanto o teste enxerga do pior caso?"  (ESP-IDF / FreeRTOS)
 *
 * Mede o tempo de resposta da tarefa Huffman em quatro escopos de teste, com
 * estresse crescente, e envia as amostras pela serial em CSV:
 *
 *   Etapa 1  Tarefa          : Huffman isolado (so o tempo de execucao).
 *   Etapa 2  Comportamental  : Huffman reagindo a UM evento externo por vez
 *                              (interrupcao simulada por timer de hardware).
 *   Etapa 3  Intertarefas    : 3 tarefas periodicas (sensor, logger, Huffman),
 *                              mutex com heranca de prioridade, preempcao.
 *   Etapa 4  Sistema         : Etapa 3 + carga externa (tempestade de
 *                              interrupcoes, trafego de memoria/flash no outro
 *                              nucleo, rajadas de CPU, I/O na UART).
 *
 * Tempo de resposta = instante de CONCLUSAO - instante de LIBERACAO (release).
 * Nas etapas 2-4 a liberacao e carimbada dentro da ISR do timer, antes de
 * qualquer escalonamento, portanto o tempo inclui latencia de interrupcao,
 * espera por tarefas mais prioritarias, bloqueio em mutex e a execucao.
 * Na etapa 1 so se mede a execucao da funcao (como no exemplo da aula).
 *
 * O deadline D e definido UMA vez: D = ceil(HWM(100%) da etapa 1 * D_FACTOR)
 * e vale para as quatro etapas.
 *
 * Saida serial (linhas iniciadas por '#' sao comentarios/metadados):
 *   #META chave=valor ...
 *   #BEGIN stage=N n=... D_us=...
 *   ativacao,tempo_us
 *   0,2301
 *   ...
 *   #END stage=N
 *   #ALLDONE
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>
#include <math.h>
#include <time.h>

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_idf_version.h"

#include "huffman.h"

/* ====================================================================== */
/* Configuracao do experimento (registre estes valores no relatorio)       */
/* ====================================================================== */
#define HUFF_N            2048          /* tamanho do array de caracteres       */
#define HUFF_SEED         123456789u    /* semente do gerador aleatorio         */
#define D_FACTOR          1.2           /* D = HWM(100%) da etapa 1 * D_FACTOR  */

#define S1_ACT            10000         /* ativacoes por etapa (>= 1000)        */
#define S2_ACT            2000
#define S3_ACT            5000
#define S4_ACT            10000         /* >= 10000 p/ experimento B            */
#define MAX_ACT           10000         /* maior das contagens acima            */

#define S1_PERIOD_MS      5             /* intervalo entre ativacoes (etapa 1)  */
#define S2_PERIOD_US      10000         /* intervalo entre eventos  (etapa 2)   */
#define HUFF_PERIOD_US    20000         /* periodo da Huffman (etapas 3 e 4)    */
#define SENSOR_PERIOD_US  7000          /* periodos "primos entre si" para que  */
#define LOGGER_PERIOD_US  13000         /* as liberacoes coincidam raramente    */

#define PRIO_SENSOR       7             /* maior prioridade                     */
#define PRIO_LOGGER       6
#define PRIO_HUFF         5
#define PRIO_BURST        6             /* carga da etapa 4                     */
#define PRIO_UARTLOAD     4
#define PRIO_HOG          3

#define RT_CORE           0             /* nucleo das tarefas de tempo real     */
#define LOAD_CORE         1             /* nucleo da carga de memoria (etapa 4) */
#define STACK_BYTES       4096

#define SHARED_LEN        64            /* area compartilhada protegida por mutex */
#define STORM_PERIOD_US   700           /* ISR "tempestade": periodo            */
#define STORM_BURN_US     40            /* ... e trabalho por interrupcao       */
#define FLASH_BLOB_SZ     (128 * 1024)  /* bloco em flash lido pela carga       */
#define HOG_SZ            (32 * 1024)

#define REL_RING          32            /* fila de instantes de liberacao       */

/* ====================================================================== */
/* Tempo                                                                    */
/* ====================================================================== */

/* clock_gettime(CLOCK_MONOTONIC): relogio exigido pelo enunciado. */
static inline uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static inline uint64_t now_us(void) { return now_ns() / 1000u; }

/* clock_gettime(CLOCK_MONOTONIC) e esp_timer_get_time() tem a mesma taxa, mas
 * ORIGENS diferentes em algumas versoes do IDF (observado: ~73 ms no IDF 6.x).
 * Como a liberacao e carimbada na ISR com esp_timer e o fim com clock_gettime,
 * subtraimos este offset (medido uma vez, no inicio) de now_us() nas etapas 2-4. */
static int64_t g_clk_offset_us;
static int64_t g_clk_spread_us;   /* incerteza da calibracao (menor janela observada) */

static void calibrate_clock_offset(void)
{
    int64_t best = INT64_MAX, off = 0;
    for (int i = 0; i < 200; i++) {
        int64_t a = esp_timer_get_time();
        int64_t b = (int64_t)now_us();
        int64_t c = esp_timer_get_time();
        if (c - a < best) {            /* janela mais curta = melhor estimativa */
            best = c - a;
            off = b - (a + c) / 2;
        }
    }
    g_clk_offset_us = off;
    g_clk_spread_us = best;
}

/* ====================================================================== */
/* Gerador pseudo-aleatorio (xorshift32): deterministico e sem estado global */
/* ====================================================================== */
static inline uint32_t xorshift32(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return *s = x;
}

/* ====================================================================== */
/* Carga de CPU calibrada (nao depende de espera por relogio, entao se a    */
/* tarefa for preemptada o tempo de CPU consumido continua o mesmo)         */
/* ====================================================================== */
static volatile uint32_t g_sink;
static uint32_t g_iters_per_us = 1;

static void burn_iters(uint32_t n)
{
    uint32_t a = g_sink;
    for (uint32_t i = 0; i < n; i++) {
        a = a * 1664525u + 1013904223u;
    }
    g_sink = a;
}
static void burn_us(uint32_t us) { burn_iters(us * g_iters_per_us); }

static void calibrate_burn(void)
{
    const uint32_t probe = 400000;
    burn_iters(1000);                                   /* aquece o cache */
    uint64_t t0 = now_ns();
    burn_iters(probe);
    uint64_t dt_us = (now_ns() - t0) / 1000u;
    g_iters_per_us = dt_us ? (uint32_t)(probe / dt_us) : 1;
    if (g_iters_per_us == 0) g_iters_per_us = 1;
}

/* ====================================================================== */
/* Tarefa sob teste: Huffman                                                */
/* ====================================================================== */
static huff_ctx_t g_ctx;
static uint8_t    g_in[HUFF_N];
static uint8_t    g_out[HUFF_N];
static uint8_t    g_bits[HUFF_N * 2];
static uint32_t   g_rng = HUFF_SEED;
static volatile uint32_t g_huff_fail;     /* ativacoes cuja validacao falhou */

static uint32_t g_resp[MAX_ACT];          /* tempos de resposta (us)         */

/* Area compartilhada (etapas 3 e 4) e seu mutex. NULL nas etapas 1 e 2. */
static SemaphoreHandle_t g_mux;
static uint8_t           g_shared[SHARED_LEN];

/* Gera um novo array aleatorio. Fica FORA da janela cronometrada. */
static void huff_regen(void)
{
    for (int i = 0; i < HUFF_N; i++) {
        g_in[i] = (uint8_t)xorshift32(&g_rng);
    }
}

/* Corpo cronometrado da tarefa: [le dado compartilhado] + encode + decode +
 * comparacao com a entrada (validacao logica, teste de tarefa). */
static void huff_job(void)
{
    if (g_mux) {
        /* Segura o mutex enquanto copia o dado do sensor. O sensor (prio mais
         * alta) pode ficar bloqueado aqui: dispara heranca de prioridade. */
        xSemaphoreTake(g_mux, portMAX_DELAY);
        memcpy(g_in, g_shared, SHARED_LEN);
        burn_us(100);
        xSemaphoreGive(g_mux);
    }
    if (huffman_roundtrip(&g_ctx, g_in, HUFF_N) != HUFF_OK) {
        g_huff_fail++;
    }
}

/* ====================================================================== */
/* Infraestrutura de tarefas liberadas por timer de hardware (etapas 2-4)    */
/* ====================================================================== */
typedef struct rt_task {
    const char        *name;
    UBaseType_t        prio;
    BaseType_t         core;
    uint32_t           period_us;
    void             (*work)(void);        /* corpo do job (cronometrado)       */
    void             (*post)(void);        /* apos o carimbo de fim (nao conta) */
    uint32_t           target;             /* ativacoes a medir (0 = nao mede)  */
    uint32_t          *resp;               /* vetor de saida                    */
    TaskHandle_t       handle;
    esp_timer_handle_t timer;
    SemaphoreHandle_t  done, reached;
    volatile uint64_t  rel[REL_RING];      /* instantes de liberacao (us)       */
    volatile uint32_t  released;           /* liberacoes ocorridas (ISR)        */
    uint32_t           completed;          /* jobs concluidos (tarefa)          */
    volatile bool      stop;
} rt_task_t;

/* ISR do timer: carimba o instante de liberacao e acorda a tarefa.
 * Usa esp_timer_get_time(), a mesma base de tempo do CLOCK_MONOTONIC do IDF
 * (verificado em print_platform), mas segura para ISR. */
static void IRAM_ATTR release_isr(void *arg)
{
    rt_task_t *t = (rt_task_t *)arg;
    uint32_t r = t->released;
    t->rel[r % REL_RING] = (uint64_t)esp_timer_get_time();
    t->released = r + 1;
    BaseType_t hp = pdFALSE;
    vTaskNotifyGiveFromISR(t->handle, &hp);
    if (hp) {
        portYIELD_FROM_ISR();
    }
}

/* Loop da tarefa: espera liberacao -> executa -> mede resposta -> pos-processa. */
static void rt_body(void *arg)
{
    rt_task_t *t = (rt_task_t *)arg;
    for (;;) {
        ulTaskNotifyTake(pdFALSE, portMAX_DELAY);   /* contador: uma liberacao por vez */
        if (t->stop) {
            break;
        }
        uint32_t k = t->completed;
        t->work();
        /* clock_gettime(CLOCK_MONOTONIC), na mesma origem do carimbo da ISR */
        uint64_t end = (uint64_t)((int64_t)now_us() - g_clk_offset_us);
        if (t->resp && k < t->target) {
            t->resp[k] = (uint32_t)(end - t->rel[k % REL_RING]);
            if (k + 1 == t->target) {
                xSemaphoreGive(t->reached);
            }
        }
        t->completed = k + 1;
        if (t->post) {
            t->post();
        }
    }
    xSemaphoreGive(t->done);
    vTaskDelete(NULL);
}

static void rt_init(rt_task_t *t, const char *name, UBaseType_t prio, uint32_t period_us,
                    void (*work)(void), void (*post)(void), uint32_t target, uint32_t *resp)
{
    memset((void *)t, 0, sizeof(*t));
    t->name = name; t->prio = prio; t->core = RT_CORE; t->period_us = period_us;
    t->work = work; t->post = post; t->target = target; t->resp = resp;
    t->done = xSemaphoreCreateBinary();
    t->reached = xSemaphoreCreateBinary();
}

static void rt_create(rt_task_t *t)
{
    xTaskCreatePinnedToCore(rt_body, t->name, STACK_BYTES, t, t->prio, &t->handle, t->core);
}

static void rt_arm(rt_task_t *t)
{
    const esp_timer_create_args_t args = {
        .callback = release_isr,
        .arg = t,
        .dispatch_method = ESP_TIMER_ISR,      /* callback roda em interrupcao */
        .name = t->name,
    };
    ESP_ERROR_CHECK(esp_timer_create(&args, &t->timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(t->timer, t->period_us));
}

static void rt_disarm(rt_task_t *t)
{
    esp_timer_stop(t->timer);
    esp_timer_delete(t->timer);
}

static void rt_halt(rt_task_t *t)
{
    t->stop = true;
    xTaskNotifyGive(t->handle);
    xSemaphoreTake(t->done, portMAX_DELAY);
    vSemaphoreDelete(t->done);
    vSemaphoreDelete(t->reached);
}

/* Executa um grupo de tarefas ate a tarefa medida atingir suas ativacoes. */
static void run_group(rt_task_t **tk, int n, rt_task_t *measured,
                      void (*extra_start)(void), void (*extra_stop)(void))
{
    for (int i = 0; i < n; i++) rt_create(tk[i]);
    if (extra_start) extra_start();
    for (int i = 0; i < n; i++) rt_arm(tk[i]);      /* liberacoes quase simultaneas */

    xSemaphoreTake(measured->reached, portMAX_DELAY);

    for (int i = 0; i < n; i++) rt_disarm(tk[i]);   /* primeiro todos os timers... */
    if (extra_stop) extra_stop();
    for (int i = 0; i < n; i++) rt_halt(tk[i]);     /* ...depois as tarefas        */
}

/* ====================================================================== */
/* Corpos das demais tarefas (etapa 3)                                      */
/* ====================================================================== */
static uint32_t g_rng_sensor = 0xC0FFEEu;

/* Sensor (alta prioridade, 7 ms): atualiza o buffer compartilhado sob mutex. */
static void sensor_work(void)
{
    xSemaphoreTake(g_mux, portMAX_DELAY);
    for (int i = 0; i < SHARED_LEN; i++) g_shared[i] = (uint8_t)xorshift32(&g_rng_sensor);
    burn_us(150);                    /* secao critica */
    xSemaphoreGive(g_mux);
    burn_us(250);                    /* processamento fora da secao critica */
}

/* Logger/filtro (media prioridade, 13 ms): le o buffer sob mutex e processa. */
static void logger_work(void)
{
    uint8_t local[SHARED_LEN];
    xSemaphoreTake(g_mux, portMAX_DELAY);
    memcpy(local, g_shared, SHARED_LEN);
    burn_us(300);
    xSemaphoreGive(g_mux);
    burn_us(1200);                   /* filtragem/CRC */
    g_sink += local[0];
}

/* ====================================================================== */
/* Carga externa da etapa 4                                                 */
/* ====================================================================== */
static volatile bool g_load_run;
static SemaphoreHandle_t g_load_done;
static esp_timer_handle_t g_storm_timer;
static uint32_t g_storm_iters;
static volatile uint32_t g_storm_sink;

/* (1) Tempestade de interrupcoes: ISR de 40 us a cada 700 us (~6% do nucleo 0),
 *     rouba CPU de qualquer tarefa, inclusive da de maior prioridade. */
static void IRAM_ATTR storm_isr(void *arg)
{
    (void)arg;
    uint32_t a = g_storm_sink;
    for (uint32_t i = 0; i < g_storm_iters; i++) {
        a = a * 1664525u + 1013904223u;
    }
    g_storm_sink = a;
}

/* (2) Trafego de memoria: no outro nucleo, varre um bloco de 128 KB em flash
 *     (cache miss + disputa do barramento SPI de flash) e copia 32 KB de RAM. */
static const uint8_t k_flash_blob[FLASH_BLOB_SZ] = { 1, 2, 3 };
static uint8_t g_hog_a[HOG_SZ], g_hog_b[HOG_SZ];

static void hog_task(void *arg)
{
    (void)arg;
    uint32_t acc = 0;
    while (g_load_run) {
        const volatile uint8_t *p = k_flash_blob;
        for (uint32_t i = 0; i < FLASH_BLOB_SZ; i += 32) acc += p[i];
        memcpy(g_hog_a, g_hog_b, HOG_SZ);
        g_hog_b[0] = (uint8_t)acc;
        vTaskDelay(1);
    }
    xSemaphoreGive(g_load_done);
    vTaskDelete(NULL);
}

/* (3) Rajadas de CPU: a cada 5-40 ms consome de 0,3 a 2,5 ms, com prioridade
 *     acima da Huffman. Gera interferencia irregular (nao periodica). */
static void burst_task(void *arg)
{
    (void)arg;
    uint32_t rng = 0xBADC0DEu;
    while (g_load_run) {
        vTaskDelay(pdMS_TO_TICKS(5 + xorshift32(&rng) % 36));
        burn_us(300 + xorshift32(&rng) % 2200);
    }
    xSemaphoreGive(g_load_done);
    vTaskDelete(NULL);
}

/* (4) I/O na UART: uma linha a cada 25 ms (comentario '#', ignorado pelo
 *     script de captura). Disputa o lock do console e gera interrupcoes de UART. */
static void uartload_task(void *arg)
{
    (void)arg;
    uint32_t k = 0;
    while (g_load_run) {
        printf("#LOAD uart_k=%" PRIu32 " 0123456789012345678901234567890123456789\n", k++);
        vTaskDelay(pdMS_TO_TICKS(25));
    }
    xSemaphoreGive(g_load_done);
    vTaskDelete(NULL);
}

static void load_start(void)
{
    g_load_run = true;
    g_load_done = xSemaphoreCreateCounting(3, 0);
    g_storm_iters = g_iters_per_us * STORM_BURN_US;
    const esp_timer_create_args_t a = {
        .callback = storm_isr, .arg = NULL,
        .dispatch_method = ESP_TIMER_ISR, .name = "storm",
    };
    ESP_ERROR_CHECK(esp_timer_create(&a, &g_storm_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(g_storm_timer, STORM_PERIOD_US));
    xTaskCreatePinnedToCore(hog_task, "hog", STACK_BYTES, NULL, PRIO_HOG, NULL,
                            portNUM_PROCESSORS > 1 ? LOAD_CORE : RT_CORE);
    xTaskCreatePinnedToCore(burst_task, "burst", STACK_BYTES, NULL, PRIO_BURST, NULL, RT_CORE);
    xTaskCreatePinnedToCore(uartload_task, "uartload", STACK_BYTES, NULL, PRIO_UARTLOAD, NULL, RT_CORE);
}

static void load_stop(void)
{
    esp_timer_stop(g_storm_timer);
    esp_timer_delete(g_storm_timer);
    g_load_run = false;
    for (int i = 0; i < 3; i++) xSemaphoreTake(g_load_done, portMAX_DELAY);
    vSemaphoreDelete(g_load_done);
}

/* ====================================================================== */
/* Etapa 1: tarefa isolada (so tempo de execucao)                           */
/* ====================================================================== */
static SemaphoreHandle_t g_s1_done;

static void s1_task(void *arg)
{
    (void)arg;
    huff_regen();
    for (uint32_t k = 0; k < S1_ACT; k++) {
        vTaskDelay(pdMS_TO_TICKS(S1_PERIOD_MS));     /* sistema ocioso entre ativacoes */
        uint64_t t0 = now_ns();
        huff_job();
        uint64_t t1 = now_ns();
        g_resp[k] = (uint32_t)((t1 - t0) / 1000u);
        huff_regen();                                /* fora da medicao */
    }
    xSemaphoreGive(g_s1_done);
    vTaskDelete(NULL);
}

static void run_stage1(void)
{
    g_mux = NULL;
    g_s1_done = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(s1_task, "s1_huff", STACK_BYTES, NULL, PRIO_HUFF, NULL, RT_CORE);
    xSemaphoreTake(g_s1_done, portMAX_DELAY);
    vSemaphoreDelete(g_s1_done);
}

/* ====================================================================== */
/* Etapa 2: comportamental (um evento externo por vez)                      */
/* ====================================================================== */
static void huff_post(void) { huff_regen(); }

static void run_stage2(void)
{
    static rt_task_t huff;
    g_mux = NULL;
    huff_regen();
    rt_init(&huff, "s2_huff", PRIO_HUFF, S2_PERIOD_US, huff_job, huff_post, S2_ACT, g_resp);
    rt_task_t *tk[] = { &huff };
    run_group(tk, 1, &huff, NULL, NULL);
}

/* ====================================================================== */
/* Etapa 3: intertarefas (3 tarefas periodicas + mutex + preempcao)         */
/* ====================================================================== */
static void run_stage3_or_4(bool with_load)
{
    static rt_task_t sensor, logger, huff;
    g_mux = xSemaphoreCreateMutex();                 /* mutex com heranca de prioridade */
    memset(g_shared, 0, sizeof(g_shared));
    huff_regen();

    rt_init(&sensor, "sensor", PRIO_SENSOR, SENSOR_PERIOD_US, sensor_work, NULL, 0, NULL);
    rt_init(&logger, "logger", PRIO_LOGGER, LOGGER_PERIOD_US, logger_work, NULL, 0, NULL);
    rt_init(&huff, with_load ? "s4_huff" : "s3_huff", PRIO_HUFF, HUFF_PERIOD_US,
            huff_job, huff_post, with_load ? S4_ACT : S3_ACT, g_resp);
    rt_task_t *tk[] = { &sensor, &logger, &huff };

    run_group(tk, 3, &huff, with_load ? load_start : NULL, with_load ? load_stop : NULL);

    vSemaphoreDelete(g_mux);
    g_mux = NULL;
}

/* ====================================================================== */
/* Saida de dados e resumo                                                  */
/* ====================================================================== */
static void print_summary(int stage, const uint32_t *r, uint32_t n, uint32_t D)
{
    uint32_t mn = UINT32_MAX, mx = 0, miss = 0;
    uint64_t sum = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (r[i] < mn) mn = r[i];
        if (r[i] > mx) mx = r[i];
        if (r[i] > D) miss++;
        sum += r[i];
    }
    printf("#SUMMARY stage=%d n=%" PRIu32 " min_us=%" PRIu32 " mean_us=%" PRIu32
           " hwm_us=%" PRIu32 " D_us=%" PRIu32 " misses=%" PRIu32 " huff_fail=%" PRIu32 "\n",
           stage, n, mn, (uint32_t)(sum / n), mx, D, miss, g_huff_fail);
}

/* Imprime o CSV da etapa (ativacao,tempo_us). Cede a CPU periodicamente para
 * nao starvar a tarefa ociosa (a UART do console opera por polling). */
static void dump_stage(int stage, const uint32_t *r, uint32_t n, uint32_t D)
{
    printf("#BEGIN stage=%d n=%" PRIu32 " D_us=%" PRIu32 "\n", stage, n, D);
    printf("ativacao,tempo_us\n");
    for (uint32_t i = 0; i < n; i++) {
        printf("%" PRIu32 ",%" PRIu32 "\n", i, r[i]);
        if ((i & 0x7F) == 0x7F) vTaskDelay(1);
    }
    printf("#END stage=%d\n", stage);
    print_summary(stage, r, n, D);
    fflush(stdout);
}

static uint32_t max_of(const uint32_t *r, uint32_t n)
{
    uint32_t m = 0;
    for (uint32_t i = 0; i < n; i++) if (r[i] > m) m = r[i];
    return m;
}

/* Metadados da plataforma (item obrigatorio do relatorio). */
static void print_platform(void)
{
    printf("#META target=%s idf=%s cores=%d cpu_mhz=%d tick_hz=%d\n",
           CONFIG_IDF_TARGET, esp_get_idf_version(), (int)portNUM_PROCESSORS,
           CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ, (int)configTICK_RATE_HZ);
#ifdef CONFIG_PM_ENABLE
    printf("#META power_governor=PM_ENABLED(DFS/light_sleep_possivel)\n");
#else
    printf("#META power_governor=fixo_sem_DFS_(CONFIG_PM_ENABLE=n)\n");
#endif
    printf("#META programas_em_execucao=apenas_FreeRTOS_idle+esp_timer+este_firmware(sem_WiFi/BT)\n");
    printf("#META huff_n=%d seed=%u d_factor=%.2f rt_core=%d load_core=%d\n",
           HUFF_N, (unsigned)HUFF_SEED, D_FACTOR, RT_CORE, LOAD_CORE);
    printf("#META act_s1=%d act_s2=%d act_s3=%d act_s4=%d\n", S1_ACT, S2_ACT, S3_ACT, S4_ACT);
    printf("#META periodos_us s1=%d s2=%d huff=%d sensor=%d logger=%d\n",
           S1_PERIOD_MS * 1000, S2_PERIOD_US, HUFF_PERIOD_US, SENSOR_PERIOD_US, LOGGER_PERIOD_US);
    printf("#META burn_iters_per_us=%" PRIu32 " clock_offset_us=%" PRId64
           " clock_calib_window_us=%" PRId64 " (offset removido de clock_gettime nas etapas 2-4)\n",
           g_iters_per_us, g_clk_offset_us, g_clk_spread_us);
}

/* ====================================================================== */
/* app_main: executa as quatro etapas em sequencia                          */
/* ====================================================================== */
void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(3000));      /* tempo para o script de captura abrir a serial */

    huffman_init(&g_ctx, g_bits, sizeof(g_bits), g_out);
    calibrate_burn();
    calibrate_clock_offset();
    print_platform();

    /* ---- Etapa 1: define o deadline D (uma unica vez) ---- */
    printf("#STAGE 1 start\n");
    run_stage1();
    uint32_t hwm1 = max_of(g_resp, S1_ACT);
    uint32_t D = (uint32_t)ceil((double)hwm1 * D_FACTOR);
    printf("#META hwm1_us=%" PRIu32 " D_us=%" PRIu32 " (D = HWM1 * %.2f)\n", hwm1, D, D_FACTOR);
    dump_stage(1, g_resp, S1_ACT, D);

    /* ---- Etapa 2 ---- */
    printf("#STAGE 2 start\n");
    g_huff_fail = 0;
    run_stage2();
    dump_stage(2, g_resp, S2_ACT, D);

    /* ---- Etapa 3 ---- */
    printf("#STAGE 3 start\n");
    g_huff_fail = 0;
    run_stage3_or_4(false);
    dump_stage(3, g_resp, S3_ACT, D);

    /* ---- Etapa 4 ---- */
    printf("#STAGE 4 start\n");
    g_huff_fail = 0;
    run_stage3_or_4(true);
    dump_stage(4, g_resp, S4_ACT, D);

    printf("#ALLDONE\n");
    fflush(stdout);
    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}
