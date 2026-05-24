/*
 * crypto.c — Implementación RC4 desde cero + gestión segura de llaves
 *
 * RC4 funciona en dos fases:
 *
 * Fase 1 — KSA (Key Scheduling Algorithm):
 *   Inicializa S[256] = [0,1,2,...,255] y lo permuta con la llave.
 *   Sin la llave, no puedes reproducir la tabla S.
 *
 * Fase 2 — PRGA (Pseudo-Random Generation Algorithm):
 *   Por cada byte de datos genera un byte de keystream desde S
 *   y hace XOR con el dato. XOR es reversible:
 *
 *   dato     = 0x48 ('H')
 *   keystream= 0xAB
 *   cifrado  = 0x48 ^ 0xAB = 0xE3
 *
 *   cifrado  = 0xE3
 *   keystream= 0xAB  (misma llave = mismo keystream)
 *   original = 0xE3 ^ 0xAB = 0x48  recuperado
 */

#if defined(__APPLE__)
#  define _DARWIN_C_SOURCE
#else
#  define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include "crypto.h"

/*
 * secure_erase_mem: borra memoria de forma segura.
 *
 * Usamos volatile para que el compilador NO pueda eliminar
 * este loop como "optimizacion de dead store".
 * Es equivalente a explicit_bzero() en cualquier plataforma.
 *
 * La diferencia con memset:
 *   memset(p, 0, n)  → el compilador PUEDE eliminarlo si detecta
 *                       que la memoria no se usa despues
 *   volatile loop    → el compilador NUNCA puede eliminarlo
 */
static void secure_erase_mem(void *ptr, size_t len)
{
    volatile uint8_t *p = (volatile uint8_t *)ptr;
    size_t n = len;
    while (n--) *p++ = 0;
}

/* ── Fase 1: KSA ─────────────────────────────────────────────────
 *
 * Inicializa S[256] = [0..255] y lo permuta con la llave.
 * La permutacion mezcla S de forma que depende completamente
 * de la llave.
 */
void rc4_init(RC4State *state, const uint8_t *key, size_t key_len)
{
    uint8_t *S = state->S;

    for (int i = 0; i < 256; i++)
        S[i] = (uint8_t)i;

    uint8_t j = 0;
    for (int i = 0; i < 256; i++) {
        j = j + S[i] + key[i % key_len];
        uint8_t tmp = S[i];
        S[i]        = S[j];
        S[j]        = tmp;
    }

    state->i = 0;
    state->j = 0;
}

/* ── Fase 2: PRGA ────────────────────────────────────────────────
 *
 * Por cada byte:
 *   1. Avanzar i y j
 *   2. Swap en S
 *   3. Generar byte de keystream
 *   4. XOR con el dato
 *
 * Opera in-place — modifica buf directamente.
 * No aloca memoria.
 */
void rc4_process(RC4State *state, uint8_t *buf, size_t len)
{
    uint8_t *S = state->S;
    uint8_t  i = state->i;
    uint8_t  j = state->j;

    for (size_t k = 0; k < len; k++) {
        i++;
        j += S[i];

        uint8_t tmp = S[i];
        S[i]        = S[j];
        S[j]        = tmp;

        uint8_t keystream = S[(uint8_t)(S[i] + S[j])];
        buf[k] ^= keystream;
    }

    state->i = i;
    state->j = j;
}

/* ── Borrado seguro ──────────────────────────────────────────────
 *
 * Usa volatile loop — el compilador no puede eliminarlo.
 * Luego libera el bloqueo de pagina con munlock.
 */
void secure_key_erase(void *key, size_t len)
{
    secure_erase_mem(key, len);
    munlock(key, len);
}

/* ── Bloqueo de pagina en RAM ────────────────────────────────────
 *
 * mlock() le dice al kernel: esta pagina NO puede ir al swap.
 * Sin mlock, la llave podria quedar escrita en el disco
 * aunque la borremos de la RAM despues.
 */
int secure_key_lock(void *key, size_t len)
{
    if (mlock(key, len) != 0) {
        fprintf(stderr,
            "[crypto] advertencia: mlock() fallo — "
            "la llave podria ir al swap\n");
        return -1;
    }
    return 0;
}

/* ── Pipeline completo de encriptacion ──────────────────────────
 *
 * Orden:
 *   1. rc4_init()         inicializar con la llave (KSA)
 *   2. rc4_process()      encriptar el buffer (PRGA + XOR)
 *   3. secure_erase_mem   borrar el estado RC4 (contiene S[256])
 *   4. secure_key_erase   borrar la llave
 *
 * El caller NO debe usar key despues de esta llamada.
 */
void crypto_encrypt(uint8_t *buf, size_t len,
                    uint8_t *key, size_t key_len)
{
    RC4State state;

    rc4_init(&state, key, key_len);
    rc4_process(&state, buf, len);

    /* Borrar el estado — contiene la tabla S que permite
     * reconstruir el keystream */
    secure_erase_mem(&state, sizeof(RC4State));

    /* Borrar la llave del caller */
    secure_key_erase(key, key_len);
}

/* RC4 es simetrico — la misma operacion cifra y descifra */
void crypto_decrypt(uint8_t *buf, size_t len,
                    uint8_t *key, size_t key_len)
{
    crypto_encrypt(buf, len, key, key_len);
}

/* ── Leer llave por consola ──────────────────────────────────────
 *
 * getpass() lee sin mostrar los caracteres en pantalla.
 * Copiamos inmediatamente a out_key (que esta mlocked)
 * y borramos el buffer interno de getpass.
 */
size_t crypto_read_key(uint8_t *out_key, size_t max_len,
                       const char *prompt)
{
    /* Mostrar el prompt */
    if (prompt) {
        write(STDERR_FILENO, "\r\n", 2);
        write(STDERR_FILENO, prompt, strlen(prompt));
    }

    /* Leer byte a byte hasta Enter, sin mostrar caracteres */
    size_t len = 0;
    char c;
    while (len < max_len) {
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n <= 0) break;
        if (c == '\r' || c == '\n') break;
        if (c == 127 || c == '\b') {
            /* Backspace */
            if (len > 0) len--;
            continue;
        }
        out_key[len++] = (uint8_t)c;
    }
    write(STDERR_FILENO, "\r\n", 2);

    if (len < RC4_KEY_MIN) {
        fprintf(stderr, "[crypto] error: minimo %d caracteres\n",
                RC4_KEY_MIN);
        secure_erase_mem(out_key, len);
        return 0;
    }

    return len;
}
