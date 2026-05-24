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
#include "crypto.h"

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
 * Pipeline mandatorio: COMPRIMIR -> ENCRIPTAR (nunca al revés)
 * La encriptación destruye los patrones que los algoritmos de
 * compresión necesitan — si encriptamos primero, no se comprime nada.
 *
 * Retorna el buffer comprimido (el caller libera) y llena *header.
 * Retorna NULL en error.
 */
static uint8_t *prepare_write(GapBuffer *gb, uint8_t flags,
                               lz4e_header_t *header, size_t *comp_size,
                               const uint8_t *key, size_t key_len)
{
    /* Exportar el GapBuffer a un buffer contiguo */
    size_t text_len = 0;
    char *text = gb_to_contiguous(gb, &text_len);
    if (!text) return NULL;

    /* Comprimir en User Space antes de tocar el kernel
     * Medir tiempo aislado (Criterio 3 de la rúbrica) */
    struct timespec t0, t1, t2, t3;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    CompressResult cr = compress_auto((const uint8_t *)text, text_len);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms_compress = (t1.tv_sec - t0.tv_sec) * 1000.0
                       + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    free(text);

    if (!cr.data) return NULL;

    /* Encriptar si FLAG_ENCRYPTED — medir tiempo aislado
     *
     * ORDEN CORRECTO: comprimir → encriptar
     * Los datos comprimidos tienen patrones eliminados,
     * la encriptación opera sobre menos bytes. */
    double ms_encrypt = 0.0;
    if ((flags & FLAG_ENCRYPTED) && key_len > 0) {
        /*
         * Copiar llave a buffer local bloqueado en RAM.
         * mlock: el kernel NO puede mandar esta página al swap.
         * Sin mlock, la llave podría quedar en disco si hay
         * presión de memoria antes de que la borremos.
         */
        uint8_t local_key[RC4_KEY_MAX];
        memset(local_key, 0, sizeof(local_key));
        secure_key_lock(local_key, sizeof(local_key));
        size_t klen = key_len < RC4_KEY_MAX ? key_len : RC4_KEY_MAX;
        memcpy(local_key, key, klen);

        clock_gettime(CLOCK_MONOTONIC, &t2);
        crypto_encrypt(cr.data, cr.size, local_key, klen);
        clock_gettime(CLOCK_MONOTONIC, &t3);
        ms_encrypt = (t3.tv_sec - t2.tv_sec) * 1000.0
                   + (t3.tv_nsec - t2.tv_nsec) / 1e6;
        /* local_key borrada por crypto_encrypt con volatile loop */
    }

    /* Imprimir tiempos aislados para el benchmark */
    fprintf(stderr,
        "[io] compresion: %.3f ms | encriptacion: %.3f ms | total CPU: %.3f ms\n",
        ms_compress, ms_encrypt, ms_compress + ms_encrypt);

    /* Llenar el header */
    header_init(header);
    header->flags           = flags;
    header->algo_primary    = cr.algo;
    header->algo_secondary  = cr.algo2;
    header->size_original   = (uint32_t)text_len;
    header->size_compressed = (uint32_t)cr.size;
    /* Checksum sobre el payload final (ya encriptado si aplica) */
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

IOStatus io_write_fd(const char *path, GapBuffer *gb, uint8_t flags,
                     const uint8_t *key, size_t key_len)
{
    lz4e_header_t header;
    size_t comp_size = 0;

    /* 1. Comprimir en User Space (sin tocar el kernel todavía) */
    uint8_t *compressed = prepare_write(gb, flags & ~FLAG_MMAP_WRITTEN,
                                         &header, &comp_size, key, key_len);
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
     * aligned_alloc: garantiza alineación a PAGE_SIZE.
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

GapBuffer *io_read_fd(const char *path, IOStatus *status,
                      const uint8_t *key, size_t key_len)
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

    /* Verificar checksum ANTES de desencriptar */
    uint32_t ck = header_checksum(p, comp_size);
    if (ck != header.checksum) {
        free(aligned_buf);
        if (status) *status = IO_ERR_CHECKSUM;
        return NULL;
    }

    /* Desencriptar ANTES de descomprimir */
    if ((header.flags & FLAG_ENCRYPTED) && key_len > 0) {
        uint8_t local_key[RC4_KEY_MAX];
        memset(local_key, 0, sizeof(local_key));
        secure_key_lock(local_key, sizeof(local_key));
        size_t klen = key_len < RC4_KEY_MAX ? key_len : RC4_KEY_MAX;
        memcpy(local_key, key, klen);
        crypto_decrypt(p, comp_size, local_key, klen);
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

IOStatus io_write_mmap(const char *path, GapBuffer *gb, uint8_t flags,
                       const uint8_t *key, size_t key_len)
{
    lz4e_header_t header;
    size_t comp_size = 0;

    /* Comprimir en User Space */
    uint8_t *compressed = prepare_write(gb, flags | FLAG_MMAP_WRITTEN,
                                         &header, &comp_size, key, key_len);
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

GapBuffer *io_read_mmap(const char *path, IOStatus *status,
                        const uint8_t *key, size_t key_len)
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

    /* Mapear todo el archivo — MAP_PRIVATE da una copia modificable */
    void *map = mmap(NULL, file_size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE, fd, 0);
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
    uint8_t *payload = (uint8_t *)map + sizeof(lz4e_header_t);

    /* Verificar checksum ANTES de desencriptar */
    uint32_t ck = header_checksum(payload, comp_size);
    if (ck != header->checksum) {
        munmap(map, file_size);
        if (status) *status = IO_ERR_CHECKSUM;
        return NULL;
    }

    /* Desencriptar ANTES de descomprimir */
    if ((header->flags & FLAG_ENCRYPTED) && key_len > 0) {
        uint8_t local_key[RC4_KEY_MAX];
        memset(local_key, 0, sizeof(local_key));
        secure_key_lock(local_key, sizeof(local_key));
        size_t klen = key_len < RC4_KEY_MAX ? key_len : RC4_KEY_MAX;
        memcpy(local_key, key, klen);
        crypto_decrypt(payload, comp_size, local_key, klen);
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