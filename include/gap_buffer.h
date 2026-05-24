#ifndef GAP_BUFFER_H
#define GAP_BUFFER_H

/*
 * gap_buffer.h — Estructura Gap Buffer para edición de texto
 *
 * Un Gap Buffer es un array con un "hueco" (gap) en la posición
 * del cursor. Insertar y borrar en el cursor es O(1).
 *
 * Representación interna:
 *
 *   "Hola mundo"  con cursor después de "Hola":
 *
 *   [ H ][ o ][ l ][ a ][ _ ][ _ ][ _ ][ _ ][ m ][ u ][ n ][ d ][ o ]
 *    0    1    2    3    4    5    6    7    8    9   10   11   12
 *                        ↑ gap_start              ↑ gap_end
 *
 *   gap_start = 4  (primer byte del gap)
 *   gap_end   = 8  (primer byte después del gap)
 *   gap_size  = gap_end - gap_start = 4
 *
 * Insertar 'X' en cursor:
 *   buffer[gap_start] = 'X'
 *   gap_start++       → O(1)
 *
 * Mover cursor a la derecha:
 *   buffer[gap_start] = buffer[gap_end]
 *   gap_start++; gap_end++  → O(1)
 *
 * Mover cursor N posiciones: O(N) — se mueven bytes al gap.
 */

#include <stddef.h>
#include <stdint.h>

#define GAP_BUFFER_INITIAL_SIZE  4096   /* tamaño inicial del buffer     */
#define GAP_BUFFER_MIN_GAP       256    /* gap mínimo antes de crecer    */
#define GAP_BUFFER_GROW_FACTOR   2      /* factor de crecimiento         */

typedef struct {
    char   *buffer;      /* array interno (heap)                         */
    size_t  buf_size;    /* tamaño total del array (incluyendo gap)       */
    size_t  gap_start;   /* índice del primer byte del gap                */
    size_t  gap_end;     /* índice del primer byte después del gap        */
} GapBuffer;

/* ── Ciclo de vida ──────────────────────────────────────────── */
GapBuffer *gb_create(void);
GapBuffer *gb_create_from(const char *text, size_t len);
void       gb_destroy(GapBuffer *gb);

/* ── Navegación del cursor ──────────────────────────────────── */
void   gb_move_cursor(GapBuffer *gb, size_t pos);   /* posición absoluta en texto lógico */
size_t gb_cursor_pos(const GapBuffer *gb);          /* posición actual del cursor        */

/* ── Edición ────────────────────────────────────────────────── */
int  gb_insert_char(GapBuffer *gb, char c);
int  gb_insert_str(GapBuffer *gb, const char *str, size_t len);
int  gb_delete_before(GapBuffer *gb);               /* backspace */
int  gb_delete_after(GapBuffer *gb);                /* delete    */

/* ── Consulta ───────────────────────────────────────────────── */
size_t gb_length(const GapBuffer *gb);              /* longitud lógica del texto         */
char   gb_char_at(const GapBuffer *gb, size_t pos); /* caracter en posición lógica       */

/* ── Exportación ────────────────────────────────────────────── */
/*
 * gb_to_contiguous: copia el texto lógico (sin gap) a un buffer contiguo.
 * El caller es responsable de liberar la memoria devuelta.
 * Retorna NULL en error. *out_len recibe la longitud sin '\0'.
 */
char *gb_to_contiguous(const GapBuffer *gb, size_t *out_len);

/* ── Debug ──────────────────────────────────────────────────── */
void gb_print_state(const GapBuffer *gb);

#endif /* GAP_BUFFER_H */
