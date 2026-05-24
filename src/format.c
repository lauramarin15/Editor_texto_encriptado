#include <stdio.h>
#include <string.h>
#include <time.h>
#include "format.h"

/* ── Inicialización ─────────────────────────────────────────── */
void header_init(lz4e_header_t *h)
{
    memset(h, 0, sizeof(*h));
    h->magic[0]  = MAGIC_0;
    h->magic[1]  = MAGIC_1;
    h->magic[2]  = MAGIC_2;
    h->magic[3]  = MAGIC_3;
    h->version   = FORMAT_VERSION;
    h->flags     = FLAG_NONE;
    h->created_at  = (uint64_t)time(NULL);
    h->modified_at = (uint64_t)time(NULL);
}

/* ── Validación ─────────────────────────────────────────────── */
int header_validate(const lz4e_header_t *h)
{
    if (h->magic[0] != MAGIC_0 || h->magic[1] != MAGIC_1 ||
        h->magic[2] != MAGIC_2 || h->magic[3] != MAGIC_3) {
        fprintf(stderr, "[format] magic number inválido\n");
        return 0;
    }
    if (h->version != FORMAT_VERSION) {
        fprintf(stderr, "[format] versión desconocida: 0x%02X\n", h->version);
        return 0;
    }
    uint8_t valid_algos[] = { ALGO_RAW, ALGO_HUFFMAN, ALGO_LZ4, ALGO_RLE };
    int ok_primary = 0, ok_secondary = 0;
    for (int i = 0; i < 4; i++) {
        if (h->algo_primary   == valid_algos[i]) ok_primary   = 1;
        if (h->algo_secondary == valid_algos[i]) ok_secondary = 1;
    }
    if (!ok_primary || !ok_secondary) {
        fprintf(stderr, "[format] algoritmo desconocido: primary=0x%02X secondary=0x%02X\n",
                h->algo_primary, h->algo_secondary);
        return 0;
    }
    return 1;
}

/* ── Checksum (suma de bytes simple, rápida) ────────────────── */
uint32_t header_checksum(const uint8_t *data, size_t len)
{
    /*
     * Fletcher-16 extendido a 32 bits:
     * Más robusto que una suma plana — detecta transposiciones de bytes.
     * No tan costoso como CRC32, apropiado para un proyecto educativo.
     */
    uint32_t sum1 = 0, sum2 = 0;
    for (size_t i = 0; i < len; i++) {
        sum1 = (sum1 + data[i])    % 65535;
        sum2 = (sum2 + sum1)       % 65535;
    }
    return (sum2 << 16) | sum1;
}

/* ── Impresión del header (para editor info) ────────────────── */
void header_print(const lz4e_header_t *h)
{
    const char *algo_name[] = { "RAW", "Huffman", "LZ4", "RLE" };

    printf("╔══════════════════════════════════════╗\n");
    printf("║         .lz4e FILE INFO              ║\n");
    printf("╠══════════════════════════════════════╣\n");
    printf("║ Magic:       %c%c%c%c (v%d)              \n",
           h->magic[0], h->magic[1], h->magic[2], h->magic[3], h->version);
    printf("║ Algoritmo:   %s",
           h->algo_primary <= 3 ? algo_name[h->algo_primary] : "?");
    if (h->algo_secondary != ALGO_RAW)
        printf(" + %s", h->algo_secondary <= 3 ? algo_name[h->algo_secondary] : "?");
    printf("\n");

    uint32_t orig = h->size_original;
    uint32_t comp = h->size_compressed;
    float ratio = (orig > 0) ? (1.0f - (float)comp / orig) * 100.0f : 0.0f;

    printf("║ Tamaño orig: %u bytes\n", orig);
    printf("║ Tamaño comp: %u bytes\n", comp);
    printf("║ Ahorro:      %.1f%%\n", ratio);
    printf("║ Checksum:    0x%08X\n", h->checksum);

    /* Flags */
    printf("║ Flags:       ");
    if (h->flags == FLAG_NONE)           printf("ninguno");
    if (h->flags & FLAG_RICH_TEXT)       printf("RICH_TEXT ");
    if (h->flags & FLAG_MMAP_WRITTEN)    printf("MMAP ");
    if (h->flags & FLAG_ENCRYPTED)       printf("ENCRYPTED (RC4) ");
    printf("\n");

    /* Timestamps */
    char buf[32];
    time_t t = (time_t)h->created_at;
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", localtime(&t));
    printf("║ Creado:      %s\n", buf);
    t = (time_t)h->modified_at;
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", localtime(&t));
    printf("║ Modificado:  %s\n", buf);
    printf("╚══════════════════════════════════════╝\n");
}
