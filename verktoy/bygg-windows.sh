#!/bin/bash
# Bygger Windows-programmet til Bernttracker II fra den samme koden som nettversjonen, med llvm-mingw
# (Windows-kompilator som kjører på Linux). GitHub kjører dette ved hver Sync (.github/workflows/web.yml).
#
# Bruk: verktoy/bygg-windows.sh <llvm-mingw-mappe> <SDL2-mingw-mappe> <byggemappe> <publiseringsmappe>
#   llvm-mingw-mappe:  llvm-mingw-20241217-ucrt-ubuntu-20.04-x86_64
#   SDL2-mingw-mappe:  SDL2-2.32.10/x86_64-w64-mingw32 (fra SDL2-devel-2.32.10-mingw.tar.gz)
#
# I publiseringsmappa (windows/ på nettsiden) kommer:
#   "Bernttracker II.exe", SDL2.dll  - programmet (oppdateringen henter disse)
#   version.json                     - versjon, dato, størrelse og SHA-256 for filene over
#   Bernttracker-II-Windows.zip      - alt for en ny installasjon, med "Demo Tracks"
#
# Versjonen hentes fra web/index.html (BT2_VERSION, BT2_DATE), så nettversjonen og programmet har samme nummer.
# Det samme kildekoden gir byte for byte det samme programmet (ingen tidsstempler), så programmet ser bare
# en oppdatering når noe i det faktisk er endret.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$PWD"
LLVM="$1"; SDL="$2"; OUT="$3"; PUB="$4"
TC="$LLVM/bin"
EXE="Bernttracker II.exe"

rm -rf "$OUT/obj" "$OUT/gen" "$OUT/pkg"
mkdir -p "$OUT/obj" "$OUT/gen" "$OUT/pkg" "$PUB"

VERSION=$(sed -n "s/.*const BT2_VERSION = '\([^']*\)'.*/\1/p" web/index.html | head -1)
DATE=$(sed -n "s/.*const BT2_DATE = '\([^']*\)'.*/\1/p" web/index.html | head -1)
if [ -z "$VERSION" ] || [ -z "$DATE" ]; then
	echo "FEIL: fant ikke BT2_VERSION / BT2_DATE i web/index.html"
	exit 1
fi
printf '#define BT2_VERSION "%s"\n#define BT2_DATE "%s"\n' "$VERSION" "$DATE" > "$OUT/gen/bt2_build.h"
echo "== Bernttracker II $VERSION ($DATE) for Windows =="

# de samme filene og innstillingene som Visual Studio-prosjektet
FILES=$(grep -o 'ClCompile Include="[^"]*"' vs2026_project/ft2-clone/ft2-clone.vcxproj | sed 's/ClCompile Include="..\\..\\//; s/"$//; s/\\/\//g')
export TC SDL ROOT OUT
export DEFS="-D__WINDOWS_MM__ -DNDEBUG -DWIN32 -D_CRT_SECURE_NO_WARNINGS -DHAS_MIDI -DHAS_LIBFLAC -DBT2_BUILD_HEADER"

compile_one() {
	local f="$1"
	local o="$OUT/obj/$(echo "$f" | tr '/' '_').o"
	local cc
	case "$f" in
		*.cpp) cc=("$TC/x86_64-w64-mingw32-clang++" -std=c++17) ;;
		*) cc=("$TC/x86_64-w64-mingw32-clang" -std=c17) ;;
	esac
	# (-include math.h og -Werror=...: som Visual Studio, så feil der også stopper her)
	if ! "${cc[@]}" -O2 -msse2 -ffast-math $DEFS -I"$SDL/include" -I"$SDL/include/SDL2" -I"$ROOT/src" -I"$OUT/gen" \
		-ffile-prefix-map="$ROOT"=. -include math.h -Wno-everything -Werror=implicit-function-declaration \
		-Werror=int-conversion -Werror=incompatible-pointer-types -c "$ROOT/$f" -o "$o" 2> "$o.log"; then
		echo "FEIL: $f"
		head -30 "$o.log"
		return 1
	fi
}
export -f compile_one

JOBS=$(nproc 2>/dev/null || echo 2)
echo "$FILES" | xargs -P "$JOBS" -I{} bash -c 'compile_one "$@"' _ {}

OBJS=()
for f in $FILES; do
	OBJS+=("$OUT/obj/$(echo "$f" | tr '/' '_').o")
done

# ikon, versjonsinfo og logoen til startskjermen
"$TC/x86_64-w64-mingw32-windres" -DBT2_BUILD_HEADER -I"$OUT/gen" -I"$SDL/include" -I"$ROOT/src" \
	"$ROOT/src/ft2-clone.rc" -O coff -o "$OUT/obj/resources.o"
OBJS+=("$OUT/obj/resources.o")

"$TC/x86_64-w64-mingw32-clang++" -O2 -s -mwindows -Wl,--no-insert-timestamp -o "$OUT/pkg/$EXE" "${OBJS[@]}" \
	-L"$SDL/lib" -lSDL2main -lSDL2 -lshlwapi -lwinmm -lole32 -luuid -lwinhttp -lbcrypt -lgdi32 -lcomdlg32 -static-libgcc -static-libstdc++
cp "$SDL/bin/SDL2.dll" "$OUT/pkg/SDL2.dll"

# SDL-versjonen programmet er bygget med (oppdateringen bytter SDL2.dll bare når den endres)
SDLVER=$(awk '/#define SDL_MAJOR_VERSION/{a=$3} /#define SDL_MINOR_VERSION/{b=$3} /#define SDL_PATCHLEVEL/{c=$3} END{print a"."b"."c}' "$SDL/include/SDL2/SDL_version.h")

# version.json og zip-fila (faste tider i zip-fila, så den også blir lik hver gang)
python3 - "$ROOT" "$OUT/pkg" "$PUB" "$VERSION" "$DATE" "${GITHUB_SHA:-local}" "$SDLVER" <<'PY'
import hashlib, json, os, shutil, sys, zipfile
root, pkg, pub, version, date, build, sdlver = sys.argv[1:8]
exe = 'Bernttracker II.exe'
files = []
for name in (exe, 'SDL2.dll'):
	data = open(os.path.join(pkg, name), 'rb').read()
	shutil.copyfile(os.path.join(pkg, name), os.path.join(pub, name))
	files.append({'path': name, 'size': len(data), 'sha256': hashlib.sha256(data).hexdigest()})

readme = ('Bernttracker II for Windows\r\n'
	'===========================\r\n\r\n'
	'Start «Bernttracker II.exe». Første gang kan Windows si «Windows beskyttet PC-en»:\r\n'
	'trykk «Mer info» og så «Kjør likevel» (programmet er ikke signert).\r\n\r\n'
	'Låtene dine lagres i Dokumenter\\BT2 (Modules, Instruments, Samples, Patterns, Tracks),\r\n'
	'og demo-låtene legges i Dokumenter\\BT2\\Demo Tracks – som i nettversjonen.\r\n\r\n'
	'Disk Op. har de samme knappene som på iPad: «BT2» (mappene dine), «Imp.» (hent filer inn,\r\n'
	'zip-filer pakkes ut), «Exp.» (lagre en kopi et annet sted), «Mod-land» (Modland-biblioteket)\r\n'
	'og «Find» (søk, også Ctrl+F). Låter du åpner fra Modland lagres i Dokumenter\\BT2\\Modland.\r\n\r\n'
	'Programmet ser etter ny versjon hver gang det starter, og du velger selv om du vil\r\n'
	'oppdatere. En oppdatering bytter bare selve programfilene. Låtene, samplene, mappene\r\n'
	'og innstillingene dine blir aldri rørt.\r\n\r\n'
	'Lavest forsinkelse: installer ASIO4ALL (gratis, https://www.asio4all.org). Programmet\r\n'
	'bruker ASIO av seg selv når det finnes. Ellers brukes Windows\' eksklusive lydmodus.\r\n\r\n'
	'Nettversjonen: https://bt2-tracker.github.io/BT2/\r\n'
	'Kildekoden:    https://github.com/bt2-tracker/BT2\r\n'
	'Basert på ft2-clone av Olav «8bitbubsy» Sørensen (BSD-3), se LICENSE.txt.\r\n'
	'Grafikk av Magnus «Vogue» Högdahl (CC BY-NC-SA 4.0).\r\n')

entries = [(exe, os.path.join(pkg, exe)), ('SDL2.dll', os.path.join(pkg, 'SDL2.dll'))]
for name, src in (('LICENSE.txt', 'LICENSE'), ('LICENSES-tredjepart.txt', 'release/LICENSES.txt')):
	if os.path.exists(os.path.join(root, src)):
		entries.append((name, os.path.join(root, src)))
demo = os.path.join(root, 'Demo Tracks')
for d, dirs, names in os.walk(demo):
	dirs.sort()
	for n in sorted(names):
		if n.lower().endswith(('.xm', '.mod', '.s3m', '.it', '.xi', '.wav')):
			p = os.path.join(d, n)
			entries.append(('Demo Tracks/' + os.path.relpath(p, demo).replace(os.sep, '/'), p))

zpath = os.path.join(pub, 'Bernttracker-II-Windows.zip')
with zipfile.ZipFile(zpath, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
	def add(name, data):
		info = zipfile.ZipInfo('Bernttracker II/' + name, date_time=(2026, 1, 1, 0, 0, 0))
		info.compress_type = zipfile.ZIP_DEFLATED
		info.external_attr = 0o644 << 16
		info.flag_bits |= 0x800  # (UTF-8 names)
		z.writestr(info, data)
	add('Les meg.txt', b'\xef\xbb\xbf' + readme.encode('utf-8'))
	for name, src in entries:
		add(name, open(src, 'rb').read())

meta = {'version': version, 'date': date, 'build': build[:24], 'sdl': sdlver, 'files': files,
	'zip': {'path': os.path.basename(zpath), 'size': os.path.getsize(zpath)}}
json.dump(meta, open(os.path.join(pub, 'version.json'), 'w'), indent=1)
print('version.json:', version, date, ', '.join('%s %d' % (f['path'], f['size']) for f in files))
print('zip:', meta['zip']['size'], 'bytes,', len(entries) + 1, 'files')
PY
ls -la "$PUB"
