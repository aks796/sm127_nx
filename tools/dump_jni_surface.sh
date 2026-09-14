#!/usr/bin/env bash
# dump_jni_surface.sh -- regenerate docs/jni_surface_godot.txt from an APK.
#
# Run this when the game ships a new APK, or against any other Godot Android
# build you want to point this wrapper at. Section 3 of the generated file is
# hand-written prose about the 3.5 -> 3.6 change and is preserved as-is.
#
# usage: tools/dump_jni_surface.sh "path/to/game.apk" [libgodot_android.so]

set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APK="${1:?usage: $0 <apk> [libgodot_android.so]}"
LIB="${2:-$ROOT/dist/sm127/libgodot_android.so}"

for c in aarch64-none-elf-readelf /opt/devkitpro/devkitA64/bin/aarch64-none-elf-readelf readelf; do
  command -v "$c" >/dev/null 2>&1 && { RE="$c"; break; }
  [ -x "$c" ] && { RE="$c"; break; }
done
[ -n "${RE:-}" ] || { echo "no readelf (install devkitA64)" >&2; exit 1; }
[ -f "$LIB" ] || { echo "no $LIB -- run scripts/extract_apk.sh first" >&2; exit 1; }

echo "== exported JNI entry points =="
"$RE" -W --dyn-syms "$LIB" | grep -oE 'Java_org_godotengine_godot_[A-Za-z0-9_]+' | sort -u
echo
echo "== GodotLib signatures (from classes.dex) =="
python3 "$ROOT/tools/dexdump.py" "$APK" GodotLib | sort -u
echo
echo "== GodotTTS / GodotIO =="
python3 "$ROOT/tools/dexdump.py" "$APK" godot/tts | sort -u
python3 "$ROOT/tools/dexdump.py" "$APK" GodotIO | sort -u
