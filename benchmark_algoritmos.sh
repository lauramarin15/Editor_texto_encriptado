#!/bin/bash
# =============================================================================
# benchmark_algoritmos.sh — Huffman vs LZ4 vs RLE
# Compara ratio, ahorro y velocidad para 5 tipos de archivo x 6 tamaños
#
# Uso: bash benchmark_algoritmos.sh   (desde la carpeta editor/)
# Requiere: make all completado, python3, lz4 instalado
# =============================================================================

set -e
EDITOR_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$EDITOR_DIR"
TMP=$(mktemp -d); trap "rm -rf $TMP; rm -f bench_mini" EXIT

B='\033[1m'; G='\033[0;32m'; Y='\033[1;33m'
BL='\033[0;34m'; R='\033[0;31m'; C='\033[0;36m'; NC='\033[0m'

echo ""
echo -e "${B}════════════════════════════════════════════════════════════════${NC}"
echo -e "${B}  BENCHMARK: Huffman vs LZ4 vs RLE${NC}"
echo -e "${B}  Plataforma : $(uname -s) $(uname -m)${NC}"
echo -e "${B}  Fecha      : $(date)${NC}"
echo -e "${B}════════════════════════════════════════════════════════════════${NC}"
echo ""

# ── Compilar helper ──────────────────────────────────────────────
echo -e "${C}[1/3] Compilando helper de medición...${NC}"

BREW_PREFIX=$(brew --prefix 2>/dev/null || echo /usr/local)

cat > "$TMP/mini.c" << 'CEOF'
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "compress.h"
#include "format.h"

int main(int argc, char *argv[]) {
    if (argc < 3) return 1;
    const char *mode = argv[1];
    FILE *f = fopen(argv[2], "rb");
    if (!f) return 1;
    fseek(f, 0, SEEK_END); long sz = ftell(f); rewind(f);
    uint8_t *data = malloc(sz);
    if (fread(data, 1, sz, f) != (size_t)sz) { fclose(f); free(data); return 1; }
    fclose(f);

    uint8_t *out = NULL; size_t outlen = 0; uint8_t algo = ALGO_RAW;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    if      (!strcmp(mode,"huffman")) { out=huffman_compress(data,sz,&outlen);  algo=ALGO_HUFFMAN; }
    else if (!strcmp(mode,"lz4"))     { out=lz4_compress_buf(data,sz,&outlen); algo=ALGO_LZ4;     }
    else if (!strcmp(mode,"rle"))     { out=rle_compress(data,sz,&outlen);      algo=ALGO_RLE;     }
    else { CompressResult cr=compress_auto(data,sz); out=cr.data; outlen=cr.size; algo=cr.algo; }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (t1.tv_sec-t0.tv_sec)*1000.0 + (t1.tv_nsec-t0.tv_nsec)/1e6;

    if (!out || outlen >= (size_t)sz) {
        free(out); out=malloc(sz); memcpy(out,data,sz); outlen=sz; algo=ALGO_RAW;
    }
    const char *names[]={"RAW","Huffman","LZ4","RLE"};
    printf("%s|%.2f|%.1f|%.3f\n",
           names[algo<=3?algo:0],
           (double)sz/(outlen>0?outlen:1),
           (1.0-(double)outlen/sz)*100.0, ms);
    free(data); free(out);
    return 0;
}
CEOF

gcc -std=c11 -O2 \
    -I"./include" -I"$BREW_PREFIX/include" \
    "$TMP/mini.c" src/compress.c src/format.c \
    -L"$BREW_PREFIX/lib" -llz4 \
    $(uname | grep -q Linux && echo "-lrt") \
    -o bench_mini 2>/dev/null

echo -e "  ${G}✓${NC} Helper compilado"
echo ""

# ── Generar archivos de prueba ───────────────────────────────────
echo -e "${C}[2/3] Generando archivos de prueba...${NC}"

python3 << PYEOF
import os, random

tmp  = "$TMP"
sizes = [128, 512, 2048, 16384, 65536, 524288]

prose = ("Lorem ipsum dolor sit amet, consectetur adipiscing elit. "
         "Sed do eiusmod tempor incididunt ut labore et dolore magna. "
         "Huffman asigna codigos cortos a bytes frecuentes en el texto. "
         "LZ4 encuentra secuencias repetidas usando una tabla hash.\n")

code  = ("int rle_compress(uint8_t *in, size_t len, uint8_t *out) {\n"
         "    size_t i=0, wi=0;\n"
         "    while (i < len) {\n"
         "        uint8_t sym=in[i]; size_t run=1;\n"
         "        while (i+run<len && in[i+run]==sym && run<255) run++;\n"
         "        out[wi++]=(uint8_t)run; out[wi++]=sym; i+=run;\n"
         "    }\n    return (int)wi;\n}\n\n")

repeat= ("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\n"
         "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB\n"
         "                                        \n"
         "0000000000000000000000000000000000000000\n")

log   = ("[2024-01-15 10:23:45] INFO  GET /api/users 200 45ms\n"
         "[2024-01-15 10:23:46] INFO  GET /api/orders 200 23ms\n"
         "[2024-01-15 10:23:47] ERROR POST /api/login 401 5ms\n"
         "[2024-01-15 10:23:48] WARN  GET /api/report 200 2340ms\n")

chunks = {"prose":prose, "code":code, "repeated":repeat, "log":log}

for name, chunk in chunks.items():
    for s in sizes:
        data = (chunk * (s//len(chunk)+1))[:s]
        open(f"{tmp}/{name}_{s}.txt","w").write(data)

for s in sizes:
    data = bytes(random.randint(0,255) for _ in range(s))
    open(f"{tmp}/random_{s}.txt","wb").write(data)

print("  5 tipos × 6 tamaños = 30 archivos generados")
PYEOF
echo ""

# ── Función de tabla ─────────────────────────────────────────────
print_section() {
    local title="$1"; local type="$2"
    echo -e "${B}${BL}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo -e "${B}  $title${NC}"
    echo -e "${B}${BL}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    printf "${B}%-8s │ %-26s │ %-26s │ %-26s │ %s${NC}\n" \
        "Tamaño" "Huffman" "LZ4" "RLE" "Auto elige"
    printf "%-8s │ %-26s │ %-26s │ %-26s │ %s\n" \
        "" "ratio  ahorro    cpu" "ratio  ahorro    cpu" "ratio  ahorro    cpu" ""
    echo "─────────┼───────────────────────────┼───────────────────────────┼───────────────────────────┼──────────"

    for s in 128 512 2048 16384 65536 524288; do
        local sname
        case $s in
            128)    sname="128 B"  ;;
            512)    sname="512 B"  ;;
            2048)   sname="2 KB"   ;;
            16384)  sname="16 KB"  ;;
            65536)  sname="64 KB"  ;;
            524288) sname="512 KB" ;;
        esac

        local input="$TMP/${type}_${s}.txt"
        local H L Rv A
        H=$(./bench_mini huffman "$input" 2>/dev/null || echo "ERR|0|0|0")
        L=$(./bench_mini lz4     "$input" 2>/dev/null || echo "ERR|0|0|0")
        Rv=$(./bench_mini rle    "$input" 2>/dev/null || echo "ERR|0|0|0")
        A=$(./bench_mini auto    "$input" 2>/dev/null || echo "ERR|0|0|0")

        local ha hr hs hm la lr ls lm ra rr rs rm aa ar as am
        IFS='|' read -r ha hr hs hm <<< "$H"
        IFS='|' read -r la lr ls lm <<< "$L"
        IFS='|' read -r ra rr rs rm <<< "$Rv"
        IFS='|' read -r aa ar as am <<< "$A"

        local color
        case "$aa" in
            Huffman) color="${G}"  ;;
            LZ4)     color="${BL}" ;;
            RLE)     color="${Y}"  ;;
            *)       color="${R}"  ;;
        esac

        printf "%-8s │ %5s× %5s%% %6sms │ %5s× %5s%% %6sms │ %5s× %5s%% %6sms │ ${color}%s${NC}\n" \
            "$sname" \
            "$hr" "$hs" "$hm" \
            "$lr" "$ls" "$lm" \
            "$rr" "$rs" "$rm" \
            "$aa"
    done
    echo ""
}

# ── Tablas ───────────────────────────────────────────────────────
echo -e "${C}[3/3] Midiendo...${NC}"
echo ""

print_section "📄 TEXTO PROSA  (Lorem ipsum, texto natural)" "prose"
print_section "💻 CÓDIGO FUENTE C  (palabras clave repetidas)" "code"
print_section "🔁 CONTENIDO REPETITIVO  (rachas largas — ideal RLE)" "repeated"
print_section "📋 LOG DE SERVIDOR  (semi-repetitivo)" "log"
print_section "🎲 DATOS ALEATORIOS  (ingcompresibles)" "random"

# ── Resumen ──────────────────────────────────────────────────────
echo -e "${B}${BL}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo -e "${B}  CONCLUSIONES${NC}"
echo -e "${B}${BL}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo ""
echo -e "  ${G}Huffman${NC}  → Archivos pequeños (<1KB) con texto variado."
echo -e "           Overhead fijo del árbol amortizado desde los primeros bytes."
echo ""
echo -e "  ${BL}LZ4${NC}      → Archivos grandes (>1KB). Encuentra repeticiones de"
echo -e "           secuencias en ventana de 64KB. Velocidad extrema."
echo ""
echo -e "  ${Y}RLE${NC}      → Cuando >50%% del contenido son rachas de bytes iguales."
echo -e "           O(n) trivial. Gana en bitmaps, logs con espacios, datos binarios."
echo ""
echo -e "  ${R}RAW${NC}      → Datos aleatorios o archivos <64B. Ningún algoritmo ayuda."
echo ""
echo -e "${B}Cómo leer la tabla:${NC}"
echo -e "  ratio  = original ÷ comprimido  (mayor = mejor compresión)"
echo -e "  ahorro = espacio ahorrado en %  (mayor = mejor)"
echo -e "  cpu    = tiempo de compresión   (menor = más rápido)"
echo ""
