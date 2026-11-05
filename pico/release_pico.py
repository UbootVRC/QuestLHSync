"""Packs the Pico 4 test release into out/release-<version>-pico/ from what build.bat and pico/build_module.py built:

  QuestLHSync-pico-module-<version>.zip    the Pico 4's Magisk module
  QuestLHSync-steamvr-pico-<version>.zip   the SteamVR driver folder questlhsync/ with Pico 4 support (register it by
                                           hand: the official installer downloads CreoleVR's release, without it)

Each package carries LICENSE and THIRD_PARTY_NOTICES.md, as the official ones do. Refuses when a build is older than
its sources.
"""
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, ROOT)
sys.path.insert(0, os.path.join(ROOT, "magisk"))
import release  # noqa: E402  (steamvr_zip, module_zip, newest)

VER = release.VERSION.lstrip("v")
PICO_MODULE = os.path.join(ROOT, "out", f"QuestLHSync-pico-module-{VER}.zip")


def main():
    os.chdir(ROOT)  # release.newest() takes paths relative to the repo
    checks = [
        (os.path.join(release.BIN, "driver_questlhsync.dll"), release.newest("src/driver", "src/common", "third_party"),
         "build.bat"),
        (os.path.join(release.BIN, "QuestLHSync.exe"), release.newest("src/overlay", "src/common", "third_party"),
         "build.bat"),
        (PICO_MODULE, release.newest("pico/src", "pico/module", "magisk/src", "src/headset"),
         "python pico\\build_module.py"),
    ]
    for path, src, how in checks:
        if not os.path.exists(path) or os.path.getmtime(path) < src:
            sys.exit(f"{os.path.relpath(path, ROOT)} is missing or older than its sources: run {how}")
    dest = os.path.join(ROOT, "out", f"release-{VER}-pico")
    shutil.rmtree(dest, ignore_errors=True)
    os.makedirs(dest)
    release.MODULE = PICO_MODULE
    release.module_zip(os.path.join(dest, f"QuestLHSync-pico-module-{VER}.zip"))
    release.steamvr_zip(os.path.join(dest, f"QuestLHSync-steamvr-pico-{VER}.zip"))
    for f in sorted(os.listdir(dest)):
        print(f"  {f}  ({os.path.getsize(os.path.join(dest, f)) / 1e6:.1f} MB)")
    print(f"built {dest}")


if __name__ == "__main__":
    main()
