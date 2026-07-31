#!/usr/bin/env python3
"""Build the self-contained PCM1802 USB capture share bundle."""

import argparse
import hashlib
from pathlib import Path
import zipfile


REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
PACKAGE_DIRECTORY = "neopico-pcm1802-usb-capture"
FIXED_ZIP_TIMESTAMP = (1980, 1, 1, 0, 0, 0)
UF2_MAGIC_START = b"UF2\n"
UF2_BLOCK_BYTES = 512


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def zip_info(name: str, executable: bool = False) -> zipfile.ZipInfo:
    info = zipfile.ZipInfo(f"{PACKAGE_DIRECTORY}/{name}", FIXED_ZIP_TIMESTAMP)
    info.create_system = 3
    info.compress_type = zipfile.ZIP_DEFLATED
    info.external_attr = (0o100755 if executable else 0o100644) << 16
    return info


def build_bundle(uf2_path: Path, output_path: Path) -> None:
    sources = {
        "README.md": REPOSITORY_ROOT / "docs/PCM1802_USB_CAPTURE_BUNDLE.md",
        "capture_pcm1802_usb.py": REPOSITORY_ROOT / "scripts/capture_pcm1802_usb.py",
        "neopico_pcm1802_usb_capture.uf2": uf2_path,
    }

    payloads = {}
    for name, path in sources.items():
        if not path.is_file():
            raise FileNotFoundError(f"Required bundle input was not found: {path}")
        payloads[name] = path.read_bytes()

    uf2 = payloads["neopico_pcm1802_usb_capture.uf2"]
    if len(uf2) < UF2_BLOCK_BYTES or len(uf2) % UF2_BLOCK_BYTES != 0 or not uf2.startswith(UF2_MAGIC_START):
        raise ValueError(f"Input is not a structurally valid UF2 file: {uf2_path}")

    checksums = "".join(
        f"{sha256(payloads[name])}  {name}\n"
        for name in sorted(payloads)
    ).encode("ascii")

    output_path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(
        output_path,
        "w",
        compression=zipfile.ZIP_DEFLATED,
        compresslevel=9,
    ) as archive:
        for name in sorted(payloads):
            archive.writestr(zip_info(name, executable=name.endswith(".py")), payloads[name])
        archive.writestr(zip_info("SHA256SUMS.txt"), checksums)

    zip_digest = sha256(output_path.read_bytes())
    zip_checksum_path = output_path.with_name(output_path.name + ".sha256")
    zip_checksum_path.write_text(
        f"{zip_digest}  {output_path.name}\n",
        encoding="ascii",
    )

    print(f"Created {output_path}")
    print(f"Created {zip_checksum_path}")
    print(f"ZIP SHA-256: {zip_digest}")
    for line in checksums.decode("ascii").splitlines():
        print(line)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--uf2",
        type=Path,
        default=REPOSITORY_ROOT / "build-pcm1802-usb/src/neopico_pcm1802_usb_capture.uf2",
        help="Standalone capture UF2 to package",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=REPOSITORY_ROOT / "build-pcm1802-usb/share/neopico-pcm1802-usb-capture.zip",
        help="Output ZIP path",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    build_bundle(args.uf2.resolve(), args.output.resolve())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
