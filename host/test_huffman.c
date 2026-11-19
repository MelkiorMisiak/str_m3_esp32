/*
 * test_huffman.c - Experimento A (cobertura x HWM), executado no PC com gcov.
 *
 * Parte 1: casos de teste dirigidos até 100% de linha e de desvio em huffman.c.
 *          Cada caso é anunciado, para você montar a tabela "caso -> cobertura".
 * Parte 2: campanha aleatória de 10.000 execuções, imprimindo o HWM acumulado.
 *          Mostra que, com a cobertura já saturada, o HWM continua subindo.
 *
 * Compilar/rodar: ./run_coverage.sh
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "huffman.h"

#define N_RANDOM 2048            /* mesmo tamanho do firmware (HUFF_N) */
#define SEED     123456789u      /* mesma semente do firmware          */

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { printf("  FALHOU: %s (linha %d)\n", #cond, __LINE__); g_fail++; } } while (0)

static uint32_t rng_state = SEED;
static uint32_t xorshift32(void)
{
    uint32_t x = rng_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return rng_state = x;
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint8_t g_in[8192], g_out[8192], g_bits[16384];
static huff_ctx_t g_ctx;

static void fresh(size_t bits_cap)
{
    huffman_init(&g_ctx, g_bits, bits_cap, g_out);
}

/* ---------------- Parte 1: casos dirigidos ---------------- */

static void t_empty(void)
{
    printf("[T1] entrada vazia\n");
    fresh(sizeof g_bits);
    CHECK(huffman_encode(&g_ctx, g_in, 0) == HUFF_ERR_EMPTY);
}

static void t_single_symbol(void)
{
    printf("[T2] um unico simbolo distinto (raiz = folha)\n");
    fresh(sizeof g_bits);
    memset(g_in, 'A', 100);
    CHECK(huffman_roundtrip(&g_ctx, g_in, 100) == HUFF_OK);
    CHECK(g_ctx.bits_len == 100);
}

static void t_two_symbols(void)
{
    printf("[T3] dois simbolos (arvore minima)\n");
    fresh(sizeof g_bits);
    for (int i = 0; i < 64; i++) g_in[i] = (i & 1) ? 'x' : 'y';
    CHECK(huffman_roundtrip(&g_ctx, g_in, 64) == HUFF_OK);
}

static void t_all_equal_freq(void)
{
    printf("[T4] 256 simbolos com mesma frequencia (desempate por indice)\n");
    fresh(sizeof g_bits);
    for (int i = 0; i < 256; i++) g_in[i] = (uint8_t)i;
    CHECK(huffman_roundtrip(&g_ctx, g_in, 256) == HUFF_OK);
}

static void t_skewed_fibonacci(void)
{
    printf("[T5] frequencias de Fibonacci (arvore degenerada, codigos longos)\n");
    fresh(sizeof g_bits);
    size_t n = 0;
    unsigned a = 1, b = 1;
    for (int s = 0; s < 16; s++) {
        for (unsigned k = 0; k < a; k++) g_in[n++] = (uint8_t)('a' + s);
        unsigned t = a + b; a = b; b = t;
    }
    CHECK(huffman_roundtrip(&g_ctx, g_in, n) == HUFF_OK);
}

static void t_random(void)
{
    printf("[T6] 2048 bytes aleatorios (caso do experimento)\n");
    fresh(sizeof g_bits);
    for (int i = 0; i < N_RANDOM; i++) g_in[i] = (uint8_t)xorshift32();
    CHECK(huffman_roundtrip(&g_ctx, g_in, N_RANDOM) == HUFF_OK);
}

static void t_overflow(void)
{
    printf("[T7] buffer de bits pequeno demais -> OVERFLOW\n");
    fresh(4);
    for (int i = 0; i < 256; i++) g_in[i] = (uint8_t)i;
    CHECK(huffman_encode(&g_ctx, g_in, 256) == HUFF_ERR_OVERFLOW);
}

static void t_decode_truncated(void)
{
    printf("[T8] decodificar mais simbolos do que foram codificados -> DECODE\n");
    fresh(sizeof g_bits);
    for (int i = 0; i < 64; i++) g_in[i] = (uint8_t)(i % 7);
    CHECK(huffman_encode(&g_ctx, g_in, 64) == HUFF_OK);
    CHECK(huffman_decode(&g_ctx, g_out, 200) == HUFF_ERR_DECODE);

    /* mesmo caminho no caso de um unico simbolo */
    fresh(sizeof g_bits);
    memset(g_in, 'Z', 10);
    CHECK(huffman_encode(&g_ctx, g_in, 10) == HUFF_OK);
    CHECK(huffman_decode(&g_ctx, g_out, 20) == HUFF_ERR_DECODE);
}

static void t_fault_injection(void)
{
    printf("[T9] injecao de falha: validacao deve detectar MISMATCH e DECODE\n");
    int saw_mismatch = 0, saw_decode = 0, saw_ok_beyond = 0;
    for (int i = 0; i < 64; i++) g_in[i] = (uint8_t)(xorshift32() % 5);

    fresh(sizeof g_bits);
    CHECK(huffman_encode(&g_ctx, g_in, 64) == HUFF_OK);
    size_t nbits = g_ctx.bits_len;

    for (size_t b = 0; b < nbits; b++) {
        fresh(sizeof g_bits);
        g_ctx.fault_bit = (int32_t)b;
        huff_status_t st = huffman_roundtrip(&g_ctx, g_in, 64);
        if (st == HUFF_ERR_MISMATCH) saw_mismatch = 1;
        if (st == HUFF_ERR_DECODE)   saw_decode = 1;
        CHECK(st != HUFF_OK);                      /* falha injetada nunca passa */
    }
    CHECK(saw_mismatch);
    CHECK(saw_decode);

    /* fault_bit fora do fluxo: nada e alterado, resultado OK */
    fresh(sizeof g_bits);
    g_ctx.fault_bit = (int32_t)nbits + 1000;
    if (huffman_roundtrip(&g_ctx, g_in, 64) == HUFF_OK) saw_ok_beyond = 1;
    CHECK(saw_ok_beyond);

    /* overflow tambem propaga pelo roundtrip */
    fresh(2);
    CHECK(huffman_roundtrip(&g_ctx, g_in, 64) == HUFF_ERR_OVERFLOW);
}

/* ---------------- Parte 2: HWM com cobertura saturada ---------------- */

static void hwm_campaign(void)
{
    static const int checkpoints[] = {10, 20, 100, 1000, 10000};
    const int total = 10000;
    uint64_t hwm = 0, mn = UINT64_MAX, sum = 0;
    size_t ci = 0;

    printf("\n== Campanha aleatoria no PC (cobertura ja saturada) ==\n");
    printf("%-10s %-12s\n", "amostras", "HWM_ns");

    FILE *f = fopen("host_hwm.csv", "w");
    if (f) fprintf(f, "ativacao,tempo_us\n");

    fresh(sizeof g_bits);
    for (int k = 1; k <= total; k++) {
        for (int i = 0; i < N_RANDOM; i++) g_in[i] = (uint8_t)xorshift32();
        uint64_t t0 = now_ns();
        huff_status_t st = huffman_roundtrip(&g_ctx, g_in, N_RANDOM);
        uint64_t dt = now_ns() - t0;
        if (st != HUFF_OK) { printf("erro na ativacao %d\n", k); g_fail++; }
        if (dt > hwm) hwm = dt;
        if (dt < mn) mn = dt;
        sum += dt;
        if (f) fprintf(f, "%d,%llu\n", k - 1, (unsigned long long)(dt / 1000));
        if (ci < sizeof(checkpoints) / sizeof(checkpoints[0]) && k == checkpoints[ci]) {
            printf("%-10d %-12llu\n", k, (unsigned long long)hwm);
            ci++;
        }
    }
    printf("min=%llu ns  media=%llu ns  HWM=%llu ns\n",
           (unsigned long long)mn, (unsigned long long)(sum / total), (unsigned long long)hwm);
    if (f) fclose(f);
}

int main(void)
{
    printf("== Casos dirigidos (cobertura de huffman.c) ==\n");
    t_empty();
    t_single_symbol();
    t_two_symbols();
    t_all_equal_freq();
    t_skewed_fibonacci();
    t_random();
    t_overflow();
    t_decode_truncated();
    t_fault_injection();

    printf("\nTestes: %s (%d falhas)\n", g_fail ? "FALHA" : "OK", g_fail);
    if (g_fail) return 1;

    hwm_campaign();
    return g_fail ? 1 : 0;
}
