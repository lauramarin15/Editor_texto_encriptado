# Editor de Texto CLI con Compresión Híbrida + Encriptación RC4

Editor de texto por línea de comandos que guarda archivos comprimidos y encriptados automáticamente en formato `.lz4e`, eligiendo el algoritmo óptimo según el contenido y tamaño del archivo. Ningún archivo viaja al disco en texto claro.

El editor implementa un **pipeline de compresión híbrida** que opera completamente en User Space antes de invocar cualquier llamada al sistema:

```
Texto (Gap Buffer)
      │
      ▼
 rle_score()          ← analiza el contenido
      │
      ▼
choose_algorithm()    ← RAW / Huffman / LZ4 / RLE
      │
      ▼
compress_auto()       ← comprime en buffer intermedio
      │
      ▼
crypto_encrypt()      ← encripta con RC4 (si --encrypt)
      │
      ▼
Header .lz4e (64B)    ← metadatos binarios empaquetados
      │
      ▼
write() / mmap()      ← única llamada al kernel
      │
      ▼
   Disco
```

> **Pipeline mandatorio: COMPRIMIR → ENCRIPTAR**
> Nunca al revés. La encriptación genera entropía máxima (datos pseudoaleatorios).
> Si encriptamos primero, los algoritmos de compresión no encuentran nada que reducir.

---

## Requisitos

| Herramienta | macOS | Linux |
|---|---|---|
| `gcc` | `brew install gcc` | `apt install gcc` |
| `liblz4` | `brew install lz4` | `apt install liblz4-dev` |
| `python3` | Preinstalado | `apt install python3` |
| `make` | `xcode-select --install` | `apt install make` |

---

## Instalación

```bash
# 1. Instalar dependencias (macOS)
brew install lz4

# 2. Compilar
make all
```

La salida esperada:

```
Compilado para macOS
  Binarios: ./editor (fd+write)  ./editor_mmap (mmap)
```

---

## Uso del editor

### Crear un archivo nuevo

```bash
./editor new mi_documento.lz4e
```

Abre el editor vacío. Al guardar con `:wq`, el archivo se comprime automáticamente.

### Crear un archivo encriptado

```bash
./editor new secreto.lz4e --encrypt --key miClave123
```

Pipeline: texto → comprimir → encriptar (RC4) → disco. La llave se borra de la RAM con `volatile loop` inmediatamente después de usarse.

### Abrir un archivo existente

```bash
./editor open mi_documento.lz4e

# Si el archivo está encriptado:
./editor open secreto.lz4e --key miClave123
```

### Ver el contenido sin editar

```bash
./editor view mi_documento.lz4e

# Si el archivo está encriptado:
./editor view secreto.lz4e --key miClave123
```

### Ver metadatos del archivo

```bash
./editor info mi_documento.lz4e
```

Muestra el header binario sin descomprimir el payload:

```
╔══════════════════════════════════════╗
║         .lz4e FILE INFO              ║
╠══════════════════════════════════════╣
║ Magic:       LE4e (v2)
║ Algoritmo:   LZ4
║ Encriptado:  Si (RC4)
║ Tamaño orig: 8192 bytes
║ Tamaño comp: 312 bytes
║ Ahorro:      96.2%
║ Checksum:    0x1A2B3C4D
║ Creado:      2026-05-24 10:23:45
║ Modificado:  2026-05-24 10:45:12
╚══════════════════════════════════════╝
```

---

## Comandos disponibles dentro del editor

El editor tiene dos modos: **INSERT** y **COMMAND**.

| Tecla | Modo | Acción |
|---|---|---|
| `i` | COMMAND → INSERT | Entrar a modo insertar |
| `ESC` | INSERT → COMMAND | Volver a modo comando |
| `:w` | COMMAND | Guardar sin salir |
| `:q` | COMMAND | Salir sin guardar |
| `:wq` | COMMAND | Guardar y salir |
| `:gN` | COMMAND | Ir a la línea N (ej. `:g10`) |
| `:d` | COMMAND | Borrar línea actual |
| `h` / `l` | COMMAND | Mover cursor izquierda / derecha |

### Ejemplo de sesión

```bash
# Sin encriptación
./editor new notas.lz4e
# i → escribir texto → ESC → :wq

./editor view notas.lz4e
./editor info notas.lz4e

# Con encriptación
./editor new secreto.lz4e --encrypt --key miClave123
# i → escribir texto → ESC → :wq

./editor view secreto.lz4e --key miClave123

# Ver que el payload es ilegible sin la llave
xxd secreto.lz4e | head -6
```

---

## Algoritmos de compresión

| Algoritmo | Rango | Cuándo |
|---|---|---|
| RAW | < 64 bytes | Sin compresión — overhead mayor que ahorro |
| Huffman | 64B – 1KB | Texto variado pequeño |
| LZ4 | ≥ 1KB | Texto, código, logs |
| RLE | Cualquiera | Cuando rle_score > 0.50 (rachas largas) |

### Lógica de decisión automática

```
< 64 bytes          → RAW
64B – 1KB           → Huffman  (o RLE si score > 0.50)
≥ 1KB               → LZ4      (o RLE si score > 0.60)
comprimido ≥ original → RAW (fallback)
```

---

## Encriptación RC4

Implementado desde cero en `src/crypto.c`. Gestión segura de llaves:

1. La llave se pasa por `--key` (nunca hardcoded en el código)
2. Se copia a un buffer bloqueado en RAM con `mlock()` — el kernel no puede mandarla al swap
3. Se usa para encriptar con RC4 (XOR con keystream pseudoaleatorio)
4. Se borra inmediatamente con `volatile loop` (el compilador no puede optimizarlo)

```bash
# Verificar que el archivo está realmente encriptado en disco
xxd secreto.lz4e | head -6
# Byte 5 debe ser 0x04 (FLAG_ENCRYPTED)
# Payload desde byte 64 debe ser ilegible
```

---

## Benchmark

### Benchmark de algoritmos (ratio y velocidad)

```bash
bash benchmark_algoritmos.sh
```

Resultados reales en macOS Darwin arm64:

```
Tamaño  │ Huffman              │ LZ4                   │ RLE                  │ Auto
────────┼──────────────────────┼───────────────────────┼──────────────────────┼─────────
128 B   │ 1.28× 21.9% 0.008ms │  1.00×  0.0% 0.313ms │ 1.00×  0.0% 0.001ms │ Huffman
512 B   │ 1.64× 39.1% 0.015ms │  2.11× 52.5% 0.007ms │ 1.00×  0.0% 0.001ms │ Huffman
2 KB    │ 1.82× 45.1% 0.032ms │  8.19× 87.8% 0.006ms │ 1.00×  0.0% 0.003ms │ LZ4
64 KB   │ 1.89× 47.1% 0.303ms │ 131.6× 99.2% 0.010ms │ 1.00×  0.0% 0.052ms │ LZ4
512 KB  │ 1.89× 47.1% 2.290ms │ 227.9× 99.6% 0.021ms │ 1.00×  0.0% 0.423ms │ LZ4
```

### Benchmark de I/O (fd+write vs mmap vs clásico)

```bash
bash benchmark.sh
```

### Benchmark de encriptación

```bash
bash benchmark_encriptacion.sh
```

Resultados reales en macOS Darwin arm64 (512KB de texto):

```
Metrica                        A.Clasico   B.Compresion   C.Comp+Enc
======================================================================
Volumen datos a disco (bytes)    524,288          3,566        3,566
Ahorro en disco                       0%          99.3%        99.3%
CPU compresion (ms)                0.000          0.375        0.043
CPU encriptacion (ms)              0.000          0.000        0.005
CPU total (ms)                     0.000          0.375        0.048
Texto claro en disco                  Si             No           No
Encriptado                            No             No     Si (RC4)
```

> Añadir RC4 agrega solo **0.005ms** de CPU. El archivo sigue siendo **99.3% más pequeño** que el texto plano. Sistema 100% cifrado operando en tiempo similar al enfoque clásico.

---

## Verificación de memoria

```bash
# macOS
make valgrind

# Manual
MallocStackLogging=1 leaks --atExit -- ./editor view archivo.lz4e

# Resultado esperado:
# Process 18595: 0 leaks for 0 total leaked bytes
```

---

## Estructura del proyecto

```
editor/
├── include/
│   ├── format.h              ← Header binario .lz4e + constantes ALGO_* + FLAG_ENCRYPTED
│   ├── gap_buffer.h          ← Interfaz del Gap Buffer
│   ├── compress.h            ← Pipeline de compresión (RLE, Huffman, LZ4)
│   ├── io.h                  ← API de I/O: fd+write y mmap con soporte de llave
│   └── crypto.h              ← Interfaz RC4 + gestión segura de llaves
├── src/
│   ├── format.c              ← init, validate, checksum Fletcher-16, print
│   ├── gap_buffer.c          ← Gap Buffer completo (insert, delete, move, grow)
│   ├── compress.c            ← RLE + Huffman desde cero + wrapper LZ4
│   ├── io.c                  ← Ambas APIs con buffers alineados a página + encriptación
│   ├── crypto.c              ← RC4 desde cero + mlock + volatile erase
│   └── main.c                ← CLI: new/open/view/info + --encrypt + --key
├── Makefile                  ← Compila editor (fd) y editor_mmap (mmap)
├── benchmark.sh              ← Benchmark I/O: clásico vs fd+write vs mmap
├── benchmark_algoritmos.sh   ← Benchmark: Huffman vs LZ4 vs RLE
├── benchmark_encriptacion.sh ← Benchmark: clásico vs compresión vs comp+RC4
└── README.md                 ← Este archivo
```