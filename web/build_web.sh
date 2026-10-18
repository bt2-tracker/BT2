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
 -sEXPORTED_RUNTIME_METHODS=FS,UTF8ToString,ccall \
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

cp web/manifest.webmanifest web/coi-serviceworker.js web/icon-*.png web/bt2-logo.png "$OUT/" 2>/dev/null || true

# Build id (GitHub: the commit, locally: the time). The page and version.json carry it, so the app on
# the device can tell when a newer version is out (see "app on the device" in index.html).
BT2_BUILD="${GITHUB_SHA:-local-$(date +%Y%m%d%H%M%S)}"
BT2_BUILD="${BT2_BUILD:0:24}"
sed "s/__BT2_BUILD__/$BT2_BUILD/g" web/index.html > "$OUT/index.html"

# version.json: the app's files with size and SHA-256 (the update checks them before switching)
python3 - "$OUT" "$BT2_BUILD" <<'PY'
import hashlib, json, os, re, sys
out, build = sys.argv[1], sys.argv[2]
html = open(os.path.join(out, 'index.html'), encoding='utf-8').read()
version = re.search(r"const BT2_VERSION = '([^']*)'", html).group(1)
date = re.search(r"const BT2_DATE = '([^']*)'", html).group(1)
names = ['index.html', 'bt2.js', 'bt2.wasm', 'bt2.ww.js', 'bt2.aw.js', 'bt2-compat.js', 'bt2-compat.wasm',
         'coi-serviceworker.js', 'manifest.webmanifest', 'icon-180.png', 'icon-192.png', 'icon-512.png',
         'bt2-logo.png']
files = []
for n in names:
	p = os.path.join(out, n)
	if os.path.exists(p):
		data = open(p, 'rb').read()
		files.append({'path': n, 'size': len(data), 'sha256': hashlib.sha256(data).hexdigest()})
json.dump({'build': build, 'version': version, 'date': date, 'files': files}, open(os.path.join(out, 'version.json'), 'w'), indent=1)
print('version.json:', version, build, len(files), 'filer')
PY

# Demo-låter: "Demo Tracks/<sjanger>/*.xm" kopieres med, og en liste lages så appen kan hente dem
if [ -d "Demo Tracks" ]; then
	rm -rf "$OUT/demo"
	mkdir -p "$OUT/demo"
	cp -r "Demo Tracks/." "$OUT/demo/"
	python3 - "$OUT/demo" <<'PY'
import json, os, sys
root = sys.argv[1]
files = []
for d, _, names in os.walk(root):
	for n in sorted(names):
		if n.lower().endswith(('.xm', '.mod', '.s3m', '.it', '.xi', '.wav')):
			rel = os.path.relpath(os.path.join(d, n), root).replace(os.sep, '/')
			files.append({'path': rel, 'size': os.path.getsize(os.path.join(d, n))})
json.dump({'version': 1, 'files': files}, open(os.path.join(root, 'index.json'), 'w'), indent=1)
print('demo-filer:', len(files))
PY
fi
echo "Ferdig: $OUT"
