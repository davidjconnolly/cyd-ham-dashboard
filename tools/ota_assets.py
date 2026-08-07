#!/usr/bin/env python3
"""Name and verify the per-environment OTA release assets.

Every release carries one firmware image per display driver, and a board must
never install the other one: the drivers are not interchangeable and a
wrong-driver flash leaves a garbled panel that only a USB cable can recover.

The device side of that guarantee is the OTA_ASSET_NAME macro, set per
environment in platformio.ini and compiled into the image (src/ota.cpp will
only download an asset whose name matches it exactly). This script is the CI
side: it reads the same macro out of platformio.ini so the published asset
names cannot drift from the names the firmware is looking for, and it checks
each built image really does carry its own name and no other environment's.

Usage:
    python tools/ota_assets.py --list           # print "env<TAB>asset" lines
    python tools/ota_assets.py --stage DEST     # verify, then copy the bins
"""

import argparse
import configparser
import pathlib
import re
import shutil
import sys

ASSET_FLAG = re.compile(r'OTA_ASSET_NAME=\\?"([^"\\]+)\\?"')


def read_assets(project_root: pathlib.Path) -> "dict[str, str]":
    """Map each PlatformIO environment to the release asset it owns."""
    config = configparser.ConfigParser()
    config.read(project_root / "platformio.ini")

    assets = {}
    for section in config.sections():
        if not section.startswith("env:"):
            continue
        env = section[len("env:") :]
        match = ASSET_FLAG.search(config[section].get("build_flags", ""))
        if not match:
            sys.exit(
                f"[{section}] has no OTA_ASSET_NAME build flag. Every environment "
                f"must name the release asset it is allowed to install; see the "
                f"comment at the top of platformio.ini."
            )
        assets[env] = match.group(1)

    if not assets:
        sys.exit("platformio.ini declares no environments")

    duplicates = {name for name in assets.values() if list(assets.values()).count(name) > 1}
    if duplicates:
        sys.exit(
            f"OTA_ASSET_NAME is shared by more than one environment: {sorted(duplicates)}. "
            f"Each environment needs its own, or boards will install each other's images."
        )
    return assets


def stage(project_root: pathlib.Path, dest: pathlib.Path) -> None:
    assets = read_assets(project_root)
    dest.mkdir(parents=True, exist_ok=True)

    for env, asset in assets.items():
        built = project_root / ".pio" / "build" / env / "firmware.bin"
        if not built.is_file():
            sys.exit(f"[{env}] {built} is missing; build the environment first")

        # The macro is a plain string constant, so it survives into the image.
        # Finding the wrong one here means the build flags and the environment
        # directories have been crossed somewhere.
        blob = built.read_bytes()
        if asset.encode() not in blob:
            sys.exit(f"[{env}] {built} does not contain its own asset name '{asset}'")
        for other_env, other_asset in assets.items():
            if other_env != env and other_asset.encode() in blob:
                sys.exit(
                    f"[{env}] {built} contains '{other_asset}', which belongs to "
                    f"[{other_env}]. A board built from this image could install "
                    f"the wrong display variant."
                )

        shutil.copyfile(built, dest / asset)
        print(f"{env}: {built} -> {dest / asset} ({len(blob)} bytes)")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--list", action="store_true", help="print env/asset pairs")
    group.add_argument("--stage", metavar="DEST", help="verify and copy the built images")
    args = parser.parse_args()

    project_root = pathlib.Path(__file__).resolve().parent.parent
    if args.list:
        for env, asset in read_assets(project_root).items():
            print(f"{env}\t{asset}")
    else:
        stage(project_root, pathlib.Path(args.stage))


if __name__ == "__main__":
    main()
