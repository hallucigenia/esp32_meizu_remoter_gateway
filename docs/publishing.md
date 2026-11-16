# Maintaining public releases

## Release streams

Firmware uses `vX.Y.Z` tags and publishes `firmware.ota.bin` with `manifest.json`.
The existing gateway update client follows `releases/latest/download/manifest.json`.
Only a hardware-validated firmware release should be marked as the latest stable
release.

Home Assistant integrations use `ha-vX.Y.Z` tags. Build their source-only archive
with Python 3.11 or newer:

```sh
python3 tools/package-ha-release.py --output release-ha
```

The archive includes only `custom_components/meizu_remoter_gateway/`; its version
comes from `manifest.json`. The packager also writes a SHA-256 checksum file.
The source, tests and documentation remain available in the tagged repository.
Create the HA release with `--latest=false` so that it does not become the
gateway firmware manifest endpoint. The firmware workflow only publishes for
tags beginning with `v`, so an `ha-v` tag does not publish a new firmware version.

## Validation

Run the Python unit tests and firmware sanitizer tests described in the repository
README. Check the ZIP's exact file list, unpack it into an empty directory and
compare every file against the reviewed integration source. The packaging
workflow repeats the Python tests and uploads the generated ZIP and checksum as
CI artifacts.

Before pushing, inspect the staged diff and commit author/committer metadata.
Use the maintainer's GitHub noreply address for public commits. Preserve upstream
attribution and the scoped license notices.

## Public data boundary

Examples use synthetic gateway serials such as `ABC123`, locally administered
Bluetooth addresses such as `02:00:00:00:00:01`, and generic entity names. Test keys
are deterministic fixtures, never credentials from a real gateway. Waveform
fixtures contain raw pulse/code bytes and generic command names only.

Do not commit or attach full-flash backups, NVS, Wi-Fi credentials, OTA keys,
Home Assistant `.storage`, private hostnames, real device MACs/serials, local user
paths, APKs or proprietary native libraries. Local operation records and audit
reports stay outside the published tree. Ignore rules are a convenience, not a
substitute for inspecting the actual staged files and release archive.

Existing stable firmware tags and release attachments are not rewritten when
publishing HA source. Historical GitHub object caches are outside the scope of a
new release and must not be described as erased by a clean current-tree scan.
