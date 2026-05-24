#ifndef IO_H
#define IO_H

/*
 * io.h — API de I/O con dos implementaciones comparables
 *
 * Implementación A: fd + write con buffers alineados a página (4KB)
 * Implementación B: mmap para escritura y lectura
 *
 * Ambas escriben el mismo formato .lz4e.
 * El benchmark en benchmark.sh mide cuál es más eficiente con strace y time.
 *
 * Regla de oro:
 *   - Usar buffers alineados a PAGE_SIZE (4096 bytes) para reducir
 *     el número de syscalls write() y context switches.
 *   - Con mmap, el kernel gestiona el flushing de páginas.
 */

#include <stddef.h>
#include <stdint.h>
#include "format.h"
#include "gap_buffer.h"
#include "compress.h"

/* ── Resultado de operaciones I/O ───────────────────────────── */
typedef enum {
    IO_OK           =  0,
    IO_ERR_OPEN     = -1,
    IO_ERR_WRITE    = -2,
    IO_ERR_READ     = -3,
    IO_ERR_MMAP     = -4,
    IO_ERR_FORMAT   = -5,
    IO_ERR_MEMORY   = -6,
    IO_ERR_CHECKSUM = -7,
} IOStatus;

/* ── Implementación A: fd + write alineado a página ─────────── */

/*
 * io_write_fd: comprime el contenido del GapBuffer y lo escribe
 * usando open() + write() con buffers de PAGE_SIZE bytes.
 *
 * El buffer de escritura se alinea con posix_memalign() al tamaño
 * de página para minimizar el número de syscalls.
 */
IOStatus io_write_fd(const char *path, GapBuffer *gb, uint8_t flags);

/*
 * io_read_fd: lee el archivo .lz4e con read() en bloques de PAGE_SIZE,
 * descomprime y carga el contenido en un GapBuffer nuevo.
 * El caller es responsable de llamar gb_destroy() sobre el resultado.
 */
GapBuffer *io_read_fd(const char *path, IOStatus *status);

/* ── Implementación B: mmap ──────────────────────────────────── */

/*
 * io_write_mmap: comprime el contenido del GapBuffer y lo escribe
 * usando mmap() + msync(). El kernel decide cuándo hacer el flush
 * a disco, reduciendo context switches.
 */
IOStatus io_write_mmap(const char *path, GapBuffer *gb, uint8_t flags);

/*
 * io_read_mmap: mapea el archivo en memoria con mmap(MAP_PRIVATE),
 * descomprime y carga en GapBuffer.
 */
GapBuffer *io_read_mmap(const char *path, IOStatus *status);

/* ── Helpers ─────────────────────────────────────────────────── */
const char *io_status_str(IOStatus s);

/*
 * io_file_info: lee el header de un .lz4e sin descomprimir el payload.
 * Útil para el comando `editor info`.
 */
IOStatus io_file_info(const char *path, lz4e_header_t *out_header);

#endif /* IO_H */
