"""Builds the QuestLHSync Magisk module for the Pico 4: out/QuestLHSync-pico-module-<version>-pico<build>.zip

  lhsyncd       ../src/headset/lhsyncd.c (shared) + src/pico.c, a static aarch64 musl binary built with Zig (no NDK
                needed: pico.c uses no Android library, it reads properties through getprop)
  lhsight.js    ../magisk/src/lhsight.js with ../magisk/src/lhsight.c inlined (Frida CModule), shared with the Quest
  frida-inject  the Quest module's: Frida 17.10.0 android-arm64, downloaded once into ../magisk/frida (hash-checked)

Zig: `zig` on PATH, or ZIG set to it.
"""
import os
import shutil
import subprocess
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT = os.path.join(ROOT, "out")
HEADSET = os.path.join(ROOT, "src", "headset")
sys.path.insert(0, os.path.join(ROOT, "magisk"))
import build_module as quest  # noqa: E402  (VERSION, frida_inject, lf)

sys.path.insert(0, HERE)
from pico_version import PICO_BUILD  # noqa: E402

VERSION = quest.VERSION
LABEL = f"{VERSION}-pico{PICO_BUILD}"  # v1.28-pico2
CODE = PICO_BUILD  # Magisk's versionCode: each test build an update


def zig():
    exe = os.environ.get("ZIG") or shutil.which("zig")
    if not exe:
        sys.exit("zig not found: put it on PATH or set ZIG")
    return exe


def main():
    os.makedirs(os.path.join(HERE, "build"), exist_ok=True)
    os.makedirs(OUT, exist_ok=True)
    daemon = os.path.join(HERE, "build", "lhsyncd")
    subprocess.run([zig(), "cc", "-target", "aarch64-linux-musl", "-static", "-O2", "-Wall", "-Wno-unused-result",
                    f'-DMODULE_VERSION="{LABEL}"', f"-I{HEADSET}", os.path.join(HEADSET, "lhsyncd.c"),
                    os.path.join(HERE, "src", "pico.c"), "-s", "-o", daemon], check=True)
    msrc = os.path.join(ROOT, "magisk", "src")
    js = open(os.path.join(msrc, "lhsight.js"), encoding="utf-8").read()
    c = open(os.path.join(msrc, "lhsight.c"), encoding="utf-8").read()
    js = js.replace("@CSRC@", c).replace("@DUR@", "0")
    prop = open(os.path.join(HERE, "module", "module.prop.in"), encoding="utf-8").read()
    prop = prop.replace("@VERSION@", LABEL).replace("@CODE@", str(CODE))
    zpath = os.path.join(OUT, f"QuestLHSync-pico-module-{LABEL.lstrip('v')}.zip")
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED) as z:
        def add(name, data, mode=0o644):
            info = zipfile.ZipInfo(name, date_time=(2026, 11, 5, 0, 0, 0))
            info.external_attr = (0o100000 | mode) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(info, data)
        m = os.path.join(HERE, "module")
        for rel in ("META-INF/com/google/android/update-binary", "META-INF/com/google/android/updater-script",
                    "service.sh", "customize.sh", "uninstall.sh"):
            add(rel, quest.lf(open(os.path.join(m, rel), encoding="utf-8").read()),
                0o755 if rel.endswith((".sh", "binary")) else 0o644)
        add("module.prop", quest.lf(prop))
        add("lhsight.js", quest.lf(js))
        add("lhsyncd", open(daemon, "rb").read(), 0o755)
        add("frida-inject", quest.frida_inject(), 0o755)
    print(f"built {zpath} ({os.path.getsize(zpath) / 1e6:.1f} MB)")
    return zpath


if __name__ == "__main__":
    sys.exit(0 if main() else 1)
