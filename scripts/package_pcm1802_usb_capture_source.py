#!/usr/bin/env python3
"""Export the standalone PCM1802 USB capture source as a ZIP archive."""

import argparse
from pathlib import Path
import zipfile


REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
PACKAGE_DIRECTORY = "neopico-pcm1802-usb-capture-source"
FIXED_ZIP_TIMESTAMP = (1980, 1, 1, 0, 0, 0)

SOURCE_FILES = {
    "CMakeLists.txt": REPOSITORY_ROOT / "packaging/pcm1802_usb_capture_source/CMakeLists.txt",
    "LICENSE": REPOSITORY_ROOT / "LICENSE",
    "README.md": REPOSITORY_ROOT / "packaging/pcm1802_usb_capture_source/README.md",
    "src/i2s_capture.pio": REPOSITORY_ROOT / "src/audio/i2s_capture.pio",
    "src/main.c": REPOSITORY_ROOT / "src/diagnostics/pcm1802_usb_capture/main.c",
    "src/tusb_config.h": REPOSITORY_ROOT / "src/diagnostics/pcm1802_usb_capture/tusb_config.h",
    "src/usb_descriptors.c": REPOSITORY_ROOT / "src/diagnostics/pcm1802_usb_capture/usb_descriptors.c",
    "tools/capture_pcm1802_usb.py": REPOSITORY_ROOT / "scripts/capture_pcm1802_usb.py",
}


def zip_info(name: str, executable: bool = False) -> zipfile.ZipInfo:
    info = zipfile.ZipInfo(f"{PACKAGE_DIRECTORY}/{name}", FIXED_ZIP_TIMESTAMP)
    info.create_system = 3
    info.compress_type = zipfile.ZIP_DEFLATED
    info.external_attr = (0o100755 if executable else 0o100644) << 16
    return info


def build_source_bundle(output_path: Path) -> None:
    payloads = {}
    for name, path in SOURCE_FILES.items():
        if not path.is_file():
            raise FileNotFoundError(f"Required source file was not found: {path}")
        payloads[name] = path.read_bytes()

    output_path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(
        output_path,
        "w",
        compression=zipfile.ZIP_DEFLATED,
        compresslevel=9,
    ) as archive:
        for name in sorted(payloads):
            archive.writestr(
                zip_info(name, executable=name.endswith(".py")),
                payloads[name],
            )

    print(f"Created {output_path}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--output",
        type=Path,
        default=(
            REPOSITORY_ROOT
            / "build-pcm1802-usb/share/neopico-pcm1802-usb-capture-source.zip"
        ),
        help="Output ZIP path",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    build_source_bundle(args.output.resolve())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
