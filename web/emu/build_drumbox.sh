#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# DrumBox in the browser (web/emu/drumbox_web.c, from Felucca's web/emu/build.sh).
#   web/emu/build_drumbox.sh  ->  build/emu-drumbox/{index.html, worklet.js, felucca.wasm, fonts/}
# Needs build/gen (the tables, fonts and palettes): run tools/build.py once first. The wasm is named felucca.wasm
# because the page (index.html, shared with Felucca's emulator) loads that name.
set -e
cd "$(dirname "$0")/../.."
[ -f build/gen/felucca_tables.h ] || { echo "drumbox emu: run tools/build.py once first (build/gen)"; exit 1; }
command -v emcc >/dev/null 2>&1 || { echo "drumbox emu: no emcc (Emscripten)"; exit 1; }
OUT=build/emu-drumbox
mkdir -p $OUT/fonts
# shellcheck disable=SC2086
emcc ${EMU_OPT:--O2} -ffp-contract=off -std=gnu99 -w ${EMU_CFLAGS:-} \
    -Ibuild/gen -Ifirmware/src -Ifirmware/bsp/chip -Ifirmware/bsp/boards/fm1 \
    --no-entry -sSTANDALONE_WASM -sSTACK_SIZE=1048576 -sINITIAL_MEMORY=33554432 -sFILESYSTEM=0 \
    -o $OUT/felucca.wasm web/emu/drumbox_web.c
# the page is Felucca's: name it U0U, keep the credit to Felucca (its nav links U0U's installer: web/make_site.py puts the pages together)
sed -e "s/@VERSION@/U0U/g" -e "s/@PREVIEW@/ M0/g" \
    -e 's|<title>Felucca</title>|<title>U0U</title>|' \
    -e 's|content="Felucca, the multi-engine synth firmware for the FM-1, running in your browser: the same code the instrument runs."|content="U0U, a drum machine firmware for the FM-1, running in your browser: the same code the instrument runs."|' \
    -e 's|Felucca by Leo Kuroshita, Hügelton Instruments|U0U, built on Felucca by Leo Kuroshita, Hügelton Instruments|' \
    -e 's|Felucca is not affiliated|U0U is not affiliated|' \
    web/emu/index.html > $OUT/index.html
cp web/emu/worklet.js $OUT/
cp web/emu/fonts/DotGothic16-subset.woff web/emu/fonts/OFL.txt $OUT/fonts/
echo "drumbox emu: $OUT ($(wc -c < $OUT/felucca.wasm | tr -d ' ') B wasm)"
