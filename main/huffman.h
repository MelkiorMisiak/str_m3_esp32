/*
 * huffman.h - Codificação de Huffman (C puro, sem malloc, sem dependência de ESP-IDF)
 *
 * Todo o estado fica em um huff_ctx_t fornecido pelo chamador, então o tempo de
 * execução não depende do alocador de memória. Por ser C puro, o mesmo código
 * compila no ESP32 (firmware) e no PC (teste de cobertura com gcov).
 */
#ifndef HUFFMAN_H
#define HUFFMAN_H

#include <stddef.h>
#include <stdint.h>

#define HUFF_ALPHABET   256                    /* símbolos possíveis (bytes)      */
#define HUFF_MAX_NODES  (2 * HUFF_ALPHABET - 1) /* folhas + nós internos          */
#define HUFF_NONE       0xFFFFu                /* "sem pai" (raiz)               */

typedef enum {
    HUFF_OK            =  0,
    HUFF_ERR_EMPTY     = -1,  /* entrada vazia                                   */
    HUFF_ERR_OVERFLOW  = -2,  /* buffer de bits pequeno demais                   */
    HUFF_ERR_DECODE    = -3,  /* fluxo de bits acabou antes de decodificar tudo  */
    HUFF_ERR_MISMATCH  = -4   /* saída decodificada difere da entrada            */
} huff_status_t;

typedef struct {
    /* Árvore (índices 0..255 = folhas/símbolos; 256.. = nós internos) */
    uint32_t freq[HUFF_MAX_NODES];
    uint16_t left[HUFF_MAX_NODES];
    uint16_t right[HUFF_MAX_NODES];
    uint16_t parent[HUFF_MAX_NODES];
    uint16_t heap[HUFF_ALPHABET];   /* min-heap de nós (fila de prioridade)      */
    uint16_t heap_size;
    uint16_t root;

    /* Buffers pertencentes ao chamador */
    uint8_t *bitbuf;                /* fluxo de bits codificado                  */
    size_t   bitbuf_cap;            /* capacidade de bitbuf em BYTES             */
    size_t   bits_len;              /* bits válidos em bitbuf após encode        */
    uint8_t *outbuf;                /* destino da decodificação (n bytes)        */

    /* Injeção de falha (só para testar o caminho de validação). -1 = desligada.
     * Inverte esse bit do fluxo codificado entre encode e decode.               */
    int32_t  fault_bit;
} huff_ctx_t;

/* Associa os buffers do chamador ao contexto e desliga a injeção de falha. */
void huffman_init(huff_ctx_t *c, uint8_t *bitbuf, size_t bitbuf_cap, uint8_t *outbuf);

/* Constrói a árvore a partir de in[0..n) e codifica para c->bitbuf. */
huff_status_t huffman_encode(huff_ctx_t *c, const uint8_t *in, size_t n);

/* Decodifica n símbolos de c->bitbuf para out. */
huff_status_t huffman_decode(huff_ctx_t *c, uint8_t *out, size_t n);

/* Tarefa completa sob teste: encode + decode + comparação com a entrada.
 * É isto que o teste de tarefa usa para confirmar a corretude lógica.        */
huff_status_t huffman_roundtrip(huff_ctx_t *c, const uint8_t *in, size_t n);

#endif /* HUFFMAN_H */
