#!/usr/bin/env python3
"""import_gap.py -- which imports of the loaded modules the wrapper cannot satisfy.

so_resolve() taints an unresolved import so it traps with a name when called.
That finds one gap per boot on hardware; this finds all of them before the
first build.

For each module the port loads, take its undefined dynamic symbols and subtract
what the wrapper resolves:

  - the DynLibFunction table in source/imports.c
  - the GL entry points in source/gl_imports.inc
  - symbols exported by libc++_shared.so (so_resolve chains across modules)

WEAK leftovers are fine -- so_resolve leaves them 0 and the engine checks.
GLOBAL leftovers are real work: add them to imports.c.

usage: tools/import_gap.py [dist/sm127]
"""

import pathlib
import re
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC = ROOT / "source"


def readelf():
    for cand in ("aarch64-none-elf-readelf", "/opt/devkitpro/devkitA64/bin/aarch64-none-elf-readelf", "readelf"):
        if shutil.which(cand) or pathlib.Path(cand).exists():
            return cand
    sys.exit("no readelf (install devkitA64)")


def dyn_syms(path):
    out = subprocess.run([readelf(), "-W", "--dyn-syms", str(path)],
                         capture_output=True, text=True, check=True).stdout
    und, exp = [], set()
    for line in out.splitlines():
        p = line.split()
        if len(p) < 8 or not p[0].rstrip(":").isdigit():
            continue
        name = p[7].split("@")[0]
        if not name:
            continue
        (und.append((p[4], name)) if p[6] == "UND" else exp.add(name))
    return und, exp


def main():
    libdir = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "dist" / "sm127"
    table = set(re.findall(r'\{\s*"([A-Za-z_0-9]+)"\s*,', (SRC / "imports.c").read_text()))
    table |= set(re.findall(r'"([A-Za-z_0-9]+)"', (SRC / "gl_imports.inc").read_text()))

    cxx_und, cxx_exp = dyn_syms(libdir / "libc++_shared.so")
    god_und, _ = dyn_syms(libdir / "libgodot_android.so")

    # Any other .so next to them is a GDNative/GDExtension module the engine
    # dlopen()s; it resolves through the same table (native_modules.c).
    mods = [("libc++_shared.so", cxx_und, set()),
            ("libgodot_android.so", god_und, cxx_exp)]
    for extra in sorted(libdir.glob("*.so")):
        if extra.name not in ("libc++_shared.so", "libgodot_android.so"):
            mods.append((extra.name, dyn_syms(extra)[0], cxx_exp))

    bad = 0
    for name, und, chained in mods:
        glob = sorted(n for b, n in und if b != "WEAK" and n not in table and n not in chained)
        weak = sorted(n for b, n in und if b == "WEAK" and n not in table and n not in chained)
        print(f"{name}: {len(und)} imports, {len(glob)} GLOBAL uncovered, {len(weak)} WEAK left at 0")
        for n in glob:
            print(f"    MISSING  {n}")
        bad += len(glob)
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
