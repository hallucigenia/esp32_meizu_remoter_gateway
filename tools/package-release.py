#!/usr/bin/env python3
"""Package public app-only OTA assets; never read device backups or credentials."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil


def package(image_path, output, version, bootloader=None):
    if not re.fullmatch(r"\d+\.\d+\.\d+", version):
        raise ValueError("Release version must be stable major.minor.patch")
    image = Path(image_path).read_bytes()
    if not 1024 <= len(image) <= 0x1C0000 or image[0] != 0xE9:
        raise ValueError("Invalid or oversized application image")
    if image[32:36] != bytes.fromhex("3254cdab"):
        raise ValueError("Missing ESP-IDF application descriptor")
    actual = image[48:80].split(b"\0", 1)[0].decode()
    project = image[80:112].split(b"\0", 1)[0].decode()
    if actual != version or project != "esp32_meizu_remoter_gateway":
        raise ValueError("Image project/version does not match release")
    if bootloader and Path(bootloader).stat().st_size > 0x7000:
        raise ValueError("Bootloader overlaps the existing partition table")
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    root = "https://github.com/hallucigenia/esp32_meizu_remoter_gateway/releases"
    manifest = {"schema": 1, "name": "Meizu BLE Gateway", "version": version,
                "board": "esp32-4m-ab-v1", "min_integration": "1.4.0",
                "builds": [{"chipFamily": "ESP32", "ota": {
                    "path": f"{root}/download/v{version}/firmware.ota.bin",
                    "size": len(image), "sha256": hashlib.sha256(image).hexdigest(),
                    "md5": hashlib.md5(image).hexdigest(),
                    "release_url": f"{root}/tag/v{version}",
                    "summary": "HA 原生固件更新、认证 HTTPS 下载、A/B 首启回滚与恢复模式；保留原生学码和发码应答。"}}]}
    shutil.copyfile(image_path, output / "firmware.ota.bin")
    (output / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n")
    return manifest


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--image", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--bootloader")
    args = parser.parse_args()
    manifest = package(args.image, args.output, args.version, args.bootloader)
    print(json.dumps({"version": args.version, **manifest["builds"][0]["ota"]}, ensure_ascii=False))
