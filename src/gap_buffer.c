#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gap_buffer.h"

/* ── Helpers internos ───────────────────────────────────────── */

/*
 * gap_size: retorna el tamaño actual del gap.
 */
static inline size_t gap_size(const GapBuffer *gb)
{
    return gb->gap_end - gb->gap_start;
}

/*
 * grow: duplica el buffer cuando el gap es demasiado pequeño.
 * Copia el texto antes del gap, luego el texto después, dejando
 * un gap nuevo en el centro.
 *
 * Retorna 0 en éxito, -1 en fallo de memoria.
 */
static int grow(GapBuffer *gb)
{
    size_t text_len  = gb_length(gb);
    size_t new_size  = gb->buf_size * GAP_BUFFER_GROW_FACTOR;
    if (new_size < text_len + GAP_BUFFER_MIN_GAP)
        new_size = text_len + GAP_BUFFER_MIN_GAP;

    char *new_buf = malloc(new_size);
    if (!new_buf) return -1;

    /* Copiar texto antes del gap */
    memcpy(new_buf, gb->buffer, gb->gap_start);

    /* El nuevo gap empieza justo después del texto pre-gap */
    size_t new_gap_end = new_size - (gb->buf_size - gb->gap_end);

    /* Copiar texto después del gap */
    memcpy(new_buf + new_gap_end,
           gb->buffer + gb->gap_end,
           gb->buf_size - gb->gap_end);

    free(gb->buffer);
    gb->buffer    = new_buf;
    gb->gap_end   = new_gap_end;
    gb->buf_size  = new_size;

    return 0;
}

/* ── Ciclo de vida ──────────────────────────────────────────── */

GapBuffer *gb_create(void)
{
    GapBuffer *gb = malloc(sizeof(GapBuffer));
    if (!gb) return NULL;

    gb->buffer = malloc(GAP_BUFFER_INITIAL_SIZE);
    if (!gb->buffer) {
        free(gb);
        return NULL;
    }

    gb->buf_size  = GAP_BUFFER_INITIAL_SIZE;
    gb->gap_start = 0;
    gb->gap_end   = GAP_BUFFER_INITIAL_SIZE;   /* todo el buffer es gap */

    return gb;
}

GapBuffer *gb_create_from(const char *text, size_t len)
{
    GapBuffer *gb = gb_create();
    if (!gb) return NULL;

    if (len > 0 && gb_insert_str(gb, text, len) != 0) {
        gb_destroy(gb);
        return NULL;
    }
    return gb;
}

void gb_destroy(GapBuffer *gb)
{
    if (!gb) return;
    free(gb->buffer);   /* liberar el array interno */
    gb->buffer = NULL;  /* defensivo: evitar double-free */
    free(gb);
}

/* ── Consulta ───────────────────────────────────────────────── */

size_t gb_length(const GapBuffer *gb)
{
    return gb->buf_size - gap_size(gb);
}

size_t gb_cursor_pos(const GapBuffer *gb)
{
    return gb->gap_start;
}

char gb_char_at(const GapBuffer *gb, size_t pos)
{
    if (pos >= gb_length(gb)) return '\0';

    /* Posición lógica → posición física */
    if (pos < gb->gap_start)
        return gb->buffer[pos];
    else
        return gb->buffer[gb->gap_end + (pos - gb->gap_start)];
}

/* ── Navegación ─────────────────────────────────────────────── */

/*
 * gb_move_cursor: mueve el gap (y con él el cursor) a la posición
 * lógica `pos`. El texto no cambia, solo se reordena en el array.
 *
 * Mover a la izquierda: copiar bytes del pre-gap al post-gap.
 * Mover a la derecha:   copiar bytes del post-gap al pre-gap.
 */
void gb_move_cursor(GapBuffer *gb, size_t pos)
{
    size_t len = gb_length(gb);
    if (pos > len) pos = len;
    if (pos == gb->gap_start) return;

    if (pos < gb->gap_start) {
        /* Mover a la izquierda */
        size_t delta = gb->gap_start - pos;
        gb->gap_start -= delta;
        gb->gap_end   -= delta;
        memmove(gb->buffer + gb->gap_end,
                gb->buffer + gb->gap_start,
                delta);
    } else {
        /* Mover a la derecha */
        size_t delta = pos - gb->gap_start;
        memmove(gb->buffer + gb->gap_start,
                gb->buffer + gb->gap_end,
                delta);
        gb->gap_start += delta;
        gb->gap_end   += delta;
    }
}

/* ── Edición ────────────────────────────────────────────────── */

int gb_insert_char(GapBuffer *gb, char c)
{
    /* Si el gap está lleno, crecer */
    if (gap_size(gb) < 1) {
        if (grow(gb) != 0) return -1;
    }

    gb->buffer[gb->gap_start] = c;
    gb->gap_start++;
    return 0;
}

int gb_insert_str(GapBuffer *gb, const char *str, size_t len)
{
    /* Asegurar que hay espacio suficiente en el gap */
    while (gap_size(gb) < len) {
        if (grow(gb) != 0) return -1;
    }

    memcpy(gb->buffer + gb->gap_start, str, len);
    gb->gap_start += len;
    return 0;
}

int gb_delete_before(GapBuffer *gb)
{
    /* Backspace: eliminar el byte inmediatamente antes del gap */
    if (gb->gap_start == 0) return -1;   /* inicio del buffer */
    gb->gap_start--;
    return 0;
}

int gb_delete_after(GapBuffer *gb)
{
    /* Delete: eliminar el byte inmediatamente después del gap */
    if (gb->gap_end >= gb->buf_size) return -1;   /* fin del buffer */
    gb->gap_end++;
    return 0;
}

/* ── Exportación ────────────────────────────────────────────── */

char *gb_to_contiguous(const GapBuffer *gb, size_t *out_len)
{
    size_t len = gb_length(gb);
    char *result = malloc(len + 1);   /* +1 para '\0' */
    if (!result) return NULL;

    /* Copiar parte antes del gap */
    memcpy(result, gb->buffer, gb->gap_start);

    /* Copiar parte después del gap */
    memcpy(result + gb->gap_start,
           gb->buffer + gb->gap_end,
           gb->buf_size - gb->gap_end);

    result[len] = '\0';
    if (out_len) *out_len = len;
    return result;
}

/* ── Debug ──────────────────────────────────────────────────── */

void gb_print_state(const GapBuffer *gb)
{
    printf("[GapBuffer] buf_size=%zu gap_start=%zu gap_end=%zu text_len=%zu\n",
           gb->buf_size, gb->gap_start, gb->gap_end, gb_length(gb));

    printf("  Buffer físico: [");
    for (size_t i = 0; i < gb->buf_size && i < 40; i++) {
        if (i >= gb->gap_start && i < gb->gap_end)
            printf("_");
        else
            printf("%c", gb->buffer[i] >= 32 ? gb->buffer[i] : '?');
    }
    if (gb->buf_size > 40) printf("...");
    printf("]\n");

    size_t len = gb_length(gb);
    printf("  Texto lógico: [");
    for (size_t i = 0; i < len && i < 40; i++)
        printf("%c", gb_char_at(gb, i));
    if (len > 40) printf("...");
    printf("]\n");
}
