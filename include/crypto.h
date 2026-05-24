#ifndef CRYPTO_H
#define CRYPTO_H

/*
 * crypto.h — Criptografía simétrica RC4 con gestión segura de llaves
 *
 * Pipeline mandatorio:
 *   texto → COMPRIMIR → ENCRIPTAR → disco
 *
 * Nunca al revés. La encriptación genera entropía máxima
 * (datos pseudoaleatorios sin patrones). Si encriptamos primero,
 * los algoritmos de compresión no encuentran nada que comprimir.
 *
 * Gestión segura de llaves:
 *   1. getpass()        → llave leída sin eco en consola
 *   2. mlock()          → página de RAM bloqueada, el kernel NO
 *                         puede mandarla al swap del disco
 *   3. rc4_encrypt()    → usar la llave
 *   4. explicit_bzero() → borrar la llave de la RAM
 *                         (el compilador NO puede optimizar esto)
 *   5. munlock()        → liberar el bloqueo de página
 */

#include <stdint.h>
#include <stddef.h>

#define RC4_KEY_MAX   256
#define RC4_KEY_MIN   8

typedef struct {
    uint8_t S[256];
    uint8_t i;
    uint8_t j;
} RC4State;

void   rc4_init     (RC4State *state, const uint8_t *key, size_t key_len);
void   rc4_process  (RC4State *state, uint8_t *buf, size_t len);
void   secure_key_erase(void *key, size_t len);
int    secure_key_lock (void *key, size_t len);
void   crypto_encrypt  (uint8_t *buf, size_t len, uint8_t *key, size_t key_len);
void   crypto_decrypt  (uint8_t *buf, size_t len, uint8_t *key, size_t key_len);
size_t crypto_read_key (uint8_t *out_key, size_t max_len, const char *prompt);

#endif /* CRYPTO_H */