/*
 * huffman.c - Codificação de Huffman com validação (round-trip)
 *
 * Decisões de projeto (importantes para medir tempo de resposta):
 *  - Nenhum malloc: tudo em huff_ctx_t, então não há variação do alocador.
 *  - Os códigos NÃO são tabelados: o encode sobe da folha até a raiz pelos
 *    ponteiros "parent" e emite os bits invertidos. Isso suporta árvores
 *    degeneradas (profundidade até 255) sem tabela de 256 x 255 bits.
 *  - Desempate determinístico na fila de prioridade (frequência, depois índice):
 *    a mesma entrada sempre gera a mesma árvore.
 */
#include <string.h>
#include "huffman.h"

/* ---------- min-heap ---------- */

/* Ordem total: menor frequência primeiro; empate pelo menor índice. */
static int node_less(const huff_ctx_t *c, uint16_t a, uint16_t b)
{
    if (c->freq[a] != c->freq[b]) {
        return c->freq[a] < c->freq[b];
    }
    return a < b;
}

static void heap_push(huff_ctx_t *c, uint16_t v)
{
    size_t i = c->heap_size++;
    c->heap[i] = v;
    while (i > 0) {                               /* sift-up */
        size_t p = (i - 1) / 2;
        if (!node_less(c, c->heap[i], c->heap[p])) {
            break;
        }
        uint16_t t = c->heap[i]; c->heap[i] = c->heap[p]; c->heap[p] = t;
        i = p;
    }
}

static uint16_t heap_pop(huff_ctx_t *c)
{
    uint16_t top = c->heap[0];
    c->heap[0] = c->heap[--c->heap_size];
    size_t i = 0;
    for (;;) {                                    /* sift-down */
        size_t l = 2 * i + 1;
        if (l >= c->heap_size) {
            break;
        }
        size_t r = l + 1, m = l;
        if (r < c->heap_size && node_less(c, c->heap[r], c->heap[l])) {
            m = r;
        }
        if (!node_less(c, c->heap[m], c->heap[i])) {
            break;
        }
        uint16_t t = c->heap[i]; c->heap[i] = c->heap[m]; c->heap[m] = t;
        i = m;
    }
    return top;
}

/* ---------- construção da árvore ---------- */

/* Conta frequências e funde repetidamente os dois nós de menor frequência.
 * Retorna a raiz. Com um único símbolo distinto a raiz é a própria folha. */
static uint16_t build_tree(huff_ctx_t *c, const uint8_t *in, size_t n)
{
    memset(c->freq, 0, sizeof(c->freq));
    memset(c->parent, 0xFF, sizeof(c->parent));   /* HUFF_NONE em todos */
    c->heap_size = 0;

    for (size_t i = 0; i < n; i++) {
        c->freq[in[i]]++;
    }
    for (uint16_t s = 0; s < HUFF_ALPHABET; s++) {
        if (c->freq[s] > 0) {
            heap_push(c, s);
        }
    }

    uint16_t next = HUFF_ALPHABET;                /* próximo nó interno livre */
    while (c->heap_size > 1) {
        uint16_t a = heap_pop(c);
        uint16_t b = heap_pop(c);
        c->freq[next]  = c->freq[a] + c->freq[b];
        c->left[next]  = a;
        c->right[next] = b;
        c->parent[a] = c->parent[b] = next;
        heap_push(c, next);
        next++;
    }
    return heap_pop(c);
}

/* ---------- API ---------- */

void huffman_init(huff_ctx_t *c, uint8_t *bitbuf, size_t bitbuf_cap, uint8_t *outbuf)
{
    memset(c, 0, sizeof(*c));
    c->bitbuf = bitbuf;
    c->bitbuf_cap = bitbuf_cap;
    c->outbuf = outbuf;
    c->fault_bit = -1;
}

/* Codifica: para cada símbolo sobe folha->raiz coletando o bit de cada aresta
 * (1 = filho direito), depois grava os bits em ordem raiz->folha (MSB first). */
huff_status_t huffman_encode(huff_ctx_t *c, const uint8_t *in, size_t n)
{
    if (n == 0) {
        return HUFF_ERR_EMPTY;
    }
    c->root = build_tree(c, in, n);
    memset(c->bitbuf, 0, c->bitbuf_cap);

    uint8_t tmp[HUFF_MAX_NODES];
    size_t bitpos = 0;
    for (size_t i = 0; i < n; i++) {
        uint16_t node = in[i];
        size_t len = 0;
        while (node != c->root) {
            uint16_t p = c->parent[node];
            tmp[len++] = (c->right[p] == node);
            node = p;
        }
        if (len == 0) {                           /* um único símbolo: código "0" */
            tmp[0] = 0;
            len = 1;
        }
        if (bitpos + len > c->bitbuf_cap * 8) {
            return HUFF_ERR_OVERFLOW;
        }
        for (size_t k = len; k > 0; k--) {
            if (tmp[k - 1]) {
                c->bitbuf[bitpos >> 3] |= (uint8_t)(0x80u >> (bitpos & 7));
            }
            bitpos++;
        }
    }
    c->bits_len = bitpos;
    return HUFF_OK;
}

/* Decodifica: desce da raiz seguindo os bits até encontrar uma folha (< 256). */
huff_status_t huffman_decode(huff_ctx_t *c, uint8_t *out, size_t n)
{
    size_t pos = 0;
    for (size_t i = 0; i < n; i++) {
        uint16_t node = c->root;
        if (node < HUFF_ALPHABET) {               /* caso de um único símbolo */
            if (pos >= c->bits_len) {
                return HUFF_ERR_DECODE;
            }
            pos++;
        } else {
            while (node >= HUFF_ALPHABET) {
                if (pos >= c->bits_len) {
                    return HUFF_ERR_DECODE;
                }
                unsigned bit = (c->bitbuf[pos >> 3] >> (7 - (pos & 7))) & 1u;
                pos++;
                node = bit ? c->right[node] : c->left[node];
            }
        }
        out[i] = (uint8_t)node;
    }
    return HUFF_OK;
}

/* Tarefa completa: é o que a campanha de medição cronometra. */
huff_status_t huffman_roundtrip(huff_ctx_t *c, const uint8_t *in, size_t n)
{
    huff_status_t st = huffman_encode(c, in, n);
    if (st != HUFF_OK) {
        return st;
    }
    if (c->fault_bit >= 0 && (size_t)c->fault_bit < c->bits_len) {
        c->bitbuf[c->fault_bit >> 3] ^= (uint8_t)(0x80u >> (c->fault_bit & 7));
    }
    st = huffman_decode(c, c->outbuf, n);
    if (st != HUFF_OK) {
        return st;
    }
    if (memcmp(in, c->outbuf, n) != 0) {
        return HUFF_ERR_MISMATCH;
    }
    return HUFF_OK;
}
