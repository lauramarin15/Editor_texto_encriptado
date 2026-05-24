#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <lz4.h>
#include "compress.h"

/* ═══════════════════════════════════════════════════════════════
 * RLE — Run-Length Encoding
 * ═══════════════════════════════════════════════════════════════ */

/*
 * rle_score: calcula qué fracción de los bytes pertenece a rachas
 * de 3 o más caracteres iguales. Resultado en [0.0, 1.0].
 *
 * Solo analizamos los primeros `max_scan` bytes para no desperdiciar
 * CPU en archivos grandes.
 */
float rle_score(const uint8_t *data, size_t len)
{
    if (len == 0) return 0.0f;

    size_t scan  = len < 4096 ? len : 4096;
    size_t run_bytes = 0;
    size_t i = 0;

    while (i < scan) {
        size_t j = i + 1;
        while (j < scan && data[j] == data[i]) j++;
        if (j - i >= 3) run_bytes += (j - i);
        i = j;
    }

    return (float)run_bytes / (float)scan;
}

/*
 * rle_compress: codifica rachas como pares (count, byte).
 * Formato: cada par ocupa 2 bytes. count máximo = 255.
 *
 * En el peor caso (todos los bytes distintos), el output es 2× el input.
 * El caller debe verificar que el resultado sea menor que el original.
 */
uint8_t *rle_compress(const uint8_t *in, size_t len, size_t *out_len)
{
    if (!in || len == 0) return NULL;

    /* Peor caso: 2 bytes por cada byte de entrada */
    uint8_t *out = malloc(len * 2 + 2);
    if (!out) return NULL;

    size_t wi = 0, i = 0;
    while (i < len) {
        uint8_t sym = in[i];
        size_t  run = 1;
        while (i + run < len && in[i + run] == sym && run < 255)
            run++;
        out[wi++] = (uint8_t)run;
        out[wi++] = sym;
        i += run;
    }

    *out_len = wi;
    return out;
}

uint8_t *rle_decompress(const uint8_t *in, size_t len,
                        size_t original_size, size_t *out_len)
{
    if (!in || len == 0) return NULL;

    uint8_t *out = malloc(original_size + 1);
    if (!out) return NULL;

    size_t wi = 0;
    for (size_t i = 0; i + 1 < len; i += 2) {
        uint8_t count = in[i];
        uint8_t sym   = in[i + 1];
        if (wi + count > original_size) {
            free(out);
            return NULL;
        }
        memset(out + wi, sym, count);
        wi += count;
    }

    out[wi]   = '\0';
    *out_len  = wi;
    return out;
}

/* ═══════════════════════════════════════════════════════════════
 * HUFFMAN
 * ═══════════════════════════════════════════════════════════════ */

/* ── Nodo del árbol ─────────────────────────────────────────── */
typedef struct HuffNode {
    uint8_t          symbol;
    uint32_t         freq;
    struct HuffNode *left;
    struct HuffNode *right;
} HuffNode;

/* ── Min-heap simple para construir el árbol ────────────────── */
typedef struct {
    HuffNode **nodes;
    int        size;
    int        capacity;
} MinHeap;

static MinHeap *heap_create(int cap)
{
    MinHeap *h = malloc(sizeof(MinHeap));
    if (!h) return NULL;
    h->nodes    = malloc(sizeof(HuffNode *) * cap);
    h->size     = 0;
    h->capacity = cap;
    if (!h->nodes) { free(h); return NULL; }
    return h;
}

static void heap_destroy(MinHeap *h)
{
    free(h->nodes);
    free(h);
}

static void heap_push(MinHeap *h, HuffNode *node)
{
    int i = h->size++;
    h->nodes[i] = node;
    /* Sift up */
    while (i > 0) {
        int parent = (i - 1) / 2;
        if (h->nodes[parent]->freq > h->nodes[i]->freq) {
            HuffNode *tmp      = h->nodes[parent];
            h->nodes[parent]   = h->nodes[i];
            h->nodes[i]        = tmp;
            i = parent;
        } else break;
    }
}

static HuffNode *heap_pop(MinHeap *h)
{
    if (h->size == 0) return NULL;
    HuffNode *min = h->nodes[0];
    h->nodes[0]   = h->nodes[--h->size];
    /* Sift down */
    int i = 0;
    while (1) {
        int l = 2*i+1, r = 2*i+2, smallest = i;
        if (l < h->size && h->nodes[l]->freq < h->nodes[smallest]->freq)
            smallest = l;
        if (r < h->size && h->nodes[r]->freq < h->nodes[smallest]->freq)
            smallest = r;
        if (smallest == i) break;
        HuffNode *tmp      = h->nodes[i];
        h->nodes[i]        = h->nodes[smallest];
        h->nodes[smallest] = tmp;
        i = smallest;
    }
    return min;
}

/* ── Tabla de códigos ───────────────────────────────────────── */
typedef struct {
    uint32_t code;   /* código binario, bit más significativo primero */
    uint8_t  len;    /* longitud en bits                              */
} HuffCode;

static void build_codes(HuffNode *node, uint32_t code, uint8_t depth,
                        HuffCode table[256])
{
    if (!node) return;
    if (!node->left && !node->right) {
        /* Hoja: asignar código */
        table[node->symbol].code = code;
        table[node->symbol].len  = depth ? depth : 1;
        return;
    }
    build_codes(node->left,  (code << 1) | 0, depth + 1, table);
    build_codes(node->right, (code << 1) | 1, depth + 1, table);
}

static void huff_free_tree(HuffNode *node)
{
    if (!node) return;
    huff_free_tree(node->left);
    huff_free_tree(node->right);
    free(node);
}

/*
 * Serialización del árbol:
 * Recorrido preorden: '0' + símbolo para hojas, '1' para nodos internos.
 * Esto permite reconstruir el árbol en la descompresión.
 *
 * Formato en el stream comprimido:
 *   [4 bytes: tamaño del árbol serializado]
 *   [N bytes: árbol serializado en bits, empaquetado en bytes]
 *   [datos comprimidos en bits, empaquetados en bytes]
 */

/* BitWriter: escribe bits en un buffer de bytes */
typedef struct {
    uint8_t *buf;
    size_t   capacity;
    size_t   byte_pos;
    uint8_t  bit_pos;    /* 0..7, bit actual dentro del byte */
} BitWriter;

static BitWriter *bw_create(size_t cap)
{
    BitWriter *bw = malloc(sizeof(BitWriter));
    if (!bw) return NULL;
    bw->buf      = calloc(cap, 1);
    bw->capacity = cap;
    bw->byte_pos = 0;
    bw->bit_pos  = 0;
    if (!bw->buf) { free(bw); return NULL; }
    return bw;
}

static void bw_destroy(BitWriter *bw)
{
    free(bw->buf);
    free(bw);
}

static int bw_write_bit(BitWriter *bw, uint8_t bit)
{
    if (bw->byte_pos >= bw->capacity) return -1;
    if (bit) bw->buf[bw->byte_pos] |= (0x80 >> bw->bit_pos);
    bw->bit_pos++;
    if (bw->bit_pos == 8) {
        bw->bit_pos = 0;
        bw->byte_pos++;
    }
    return 0;
}

static void bw_write_byte(BitWriter *bw, uint8_t byte)
{
    for (int i = 7; i >= 0; i--)
        bw_write_bit(bw, (byte >> i) & 1);
}

static size_t bw_bytes_used(const BitWriter *bw)
{
    return bw->byte_pos + (bw->bit_pos > 0 ? 1 : 0);
}

/* Serializar árbol (preorden) */
static void serialize_tree(HuffNode *node, BitWriter *bw)
{
    if (!node) return;
    if (!node->left && !node->right) {
        bw_write_bit(bw, 1);       /* marca de hoja */
        bw_write_byte(bw, node->symbol);
    } else {
        bw_write_bit(bw, 0);       /* marca de nodo interno */
        serialize_tree(node->left,  bw);
        serialize_tree(node->right, bw);
    }
}

/* BitReader: lee bits de un buffer de bytes */
typedef struct {
    const uint8_t *buf;
    size_t         size;
    size_t         byte_pos;
    uint8_t        bit_pos;
} BitReader;

static int br_read_bit(BitReader *br)
{
    if (br->byte_pos >= br->size) return -1;
    int bit = (br->buf[br->byte_pos] >> (7 - br->bit_pos)) & 1;
    br->bit_pos++;
    if (br->bit_pos == 8) { br->bit_pos = 0; br->byte_pos++; }
    return bit;
}

static int br_read_byte(BitReader *br)
{
    uint8_t byte = 0;
    for (int i = 7; i >= 0; i--) {
        int b = br_read_bit(br);
        if (b < 0) return -1;
        byte |= (uint8_t)(b << i);
    }
    return byte;
}

/* Deserializar árbol */
static HuffNode *deserialize_tree(BitReader *br)
{
    int bit = br_read_bit(br);
    if (bit < 0) return NULL;

    HuffNode *node = calloc(1, sizeof(HuffNode));
    if (!node) return NULL;

    if (bit == 1) {
        /* Hoja */
        int sym = br_read_byte(br);
        if (sym < 0) { free(node); return NULL; }
        node->symbol = (uint8_t)sym;
    } else {
        /* Nodo interno */
        node->left  = deserialize_tree(br);
        node->right = deserialize_tree(br);
    }
    return node;
}

/* ── Compresión Huffman ─────────────────────────────────────── */
uint8_t *huffman_compress(const uint8_t *in, size_t len, size_t *out_len)
{
    if (!in || len == 0) return NULL;

    /* 1. Contar frecuencias */
    uint32_t freq[256] = {0};
    for (size_t i = 0; i < len; i++) freq[in[i]]++;

    /* 2. Construir min-heap */
    MinHeap *heap = heap_create(512);
    if (!heap) return NULL;

    int symbol_count = 0;
    for (int i = 0; i < 256; i++) {
        if (freq[i] > 0) {
            HuffNode *node = malloc(sizeof(HuffNode));
            if (!node) { heap_destroy(heap); return NULL; }
            node->symbol = (uint8_t)i;
            node->freq   = freq[i];
            node->left   = node->right = NULL;
            heap_push(heap, node);
            symbol_count++;
        }
    }

    /* Caso especial: un solo símbolo */
    if (symbol_count == 1) {
        HuffNode *only = heap_pop(heap);
        HuffNode *root = calloc(1, sizeof(HuffNode));
        root->freq  = only->freq;
        root->left  = only;
        root->right = NULL;
        heap_push(heap, root);
    }

    /* 3. Construir árbol Huffman */
    while (heap->size > 1) {
        HuffNode *a = heap_pop(heap);
        HuffNode *b = heap_pop(heap);
        HuffNode *parent = malloc(sizeof(HuffNode));
        if (!parent) { heap_destroy(heap); return NULL; }
        parent->symbol = 0;
        parent->freq   = a->freq + b->freq;
        parent->left   = a;
        parent->right  = b;
        heap_push(heap, parent);
    }
    HuffNode *root = heap_pop(heap);
    heap_destroy(heap);

    /* 4. Generar tabla de códigos */
    HuffCode table[256] = {{0, 0}};
    build_codes(root, 0, 0, table);

    /* 5. Estimar tamaño de salida y asignar buffer */
    size_t bits_needed = 0;
    for (size_t i = 0; i < len; i++)
        bits_needed += table[in[i]].len;

    /* Header interno de Huffman: 4 bytes (bits del árbol) + árbol + datos */
    size_t buf_cap = 8 + (len * 3);   /* margen generoso */
    BitWriter *bw = bw_create(buf_cap);
    if (!bw) { huff_free_tree(root); return NULL; }

    /* 6. Serializar árbol */
    size_t tree_start_byte = 4;   /* reservamos 4 bytes para el tamaño del árbol */
    bw->byte_pos = tree_start_byte;
    serialize_tree(root, bw);
    size_t tree_bytes = bw->byte_pos - tree_start_byte
                      + (bw->bit_pos > 0 ? 1 : 0);

    /* Escribir tamaño del árbol en los primeros 4 bytes (little-endian) */
    bw->buf[0] = (tree_bytes)       & 0xFF;
    bw->buf[1] = (tree_bytes >> 8)  & 0xFF;
    bw->buf[2] = (tree_bytes >> 16) & 0xFF;
    bw->buf[3] = (tree_bytes >> 24) & 0xFF;

    /* Asegurar que empezamos en un byte limpio después del árbol */
    if (bw->bit_pos != 0) { bw->byte_pos++; bw->bit_pos = 0; }

    /* 7. Codificar datos */
    for (size_t i = 0; i < len; i++) {
        HuffCode hc = table[in[i]];
        for (int b = hc.len - 1; b >= 0; b--)
            bw_write_bit(bw, (hc.code >> b) & 1);
    }

    size_t total = bw_bytes_used(bw);
    uint8_t *result = malloc(total);
    if (result) {
        memcpy(result, bw->buf, total);
        *out_len = total;
    }

    bw_destroy(bw);
    huff_free_tree(root);
    return result;
}

/* ── Descompresión Huffman ──────────────────────────────────── */
uint8_t *huffman_decompress(const uint8_t *in, size_t len,
                            size_t original_size, size_t *out_len)
{
    if (!in || len < 4) return NULL;

    /* Leer tamaño del árbol */
    uint32_t tree_bytes = (uint32_t)in[0]
                        | ((uint32_t)in[1] << 8)
                        | ((uint32_t)in[2] << 16)
                        | ((uint32_t)in[3] << 24);

    if (tree_bytes + 4 > len) return NULL;

    /* Deserializar árbol */
    BitReader br_tree = {
        .buf      = in + 4,
        .size     = tree_bytes,
        .byte_pos = 0,
        .bit_pos  = 0
    };
    HuffNode *root = deserialize_tree(&br_tree);
    if (!root) return NULL;

    /* Descodificar datos */
    size_t data_offset = 4 + tree_bytes;
    BitReader br_data = {
        .buf      = in + data_offset,
        .size     = len - data_offset,
        .byte_pos = 0,
        .bit_pos  = 0
    };

    uint8_t *out = malloc(original_size + 1);
    if (!out) { huff_free_tree(root); return NULL; }

    for (size_t i = 0; i < original_size; i++) {
        HuffNode *node = root;
        while (node->left || node->right) {
            int bit = br_read_bit(&br_data);
            if (bit < 0) { free(out); huff_free_tree(root); return NULL; }
            node = bit ? node->right : node->left;
            if (!node) { free(out); huff_free_tree(root); return NULL; }
        }
        out[i] = node->symbol;
    }

    out[original_size] = '\0';
    *out_len = original_size;
    huff_free_tree(root);
    return out;
}

/* ═══════════════════════════════════════════════════════════════
 * LZ4 — wrapper sobre liblz4
 * ═══════════════════════════════════════════════════════════════ */

uint8_t *lz4_compress_buf(const uint8_t *in, size_t len, size_t *out_len)
{
    if (!in || len == 0) return NULL;

    int max_dst = LZ4_compressBound((int)len);
    uint8_t *out = malloc((size_t)max_dst);
    if (!out) return NULL;

    int compressed = LZ4_compress_default(
        (const char *)in, (char *)out, (int)len, max_dst);

    if (compressed <= 0) {
        free(out);
        return NULL;
    }

    *out_len = (size_t)compressed;
    return out;
}

uint8_t *lz4_decompress_buf(const uint8_t *in, size_t comp_len,
                             size_t original_size, size_t *out_len)
{
    if (!in || comp_len == 0) return NULL;

    uint8_t *out = malloc(original_size + 1);
    if (!out) return NULL;

    int result = LZ4_decompress_safe(
        (const char *)in, (char *)out,
        (int)comp_len, (int)original_size);

    if (result < 0) {
        free(out);
        return NULL;
    }

    out[original_size] = '\0';
    *out_len = original_size;
    return out;
}

/* ═══════════════════════════════════════════════════════════════
 * Pipeline principal
 * ═══════════════════════════════════════════════════════════════ */

uint8_t choose_algorithm(const uint8_t *data, size_t size, uint8_t *secondary)
{
    *secondary = ALGO_RAW;

    if (size < THRESHOLD_RAW)
        return ALGO_RAW;

    float score = rle_score(data, size);

    if (size < THRESHOLD_HUFFMAN) {
        if (score > RLE_SCORE_THRESHOLD) return ALGO_RLE;
        return ALGO_HUFFMAN;
    }

    /* Archivos grandes */
    if (score > RLE_SCORE_LARGE) return ALGO_RLE;
    return ALGO_LZ4;
}

CompressResult compress_auto(const uint8_t *input, size_t len)
{
    CompressResult res = { .data = NULL, .size = 0,
                           .algo = ALGO_RAW, .algo2 = ALGO_RAW };
    if (!input || len == 0) return res;

    uint8_t secondary = ALGO_RAW;
    uint8_t algo = choose_algorithm(input, len, &secondary);

    uint8_t *compressed = NULL;
    size_t   comp_len   = 0;

    switch (algo) {
        case ALGO_RAW:
            compressed = malloc(len);
            if (compressed) { memcpy(compressed, input, len); comp_len = len; }
            break;
        case ALGO_HUFFMAN:
            compressed = huffman_compress(input, len, &comp_len);
            break;
        case ALGO_LZ4:
            compressed = lz4_compress_buf(input, len, &comp_len);
            break;
        case ALGO_RLE:
            compressed = rle_compress(input, len, &comp_len);
            break;
    }

    /* Fallback: si comprimido >= original, guardar raw */
    if (!compressed || comp_len >= len) {
        free(compressed);
        compressed = malloc(len);
        if (compressed) { memcpy(compressed, input, len); comp_len = len; }
        algo = ALGO_RAW;
        secondary = ALGO_RAW;
    }

    res.data  = compressed;
    res.size  = comp_len;
    res.algo  = algo;
    res.algo2 = secondary;
    return res;
}

uint8_t *decompress(const uint8_t *compressed, size_t comp_len,
                    uint8_t algo_primary, uint8_t algo_secondary,
                    size_t original_size, size_t *out_len)
{
    uint8_t *result = NULL;

    switch (algo_primary) {
        case ALGO_RAW:
            result = malloc(original_size + 1);
            if (result) {
                memcpy(result, compressed, original_size);
                result[original_size] = '\0';
                *out_len = original_size;
            }
            break;
        case ALGO_HUFFMAN:
            result = huffman_decompress(compressed, comp_len,
                                        original_size, out_len);
            break;
        case ALGO_LZ4:
            result = lz4_decompress_buf(compressed, comp_len,
                                         original_size, out_len);
            break;
        case ALGO_RLE:
            result = rle_decompress(compressed, comp_len,
                                     original_size, out_len);
            break;
        default:
            fprintf(stderr, "[compress] algoritmo desconocido: 0x%02X\n", algo_primary);
            return NULL;
    }

    /* Si hay pipeline de dos algoritmos (ej. RLE + Huffman) */
    if (result && algo_secondary != ALGO_RAW) {
        uint8_t *second = NULL;
        size_t   second_len = 0;
        switch (algo_secondary) {
            case ALGO_HUFFMAN:
                second = huffman_decompress(result, *out_len,
                                            original_size, &second_len);
                break;
            case ALGO_RLE:
                second = rle_decompress(result, *out_len,
                                         original_size, &second_len);
                break;
            default: break;
        }
        if (second) {
            free(result);
            result   = second;
            *out_len = second_len;
        }
    }

    return result;
}
