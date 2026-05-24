#ifndef COMPRESS_H
#define COMPRESS_H

/*
 * compress.h — Pipeline de compresión/descompresión
 *
 * Ningún dato viaja al disco en texto claro.
 * La compresión ocurre en User Space antes de cualquier syscall.
 *
 * Pipeline de escritura:
 *   texto (GapBuffer) → buffer contiguo → elegir algoritmo
 *   → comprimir en buffer intermedio → escribir header + payload
 *
 * Pipeline de lectura:
 *   leer header → leer payload comprimido → descomprimir
 *   → cargar en GapBuffer
 */

#include <stddef.h>
#include <stdint.h>
#include "format.h"

/* ── Umbrales de decisión ───────────────────────────────────── */
#define THRESHOLD_RAW       64      /* < 64 bytes  → sin compresión  */
#define THRESHOLD_HUFFMAN   1024    /* < 1 KB      → Huffman          */
#define RLE_SCORE_THRESHOLD 0.50f   /* score RLE > 0.5 → usar RLE    */
#define RLE_SCORE_LARGE     0.60f   /* archivos grandes               */

/* ── Resultado de compresión ────────────────────────────────── */
typedef struct {
    uint8_t *data;          /* buffer comprimido (heap, caller libera) */
    size_t   size;          /* tamaño del buffer comprimido            */
    uint8_t  algo;          /* algoritmo usado (ALGO_*)                */
    uint8_t  algo2;         /* segundo algoritmo (0 = ninguno)         */
} CompressResult;

/* ── API principal ──────────────────────────────────────────── */

/*
 * compress_auto: elige el algoritmo óptimo y comprime.
 * Retorna CompressResult; result.data debe liberarse con free().
 * En error, result.data == NULL.
 */
CompressResult compress_auto(const uint8_t *input, size_t len);

/*
 * decompress: descomprime según el header del archivo.
 * Retorna buffer con datos originales; el caller libera con free().
 * *out_len recibe el tamaño descomprimido.
 */
uint8_t *decompress(const uint8_t *compressed, size_t comp_len,
                    uint8_t algo_primary, uint8_t algo_secondary,
                    size_t original_size, size_t *out_len);

/* ── Funciones internas (también exportadas para tests) ─────── */
float   rle_score(const uint8_t *data, size_t len);
uint8_t choose_algorithm(const uint8_t *data, size_t size, uint8_t *secondary);

/* ── RLE ────────────────────────────────────────────────────── */
uint8_t *rle_compress(const uint8_t *in, size_t len, size_t *out_len);
uint8_t *rle_decompress(const uint8_t *in, size_t len, size_t original_size, size_t *out_len);

/* ── Huffman ────────────────────────────────────────────────── */
uint8_t *huffman_compress(const uint8_t *in, size_t len, size_t *out_len);
uint8_t *huffman_decompress(const uint8_t *in, size_t len, size_t original_size, size_t *out_len);

/* ── LZ4 (wrapper liblz4) ───────────────────────────────────── */
uint8_t *lz4_compress_buf(const uint8_t *in, size_t len, size_t *out_len);
uint8_t *lz4_decompress_buf(const uint8_t *in, size_t comp_len, size_t original_size, size_t *out_len);

#endif /* COMPRESS_H */
