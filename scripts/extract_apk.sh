#!/usr/bin/env bash
# extract_apk.sh -- stage 1, from the Super Mario 127 Android APK you built (or
# were given) to the files the Switch wrapper loads, laid out exactly as
# sdmc:/switch/sm127/ expects.
#
#   dist/sm127/libgodot_android.so   lib/arm64-v8a/ -- the engine (Godot 3.6)
#   dist/sm127/libc++_shared.so      lib/arm64-v8a/
#   dist/sm127/assets/               the game: project.binary + the loose
#                                    files a Godot Android export stores
#
# SM127 is pure GDScript, so unlike the ports this one is forked from there is
# no GDNative module to carry across -- those two libraries are the whole
# native side.
#
# Nothing is modified; the APK is only read. dist/sm127/ is replaced.
#
# For development: players do not need this. sm127_nx.nro installs the game from
# an APK placed next to it on the first launch (source/apk_install.c). This is
# the same unpacking on a computer, plus a check that the wrapper can bind every
# import the engine libraries need.
#
# usage: scripts/extract_apk.sh "path/to/SuperMario127-android-arm64.apk"

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/dist/sm127"

say() { printf '\n\033[1m== %s\033[0m\n' "$*"; }
die() { printf '\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }
warn() { printf '\033[33mwarning:\033[0m %s\n' "$*" >&2; }

APK="${1:-}"
[ -n "$APK" ] || die "usage: $0 <path/to/game.apk>"
[ -f "$APK" ] || die "no such file: $APK"
unzip -Z1 "$APK" | grep -x 'lib/arm64-v8a/libgodot_android.so' >/dev/null \
  || die "$APK has no lib/arm64-v8a/libgodot_android.so -- not an arm64 Godot APK"

say "extracting $(basename "$APK")"
rm -rf "$OUT"
mkdir -p "$OUT"
unzip -q -o -j "$APK" 'lib/arm64-v8a/*.so' -d "$OUT"
unzip -q -o "$APK" 'assets/*' -d "$OUT"
# Android-only ART profile data; nothing on the Switch reads it.
rm -rf "$OUT/assets/dexopt"

ver="$(strings -a "$OUT/libgodot_android.so" | grep -m1 -oE '^[34]\.[0-9]+(\.[0-9]+)?\.(stable|rc[0-9]*|beta[0-9]*)(\.[a-z_]+)?$' || true)"
echo "engine: Godot ${ver:-(version string not found)}"

# The wrapper follows Godot 3.6's native interface: setup() takes a GodotTTS
# and returns a result, and touch input arrives through dispatchTouchEvent.
# 3.5 differs in both, and the NRO refuses it at boot -- so say so here, where
# there is a person watching, rather than on the console.
case "${ver:-}" in
  3.6*) ;;
  "")   warn "could not read the engine version; expected Godot 3.6" ;;
  *)    warn "this is Godot $ver, but the wrapper targets 3.6 -- the NRO will refuse it" ;;
esac

if [ -f "$OUT/assets/project.binary" ]; then
  echo "game data: assets/project.binary + $(find "$OUT/assets" -type f | wc -l | tr -d ' ') files"
elif ls "$OUT"/assets/*.sparsepck >/dev/null 2>&1; then
  die "this APK stores its data as a sparse pack (Godot 4.4+), which this wrapper does not read"
else
  die "no assets/project.binary in the APK -- the game data may be in an expansion (.obb) file"
fi

# assets/override.cfg -- Godot 3 reads res://override.cfg on top of
# project.binary (ProjectSettings::_setup), and on Android res:// is the
# assets/ folder the wrapper serves. Each key here is a separate
# ProjectSettings property, so this ADDS the autoload without disturbing the
# game's own eight.
#
# Only the autoload: SM127 seeds its own controller bindings (A confirms,
# + pauses) from scenes/menu/options/controls/presets/controller/*.cfg the
# first time the main menu loads, and rebuilds the InputMap from them, so
# anything written here under [input] would be replaced seconds later.
# The scripts the autoload points at are not copied: sm127_nx.nro carries port/ in
# its romfs and serves it as res://switch_port/ (godot_shim.c).
say "port helper (assets/override.cfg)"
cat > "$OUT/assets/override.cfg" <<'CFG'
; Written by sm127_nx's scripts/extract_apk.sh. Not a game file.
; Registers the port's helper autoload, which sm127_nx.nro serves from its own files.

[autoload]

SwitchPort="*res://switch_port/switch_port.gd"
CFG

# One file instead of ~8500: the wrapper mounts it as --main-pack
# res://sm127.pck. override.cfg stays loose next to it (Godot reads it from
# the pack's directory), so it can still be edited on the SD card.
# LOOSE=1 keeps the unpacked tree instead, for debugging.
if [ "${LOOSE:-0}" != 1 ]; then
  say "packing assets/ into assets/sm127.pck"
  PACKSRC="$OUT/assets.src"
  mv "$OUT/assets" "$PACKSRC"
  mkdir -p "$OUT/assets"
  python3 "$ROOT/tools/make_pck.py" "$PACKSRC" "$OUT/assets/sm127.pck" \
    --exclude override.cfg --version "$(echo "$ver" | grep -oE '^[0-9]+\.[0-9]+\.[0-9]+' || echo 3.6.0)"
  cp "$PACKSRC/override.cfg" "$OUT/assets/override.cfg"
  rm -rf "$PACKSRC"
fi

# SM127 ships no GDNative module. If a modded APK brings one, it is loaded
# when the engine dlopen()s it (native_modules.c) and its imports are checked
# below like any other -- so report it rather than ignore it.
extra="$(cd "$OUT" && ls *.so | grep -vx 'libgodot_android.so' | grep -vx 'libc++_shared.so' || true)"
if [ -n "$extra" ]; then
  echo "native modules: $(echo $extra)"
fi

say "import coverage"
"$ROOT/tools/import_gap.py" "$OUT" || die "imports the wrapper cannot resolve -- see above"

say "done"
du -sh "$OUT"
echo
echo "Next: make && make dist, then copy dist/sm127/ to sdmc:/switch/sm127/"
