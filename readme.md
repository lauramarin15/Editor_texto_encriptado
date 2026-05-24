# Editor de Texto CLI con Compresión Híbrida
 
Editor de texto por línea de comandos que guarda archivos comprimidos automáticamente en formato `.lz4e`, eligiendo el algoritmo óptimo según el contenido y tamaño del archivo. Ningún archivo viaja al disco en texto claro.
 
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
Header .lz4e (64B)    ← metadatos binarios empaquetados
      │
      ▼
write() / mmap()      ← única llamada al kernel
      │
      ▼
   Disco
```

## Requisitos
 
| Herramienta | macOS | Linux |
|---|---|---|
| `gcc` | `brew install gcc` | `apt install gcc` |
| `liblz4` | `brew install lz4` | `apt install liblz4-dev` |
| `python3` | `brew install python` | `apt install python3` |
| `make` | `xcode-select --install` | `apt install make` |
 
---
 
## Instalación
 
```bash
# 1. Instalar dependencias (macOS)
brew install lz4
 
# 2. Clonar o descomprimir el proyecto
tar -xzf editor_final.tar.gz
cd editor
 
# 3. Compilar
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
 
### Abrir un archivo existente
 
```bash
./editor open mi_documento.lz4e
```
 
Descomprime el archivo, lo carga en el Gap Buffer y abre el editor.
 
### Ver el contenido sin editar
 
```bash
./editor view mi_documento.lz4e
```
 
Descomprime e imprime el contenido en la terminal. No modifica el archivo.
 
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
║ Tamaño orig: 8192 bytes
║ Tamaño comp: 312 bytes
║ Ahorro:      96.2%
║ Checksum:    0x1A2B3C4D
║ Creado:      2025-05-3 10:23:45
║ Modificado:  2024-05-3 10:45:12
╚══════════════════════════════════════╝
```
 
---

## Comandos disponibles dentro del editor
 
El editor tiene dos modos: **INSERT** y **COMMAND**.
 
| Tecla | Modo | Acción |
|---|---|---|
| `ESC` | INSERT → COMMAND | Volver a modo comando |
| `:w` | COMMAND | Guardar sin salir |
| `:q` | COMMAND | Salir sin guardar |
| `:wq` | COMMAND | Guardar y salir |
| `:gN` | COMMAND | Ir a la línea N (ej. `:g10`) |
| `:d` | COMMAND | Borrar línea actual |
| `h` / `l` | COMMAND | Mover cursor izquierda / derecha |
 
### Ejemplo del proceso
 
```bash
./editor new notas.lz4e   # crear archivo
 
# En el editor:
Hola mundo                 # escribir texto
ESC                        # volver a modo comando
:wq                        # guardar y salir
 
./editor view notas.lz4e  # verificar contenido
./editor info notas.lz4e  # ver algoritmo y ahorro
```
 
---

## Benchmark
 
### Benchmark de algoritmos (ratio y velocidad)
 
Compara los tres algoritmos con 5 tipos de contenido × 6 tamaños:
 
```bash
bash benchmark_algoritmos.sh
```
 
Genera:
 
```
Tamaño  │ Huffman              │ LZ4                  │ RLE                  │ Auto
────────┼──────────────────────┼──────────────────────┼──────────────────────┼─────────
128 B   │ 1.28× 21.9%  0.02ms │ 1.00×  0.0%  0.01ms │ 1.00×  0.0%  0.00ms │ Huffman
512 B   │ 1.64× 39.1%  0.03ms │ 2.11× 52.5%  0.01ms │ 1.00×  0.0%  0.00ms │ Huffman
2 KB    │ 1.82× 45.1%  0.07ms │ 8.19× 87.8%  0.01ms │ 1.00×  0.0%  0.00ms │ LZ4
512 KB  │ 1.89× 47.1%  4.57ms │ 227×  99.6%  0.10ms │ 1.00×  0.0%  1.12ms │ LZ4
```
 
### Benchmark de I/O (fd+write vs mmap vs clásico)
 
Compara el enfoque clásico (texto plano) con los dos métodos de I/O propuestos:
 
```bash
bash benchmark.sh
```
 
Genera:
 
```
Métrica               │ Clásico   │ fd+write  │ mmap
──────────────────────┼───────────┼───────────┼──────────
Datos escritos        │ 512 KB    │ 18 KB     │ 18 KB
Ahorro en disco       │ 0%        │ 96%       │ 96%
Llamadas mmap()       │ 0         │ 0         │ 1
Checksum verificación │ No        │ Sí        │ Sí
Texto claro en disco  │ Sí        │ No        │ No
```

## Verificación de memoria
 
### macOS
 
```bash
make valgrind
```
 
O manualmente:
 
```bash
MallocStackLogging=1 leaks --atExit -- ./editor view archivo.lz4e
```
 
### Linux
 
```bash
make valgrind
```
 
O manualmente:
 
```bash
valgrind --leak-check=full \
         --show-leak-kinds=all \
         --track-origins=yes \
         ./editor view archivo.lz4e
```
 
La salida esperada debe mostrar `0 leaks` y `0 errors`.
 
---
 
## Estructura del proyecto
 
```
editor/
├── include/
│   ├── format.h          ← Header binario .lz4e + constantes ALGO_*
│   ├── gap_buffer.h      ← Interfaz del Gap Buffer
│   ├── compress.h        ← Pipeline de compresión (RLE, Huffman, LZ4)
│   └── io.h              ← API de I/O: fd+write y mmap
├── src/
│   ├── format.c          ← init, validate, checksum Fletcher-16, print
│   ├── gap_buffer.c      ← Gap Buffer completo (insert, delete, move, grow)
│   ├── compress.c        ← RLE + Huffman desde cero + wrapper LZ4
│   ├── io.c              ← Ambas APIs con buffers alineados a página
│   └── main.c            ← CLI: new / open / view / info + editor raw mode
├── Makefile              ← Compila editor (fd) y editor_mmap (mmap)
├── benchmark.sh          ← Benchmark I/O: clásico vs fd+write vs mmap
├── benchmark_algoritmos.sh ← Benchmark: Huffman vs LZ4 vs RLE
└── README.md             ← Este archivo
```