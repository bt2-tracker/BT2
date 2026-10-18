#!/bin/bash
# Bygger nettversjonen av BerntTracker II (krever Emscripten: emcc i PATH).
# Bruk: web/build_web.sh <sdl-ww-prefix> <sdl-compat-prefix> <utmappe>
#  sdl-ww-prefix:     SDL2 bygget med -matomics -mbulk-memory (for lavforsinkelse-motoren)
#  sdl-compat-prefix: SDL2 bygget uten (for kompatibel motor)
set -e
cd "$(dirname "$0")/.."
SDL_WW="$1"; SDL_COMPAT="$2"; OUT="$3"
mkdir -p "$OUT"

SRC="src/*.c src/gfxdata/*.c src/mixer/*.c src/scopes/*.c src/modloaders/*.c src/smploaders/*.c"

COMMON="-O3 -DNDEBUG -Wno-unused-command-line-argument \
 -sASYNCIFY -sASYNCIFY_STACK_SIZE=131072 \
 -sSTACK_SIZE=2097152 -sINITIAL_MEMORY=67108864 -sALLOW_MEMORY_GROWTH -sMAXIMUM_MEMORY=1073741824 \
 -sFORCE_FILESYSTEM -lidbfs.js \
 -sEXPORTED_RUNTIME_METHODS=FS,UTF8ToString \
 -sEXPORTED_FUNCTIONS=_main,_webImportDone,_webGetExportPath,_malloc,_free \
 -sMINIFY_HTML=0 -sENVIRONMENT=web,worker \
 --pre-js web/bt2_pre.js"

echo "== lavforsinkelse-motor (AudioWorklet) =="
emcc $SRC -o "$OUT/bt2.js" $COMMON -DBT2_WEB_WORKLET -sWASM_WORKERS -sAUDIO_WORKLET \
 -I"$SDL_WW/include" -L"$SDL_WW/lib" -lSDL2

echo "== kompatibel motor =="
emcc $SRC -o "$OUT/bt2-compat.js" $COMMON \
 -sEXPORTED_FUNCTIONS=_main,_webImportDone,_webGetExportPath,_webAudioRenderChunk,_malloc,_free \
 -I"$SDL_COMPAT/include" -L"$SDL_COMPAT/lib" -lSDL2

cp web/index.html web/manifest.webmanifest web/coi-serviceworker.js web/icon-*.png "$OUT/" 2>/dev/null || true
echo "Ferdig: $OUT"
