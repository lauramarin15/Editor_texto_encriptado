/* Compatibilidad macOS / Linux */
#if defined(__APPLE__)
#  define _DARWIN_C_SOURCE
#else
#  define _POSIX_C_SOURCE 200809L
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <errno.h>
#include "io.h"
#include "compress.h"
#include "format.h"

/* ── Helpers ─────────────────────────────────────────────────── */

const char *io_status_str(IOStatus s)
{
    switch (s) {
        case IO_OK:           return "OK";
        case IO_ERR_OPEN:     return "Error al abrir el archivo";
        case IO_ERR_WRITE:    return "Error al escribir";
        case IO_ERR_READ:     return "Error al leer";
        case IO_ERR_MMAP:     return "Error en mmap";
        case IO_ERR_FORMAT:   return "Formato inválido";
        case IO_ERR_MEMORY:   return "Error de memoria";
        case IO_ERR_CHECKSUM: return "Checksum inválido";
        default:              return "Error desconocido";
    }
}

/*
 * prepare_write: comprime el contenido del GapBuffer y prepara
 * el header listo para escribir a disco.
 *
 * Retorna el buffer comprimido (el caller libera) y llena *header.
 * Retorna NULL en error.
 */
static uint8_t *prepare_write(GapBuffer *gb, uint8_t flags,
                               lz4e_header_t *header, size_t *comp_size)
{
    /* Exportar el GapBuffer a un buffer contiguo */
    size_t text_len = 0;
    char *text = gb_to_contiguous(gb, &text_len);
    if (!text) return NULL;

    /* Comprimir en User Space antes de tocar el kernel */
    CompressResult cr = compress_auto((const uint8_t *)text, text_len);
    free(text);

    if (!cr.data) return NULL;

    /* Llenar el header */
    header_init(header);
    header->flags           = flags;
    header->algo_primary    = cr.algo;
    header->algo_secondary  = cr.algo2;
    header->size_original   = (uint32_t)text_len;
    header->size_compressed = (uint32_t)cr.size;
    header->checksum        = header_checksum(cr.data, cr.size);
    header->modified_at     = (uint64_t)time(NULL);

    if (flags & FLAG_RICH_TEXT)
        header->style_default = STYLE_NONE;

    *comp_size = cr.size;
    return cr.data;
}

/* ═══════════════════════════════════════════════════════════════
 * Implementación A: fd + write con buffers alineados a página
 *
 * Usamos posix_memalign para que el buffer esté alineado a 4KB.
 * Esto permite al kernel hacer DMA directo sin copias intermedias.
 * El resultado: menos context switches y menos syscalls write().
 * ═══════════════════════════════════════════════════════════════ */

IOStatus io_write_fd(const char *path, GapBuffer *gb, uint8_t flags)
{
    lz4e_header_t header;
    size_t comp_size = 0;

    /* 1. Comprimir en User Space (sin tocar el kernel todavía) */
    uint8_t *compressed = prepare_write(gb, flags & ~FLAG_MMAP_WRITTEN,
                                         &header, &comp_size);
    if (!compressed) return IO_ERR_MEMORY;

    /* 2. Abrir archivo (única syscall open) */
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        free(compressed);
        return IO_ERR_OPEN;
    }

    /* 3. Calcular tamaño total y preparar buffer alineado a página */
    size_t total = sizeof(lz4e_header_t) + comp_size;

    /*
     * posix_memalign: garantiza alineación a PAGE_SIZE.
     * Un buffer alineado permite al kernel hacer DMA directo
     * (zero-copy entre user space y el dispositivo).
     * Sin alineación, el kernel debe copiar a un buffer interno.
     */
    void *aligned_buf = NULL;
    aligned_buf = aligned_alloc(PAGE_SIZE,
                                (total + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));
    if (!aligned_buf) {
        close(fd);
        free(compressed);
        return IO_ERR_MEMORY;
    }

    /* 4. Ensamblar header + payload en el buffer alineado */
    memcpy(aligned_buf, &header, sizeof(lz4e_header_t));
    memcpy((uint8_t *)aligned_buf + sizeof(lz4e_header_t),
           compressed, comp_size);
    free(compressed);

    /* 5. Escribir en bloques de PAGE_SIZE (minimizar syscalls write) */
    size_t written    = 0;
    const uint8_t *p  = (const uint8_t *)aligned_buf;
    IOStatus status   = IO_OK;

    while (written < total) {
        size_t chunk = total - written;
        if (chunk > IO_BLOCK_SIZE) chunk = IO_BLOCK_SIZE;

        ssize_t n = write(fd, p + written, chunk);
        if (n < 0) {
            if (errno == EINTR) continue;   /* señal interrumpió, reintentar */
            status = IO_ERR_WRITE;
            break;
        }
        written += (size_t)n;
    }

    free(aligned_buf);
    close(fd);
    return status;
}

GapBuffer *io_read_fd(const char *path, IOStatus *status)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) { if (status) *status = IO_ERR_OPEN; return NULL; }

    /* Leer header */
    lz4e_header_t header;
    ssize_t n = read(fd, &header, sizeof(lz4e_header_t));
    if (n != (ssize_t)sizeof(lz4e_header_t)) {
        close(fd);
        if (status) *status = IO_ERR_READ;
        return NULL;
    }

    if (!header_validate(&header)) {
        close(fd);
        if (status) *status = IO_ERR_FORMAT;
        return NULL;
    }

    /* Leer payload comprimido en buffer alineado */
    size_t comp_size = header.size_compressed;
    void *aligned_buf = NULL;
    size_t alloc_size = (comp_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    aligned_buf = aligned_alloc(PAGE_SIZE, alloc_size);
    if (!aligned_buf) {
        close(fd);
        if (status) *status = IO_ERR_MEMORY;
        return NULL;
    }

    size_t total_read = 0;
    uint8_t *p = (uint8_t *)aligned_buf;
    while (total_read < comp_size) {
        size_t chunk = comp_size - total_read;
        if (chunk > IO_BLOCK_SIZE) chunk = IO_BLOCK_SIZE;
        n = read(fd, p + total_read, chunk);
        if (n < 0) { if (errno == EINTR) continue; break; }
        if (n == 0) break;
        total_read += (size_t)n;
    }
    close(fd);

    if (total_read != comp_size) {
        free(aligned_buf);
        if (status) *status = IO_ERR_READ;
        return NULL;
    }

    /* Verificar checksum */
    uint32_t ck = header_checksum(p, comp_size);
    if (ck != header.checksum) {
        free(aligned_buf);
        if (status) *status = IO_ERR_CHECKSUM;
        return NULL;
    }

    /* Descomprimir */
    size_t out_len = 0;
    uint8_t *text = decompress(p, comp_size,
                                header.algo_primary, header.algo_secondary,
                                header.size_original, &out_len);
    free(aligned_buf);

    if (!text) { if (status) *status = IO_ERR_MEMORY; return NULL; }

    GapBuffer *gb = gb_create_from((const char *)text, out_len);
    free(text);

    if (!gb) { if (status) *status = IO_ERR_MEMORY; return NULL; }
    if (status) *status = IO_OK;
    return gb;
}

/* ═══════════════════════════════════════════════════════════════
 * Implementación B: mmap
 *
 * mmap mapea el archivo directamente en el espacio de direcciones
 * del proceso. El kernel gestiona qué páginas están en RAM.
 *
 * Ventajas vs fd+write:
 *   - Cero copias entre user space y kernel para lecturas.
 *   - Una sola syscall mmap() en vez de N write().
 *   - El kernel puede usar page cache y prefetch.
 *
 * Desventajas:
 *   - Para archivos pequeños, el overhead de mmap puede ser mayor.
 *   - msync() puede bloquearse si la carga del disco es alta.
 * ═══════════════════════════════════════════════════════════════ */

IOStatus io_write_mmap(const char *path, GapBuffer *gb, uint8_t flags)
{
    lz4e_header_t header;
    size_t comp_size = 0;

    /* Comprimir en User Space */
    uint8_t *compressed = prepare_write(gb,
                                         flags | FLAG_MMAP_WRITTEN,
                                         &header, &comp_size);
    if (!compressed) return IO_ERR_MEMORY;

    size_t total = sizeof(lz4e_header_t) + comp_size;

    /* Crear/truncar archivo al tamaño exacto */
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { free(compressed); return IO_ERR_OPEN; }

    /* ftruncate: define el tamaño del archivo antes de mmap */
    if (ftruncate(fd, (off_t)total) < 0) {
        close(fd);
        free(compressed);
        return IO_ERR_WRITE;
    }

    /* Mapear el archivo en memoria (MAP_SHARED: cambios se reflejan en disco) */
    void *map = mmap(NULL, total, PROT_READ | PROT_WRITE,
                     MAP_SHARED, fd, 0);
    close(fd);   /* el fd puede cerrarse; el mapping permanece */

    if (map == MAP_FAILED) {
        free(compressed);
        return IO_ERR_MMAP;
    }

    /* Escribir header + payload directamente en la memoria mapeada */
    memcpy(map, &header, sizeof(lz4e_header_t));
    memcpy((uint8_t *)map + sizeof(lz4e_header_t), compressed, comp_size);
    free(compressed);

    /*
     * msync(MS_SYNC): fuerza el flush de las páginas modificadas al disco.
     * Sin esto, el kernel puede retrasar la escritura indefinidamente.
     */
    if (msync(map, total, MS_SYNC) < 0) {
        munmap(map, total);
        return IO_ERR_WRITE;
    }

    munmap(map, total);
    return IO_OK;
}

GapBuffer *io_read_mmap(const char *path, IOStatus *status)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) { if (status) *status = IO_ERR_OPEN; return NULL; }

    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size < (off_t)sizeof(lz4e_header_t)) {
        close(fd);
        if (status) *status = IO_ERR_FORMAT;
        return NULL;
    }

    size_t file_size = (size_t)st.st_size;

    /* Mapear todo el archivo en modo solo lectura */
    void *map = mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);

    if (map == MAP_FAILED) {
        if (status) *status = IO_ERR_MMAP;
        return NULL;
    }

    /* Leer header directamente desde la memoria mapeada */
    lz4e_header_t *header = (lz4e_header_t *)map;

    if (!header_validate(header)) {
        munmap(map, file_size);
        if (status) *status = IO_ERR_FORMAT;
        return NULL;
    }

    size_t comp_size = header->size_compressed;
    const uint8_t *payload = (const uint8_t *)map + sizeof(lz4e_header_t);

    /* Verificar checksum */
    uint32_t ck = header_checksum(payload, comp_size);
    if (ck != header->checksum) {
        munmap(map, file_size);
        if (status) *status = IO_ERR_CHECKSUM;
        return NULL;
    }

    /* Descomprimir */
    size_t out_len = 0;
    uint8_t *text = decompress(payload, comp_size,
                                header->algo_primary, header->algo_secondary,
                                header->size_original, &out_len);

    munmap(map, file_size);

    if (!text) { if (status) *status = IO_ERR_MEMORY; return NULL; }

    GapBuffer *gb = gb_create_from((const char *)text, out_len);
    free(text);

    if (!gb) { if (status) *status = IO_ERR_MEMORY; return NULL; }
    if (status) *status = IO_OK;
    return gb;
}

/* ── editor info ─────────────────────────────────────────────── */

IOStatus io_file_info(const char *path, lz4e_header_t *out_header)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return IO_ERR_OPEN;

    ssize_t n = read(fd, out_header, sizeof(lz4e_header_t));
    close(fd);

    if (n != (ssize_t)sizeof(lz4e_header_t)) return IO_ERR_READ;
    if (!header_validate(out_header))          return IO_ERR_FORMAT;

    return IO_OK;
}
