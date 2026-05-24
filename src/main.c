/*
 * main.c — Editor de texto CLI con compresión híbrida + encriptación RC4
 *
 * Comandos:
 *   editor new  <archivo> [--encrypt --key <clave>]   Crea archivo nuevo
 *   editor open <archivo> [--key <clave>]             Descomprime y abre
 *   editor view <archivo> [--key <clave>]             Muestra el contenido
 *   editor info <archivo>                             Muestra metadatos
 *
 * El editor opera en modos:
 *   INSERT  → escribir texto
 *   COMMAND → comandos (:w guardar, :q salir, :wq guardar y salir,
 *                        :g<N> ir a línea, :d borrar línea)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include "gap_buffer.h"
#include "io.h"
#include "format.h"
#include "compress.h"
#include "crypto.h"

/* ── Modo I/O: puede cambiarse para el benchmark ────────────── */
#ifndef USE_MMAP
#define USE_MMAP 0   /* 0 = fd+write, 1 = mmap */
#endif

/* ── Terminal en modo raw ────────────────────────────────────── */
static struct termios orig_termios;

static void disable_raw_mode(void)
{
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

static int enable_raw_mode(void)
{
    if (tcgetattr(STDIN_FILENO, &orig_termios) < 0) return -1;
    atexit(disable_raw_mode);

    struct termios raw = orig_termios;
    raw.c_lflag    &= ~(ECHO | ICANON | ISIG);
    raw.c_iflag    &= ~(IXON | ICRNL);
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;
    return tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

/* ── Pantalla ─────────────────────────────────────────────────── */
#define CLEAR_SCREEN "\x1b[2J\x1b[H"
#define MOVE_HOME    "\x1b[H"

static void render(GapBuffer *gb, const char *filename,
                   int mode_insert, const char *status_msg,
                   int encrypted)
{
    { int _r = write(STDOUT_FILENO, CLEAR_SCREEN, sizeof(CLEAR_SCREEN) - 1); (void)_r; }

    /* Barra de título — muestra [ENC] si el archivo está encriptado */
    printf("\x1b[7m %-35s %s %s \x1b[0m\n",
           filename,
           encrypted ? "[ENC]" : "     ",
           mode_insert ? "-- INSERT --" : "-- COMMAND --");

    /* Contenido */
    size_t len = gb_length(gb);
    size_t cursor = gb_cursor_pos(gb);
    for (size_t i = 0; i < len; i++) {
        char c = gb_char_at(gb, i);
        if (i == cursor) printf("\x1b[7m");   /* resaltar cursor */
        if (c == '\n') {
            if (i == cursor) printf(" \x1b[0m");
            printf("\n");
        } else {
            printf("%c", c);
            if (i == cursor) printf("\x1b[0m");
        }
    }
    if (cursor == len) printf("\x1b[7m \x1b[0m");

    /* Barra de estado */
    printf("\n\x1b[7m %-70s \x1b[0m\n",
           status_msg ? status_msg
                      : "INSERT: escribir | ESC: modo comando | :w guardar | :q salir");
    fflush(stdout);
}

/* ── Guardar — pasa flags y llave ────────────────────────────── */
static IOStatus do_save(GapBuffer *gb, const char *path,
                        uint8_t flags,
                        const uint8_t *key, size_t key_len)
{
#if USE_MMAP
    return io_write_mmap(path, gb, flags, key, key_len);
#else
    return io_write_fd(path, gb, flags, key, key_len);
#endif
}

/* ── Bucle principal del editor ──────────────────────────────── */
static int run_editor(GapBuffer *gb, const char *filename,
                      uint8_t flags,
                      const uint8_t *key, size_t key_len)
{
    int mode_insert = 1;
    char status_msg[128] = "";
    char cmd_buf[64]     = "";
    int  cmd_len         = 0;
    int  encrypted       = (flags & FLAG_ENCRYPTED) != 0;

    if (enable_raw_mode() < 0) {
        fprintf(stderr, "No se pudo habilitar modo raw\n");
        return 1;
    }

    while (1) {
        render(gb, filename, mode_insert,
               status_msg[0] ? status_msg : NULL, encrypted);
        status_msg[0] = '\0';

        char c;
        if (read(STDIN_FILENO, &c, 1) != 1) break;

        if (mode_insert) {
            if (c == 27) {   /* ESC → modo comando */
                mode_insert = 0;
                cmd_len = 0;
                cmd_buf[0] = '\0';
            } else if (c == 127 || c == '\b') {
                gb_delete_before(gb);
            } else if (c == '\r') {
                gb_insert_char(gb, '\n');
            } else {
                gb_insert_char(gb, c);
            }
        } else {
            /* Modo comando: acumular caracteres */
            if (c == 'i') {
                mode_insert = 1;
            } else if (c == 27) {
                cmd_len = 0;
                cmd_buf[0] = '\0';
            } else if (c == '\r' || c == '\n') {
                /* Ejecutar comando acumulado */
                cmd_buf[cmd_len] = '\0';

                if (strcmp(cmd_buf, ":w") == 0) {
                    IOStatus s = do_save(gb, filename, flags, key, key_len);
                    snprintf(status_msg, sizeof(status_msg),
                             s == IO_OK ? "Guardado: %s" : "Error: %s",
                             s == IO_OK ? filename : io_status_str(s));
                } else if (strcmp(cmd_buf, ":q") == 0) {
                    disable_raw_mode();
                    { int _r = write(STDOUT_FILENO, CLEAR_SCREEN, sizeof(CLEAR_SCREEN)-1); (void)_r; }
                    return 0;
                } else if (strcmp(cmd_buf, ":wq") == 0) {
                    IOStatus s = do_save(gb, filename, flags, key, key_len);
                    if (s != IO_OK)
                        snprintf(status_msg, sizeof(status_msg),
                                 "Error al guardar: %s", io_status_str(s));
                    disable_raw_mode();
                    { int _r = write(STDOUT_FILENO, CLEAR_SCREEN, sizeof(CLEAR_SCREEN)-1); (void)_r; }
                    return (s == IO_OK) ? 0 : 1;
                } else if (strncmp(cmd_buf, ":g", 2) == 0) {
                    /* :gN → ir a la línea N */
                    int target_line = atoi(cmd_buf + 2);
                    if (target_line > 0) {
                        int line = 1;
                        size_t len = gb_length(gb);
                        for (size_t i = 0; i < len; i++) {
                            if (line == target_line) {
                                gb_move_cursor(gb, i);
                                break;
                            }
                            if (gb_char_at(gb, i) == '\n') line++;
                        }
                        snprintf(status_msg, sizeof(status_msg),
                                 "Línea %d", target_line);
                    }
                } else if (strcmp(cmd_buf, ":d") == 0) {
                    /* Borrar línea actual */
                    size_t pos = gb_cursor_pos(gb);
                    /* Ir al inicio de la línea */
                    while (pos > 0 && gb_char_at(gb, pos - 1) != '\n')
                        pos--;
                    gb_move_cursor(gb, pos);
                    /* Borrar hasta el siguiente \n */
                    while (gb_cursor_pos(gb) < gb_length(gb)) {
                        char ch = gb_char_at(gb, gb_cursor_pos(gb));
                        gb_delete_after(gb);
                        if (ch == '\n') break;
                    }
                    snprintf(status_msg, sizeof(status_msg), "Línea eliminada");
                } else {
                    snprintf(status_msg, sizeof(status_msg),
                             "Comando desconocido: %s", cmd_buf);
                }

                cmd_len = 0;
                cmd_buf[0] = '\0';

            } else if (c == 127 || c == '\b') {
                if (cmd_len > 0) cmd_buf[--cmd_len] = '\0';
            } else {
                /* Acumular tecla de navegación */
                if (c == 'h') { gb_move_cursor(gb, gb_cursor_pos(gb) > 0 ? gb_cursor_pos(gb)-1 : 0); }
                else if (c == 'l') { gb_move_cursor(gb, gb_cursor_pos(gb)+1); }
                else if (c == 'k') { /* arriba: buscar \n anterior */ }
                else if (c == 'j') { /* abajo: buscar \n siguiente */ }
                else if (cmd_len < 62) {
                    cmd_buf[cmd_len++] = c;
                    cmd_buf[cmd_len]   = '\0';
                }
            }
        }
    }

    disable_raw_mode();
    return 0;
}

/* ── Comandos ─────────────────────────────────────────────────── */

static void print_usage(const char *prog)
{
    printf("Uso: %s <comando> <archivo> [opciones]\n\n", prog);
    printf("Comandos:\n");
    printf("  new  <archivo> [--encrypt --key <clave>]   Crear archivo nuevo\n");
    printf("  open <archivo> [--key <clave>]             Abrir archivo existente\n");
    printf("  view <archivo> [--key <clave>]             Ver contenido (sin editar)\n");
    printf("  info <archivo>                             Mostrar metadatos del header\n\n");
    printf("Opciones:\n");
    printf("  --encrypt          Activar encriptacion RC4\n");
    printf("  --key <contrasena> Contrasena (minimo 8 caracteres)\n\n");
    printf("Ejemplos:\n");
    printf("  %s new doc.lz4e --encrypt --key miClave123\n", prog);
    printf("  %s open doc.lz4e --key miClave123\n", prog);
    printf("  %s view doc.lz4e --key miClave123\n", prog);
    printf("\nEn el editor:\n");
    printf("  i          → modo insertar\n");
    printf("  ESC        → modo comando\n");
    printf("  :w         → guardar\n");
    printf("  :q         → salir\n");
    printf("  :wq        → guardar y salir\n");
    printf("  :gN        → ir a línea N\n");
    printf("  :d         → borrar línea actual\n");
    printf("  h/l        → mover cursor izq/der\n");
}

int main(int argc, char *argv[])
{
    if (argc < 3) {
        print_usage(argv[0]);
        return 1;
    }

    const char *cmd  = argv[1];
    const char *path = argv[2];

    /* Detectar flags --encrypt y --key */
    uint8_t flags   = FLAG_NONE;
    uint8_t key[RC4_KEY_MAX];
    size_t  key_len = 0;
    memset(key, 0, sizeof(key));

    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--encrypt") || !strcmp(argv[i], "-e"))
            flags |= FLAG_ENCRYPTED;
        if ((!strcmp(argv[i], "--key") || !strcmp(argv[i], "-k")) && i+1 < argc) {
            i++;
            key_len = strlen(argv[i]);
            if (key_len > RC4_KEY_MAX) key_len = RC4_KEY_MAX;
            memcpy(key, argv[i], key_len);
            /* Borrar la clave de argv para que no quede en memoria */
            volatile char *p = (volatile char *)argv[i];
            size_t n = strlen(argv[i]); while (n--) *p++ = 0;
        }
    }

    /* Verificar que si hay --encrypt también hay --key */
    if ((flags & FLAG_ENCRYPTED) && key_len == 0) {
        fprintf(stderr, "Error: --encrypt requiere --key <contrasena>\n");
        fprintf(stderr, "Ejemplo: %s new archivo.lz4e --encrypt --key miClave123\n", argv[0]);
        return 1;
    }
    if (key_len > 0 && key_len < RC4_KEY_MIN) {
        fprintf(stderr, "Error: la contrasena debe tener al menos %d caracteres\n", RC4_KEY_MIN);
        return 1;
    }

    /* ── editor new ─── */
    if (strcmp(cmd, "new") == 0) {
        if (flags & FLAG_ENCRYPTED)
            fprintf(stderr,
                "[info] pipeline: texto -> comprimir -> encriptar (RC4) -> disco\n");
        GapBuffer *gb = gb_create();
        if (!gb) { fprintf(stderr, "Error: no se pudo crear el buffer\n"); return 1; }
        int ret = run_editor(gb, path, flags, key, key_len);
        gb_destroy(gb);
        /* Borrar clave de memoria */
        volatile uint8_t *p = (volatile uint8_t *)key;
        size_t n = sizeof(key); while (n--) *p++ = 0;
        return ret;
    }

    /* ── editor open ─── */
    if (strcmp(cmd, "open") == 0) {
        /* Detectar si el archivo está encriptado */
        lz4e_header_t hdr;
        io_file_info(path, &hdr);
        if ((hdr.flags & FLAG_ENCRYPTED) && key_len == 0) {
            fprintf(stderr, "Error: archivo encriptado, usar --key <contrasena>\n");
            return 1;
        }
        IOStatus status;
#if USE_MMAP
        GapBuffer *gb = io_read_mmap(path, &status, key, key_len);
#else
        GapBuffer *gb = io_read_fd(path, &status, key, key_len);
#endif
        if (!gb) {
            fprintf(stderr, "Error al abrir '%s': %s\n", path, io_status_str(status));
            return 1;
        }
        int ret = run_editor(gb, path, hdr.flags, key, key_len);
        gb_destroy(gb);
        volatile uint8_t *p = (volatile uint8_t *)key;
        size_t n = sizeof(key); while (n--) *p++ = 0;
        return ret;
    }

    /* ── editor view ─── */
    if (strcmp(cmd, "view") == 0) {
        lz4e_header_t hdr;
        io_file_info(path, &hdr);
        if ((hdr.flags & FLAG_ENCRYPTED) && key_len == 0) {
            fprintf(stderr, "Error: archivo encriptado, usar --key <contrasena>\n");
            return 1;
        }
        IOStatus status;
#if USE_MMAP
        GapBuffer *gb = io_read_mmap(path, &status, key, key_len);
#else
        GapBuffer *gb = io_read_fd(path, &status, key, key_len);
#endif
        if (!gb) {
            fprintf(stderr, "Error al abrir '%s': %s\n", path, io_status_str(status));
            return 1;
        }
        size_t len = gb_length(gb);
        for (size_t i = 0; i < len; i++)
            putchar(gb_char_at(gb, i));
        putchar('\n');
        gb_destroy(gb);
        volatile uint8_t *p = (volatile uint8_t *)key;
        size_t n = sizeof(key); while (n--) *p++ = 0;
        return 0;
    }

    /* ── editor info ─── */
    if (strcmp(cmd, "info") == 0) {
        lz4e_header_t header;
        IOStatus s = io_file_info(path, &header);
        if (s != IO_OK) {
            fprintf(stderr, "Error al leer '%s': %s\n", path, io_status_str(s));
            return 1;
        }
        header_print(&header);
        return 0;
    }

    fprintf(stderr, "Comando desconocido: %s\n\n", cmd);
    print_usage(argv[0]);
    return 1;
}
