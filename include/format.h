#ifndef FORMAT_H
#define FORMAT_H

/*
 * format.h — Definición del formato binario .lz4e
 *
 * Estructura del archivo en disco:
 *   [0..63]   lz4e_header_t  → metadatos, magic, algoritmos, estilos
 *   [64..]    payload        → datos comprimidos
 *
 * El header ocupa exactamente 64 bytes (__attribute__((packed))).
 * Ningún archivo viaja al disco en texto claro.
 */

#include <stdint.h>

/* ── Magic number ───────────────────────────────────────────── */
#define MAGIC_0     0x4C   /* 'L' */
#define MAGIC_1     0x45   /* 'E' */
#define MAGIC_2     0x34   /* '4' */
#define MAGIC_3     0x65   /* 'e' */

#define FORMAT_VERSION  0x02

/* ── Algoritmos (byte algo_primary / algo_secondary) ────────── */
#define ALGO_RAW        0x00   /* sin compresión                */
#define ALGO_HUFFMAN    0x01   /* Huffman implementado en C     */
#define ALGO_LZ4        0x02   /* liblz4                        */
#define ALGO_RLE        0x03   /* Run-Length Encoding           */

/* ── Flags (bits del campo flags) ───────────────────────────── */
#define FLAG_NONE           0x00
#define FLAG_RICH_TEXT      0x01   /* archivo tiene estilos de texto enriquecido */
#define FLAG_MMAP_WRITTEN   0x02   /* fue escrito con mmap (informativo)         */
#define FLAG_ENCRYPTED      0x04   /* reservado para uso futuro                  */

/* ── Tipos de estilo para texto enriquecido ─────────────────── */
#define STYLE_NONE      0x00
#define STYLE_BOLD      0x01
#define STYLE_ITALIC    0x02
#define STYLE_UNDERLINE 0x04

/* ── Tamaño de página para I/O alineado ─────────────────────── */
#define PAGE_SIZE       4096
#define IO_BLOCK_SIZE   4096   /* buffers alineados al tamaño de página */

/*
 * lz4e_header_t — Header binario de 64 bytes exactos
 *
 * Campos:
 *   magic[4]         → identifica el formato (.lz4e)
 *   version          → versión del formato
 *   flags            → opciones (texto enriquecido, mmap, etc.)
 *   algo_primary     → algoritmo principal de compresión
 *   algo_secondary   → segundo algoritmo en pipeline (0x00 = ninguno)
 *   size_original    → tamaño original antes de comprimir (bytes)
 *   size_compressed  → tamaño del payload comprimido (bytes)
 *   checksum         → suma de verificación del payload
 *   style_default    → estilo por defecto del documento
 *   line_count       → número de líneas del documento
 *   created_at       → timestamp de creación (unix time)
 *   modified_at      → timestamp de última modificación
 *   reserved[16]     → reservado para extensiones futuras
 *
 * Total: 4+1+1+1+1+4+4+4+1+1+8+8+16 = 54 bytes + 10 padding → 64 bytes
 * Con __attribute__((packed)) no hay padding del compilador.
 * Los 10 bytes de 'reserved' son el relleno explícito hasta 64.
 */
typedef struct __attribute__((packed)) {
    uint8_t  magic[4];          /*  4 bytes  offset 0  */
    uint8_t  version;           /*  1 byte   offset 4  */
    uint8_t  flags;             /*  1 byte   offset 5  */
    uint8_t  algo_primary;      /*  1 byte   offset 6  */
    uint8_t  algo_secondary;    /*  1 byte   offset 7  */
    uint32_t size_original;     /*  4 bytes  offset 8  */
    uint32_t size_compressed;   /*  4 bytes  offset 12 */
    uint32_t checksum;          /*  4 bytes  offset 16 */
    uint8_t  style_default;     /*  1 byte   offset 20 */
    uint8_t  line_count_hint;   /*  1 byte   offset 21 */
    uint64_t created_at;        /*  8 bytes  offset 22 */
    uint64_t modified_at;       /*  8 bytes  offset 30 */
    uint8_t  reserved[26];      /* 26 bytes  offset 38 → total = 64 */
} lz4e_header_t;

/* Verificación en tiempo de compilación: el header debe ser exactamente 64 bytes */
_Static_assert(sizeof(lz4e_header_t) == 64, "lz4e_header_t debe ser exactamente 64 bytes");

/* ── Funciones del header ────────────────────────────────────── */
void     header_init(lz4e_header_t *h);
int      header_validate(const lz4e_header_t *h);
uint32_t header_checksum(const uint8_t *data, size_t len);
void     header_print(const lz4e_header_t *h);

#endif /* FORMAT_H */
