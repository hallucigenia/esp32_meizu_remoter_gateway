# Gateway OTA

Firmware 0.7.3 adds authenticated device-side HTTPS OTA, compatible with the
ESPHome managed-update manifest shape. The Home Assistant integration needs 1.4.0.
The existing TCP protocol, bindings, measurement interval, IR acknowledgements and
learning transactions remain the business interface.

## Initial installation

An older firmware has no OTA service. Back up the full flash, identify the board,
read its actual partition table and OTA selector, and verify the active application.
This target is ESP32 with 4 MB flash and the measured `esp32-4m-ab-v1` layout:

| Partition | Address | Size |
| --- | --- | --- |
| nvs | 0x9000 | 0x5000 |
| otadata | 0xe000 | 0x2000 |
| app0 | 0x10000 | 0x1c0000 |
| app1 | 0x1d0000 | 0x1c0000 |
| eeprom | 0x390000 | 0x1000 |
| spiffs | 0x391000 | 0xf000 |

On this existing board, install the matching rollback-enabled bootloader at
0x1000 and the application in the verified active slot through USB. The
bootloader must fit below the existing partition table at 0x8000. Do not use
`idf.py flash`, erase flash, or overwrite NVS, partition table, or OTA selectors.
An application-only upload does not upgrade an older bootloader.

After boot, enter `ota-key` through the private 115200-baud USB serial console.
The device returns its randomly generated 64-character per-device key. In HA,
open the gateway integration's reconfigure form and enter that key. It is tested
using a read-only authenticated operation. Blank input preserves the saved key.
Public application images contain neither this key nor Wi-Fi credentials. The
key remains in a dedicated NVS namespace; HA stores it in the config entry.

## Updates

The gateway's HA firmware entity checks the latest stable release every six
hours. Installing downloads the immutable release image directly on the gateway,
using ESP-IDF `esp_https_ota` in a separate task. A signed per-device approval
binds a one-use 60-second nonce to the gateway serial, request ID, version, URL,
file length, SHA-256 and current time. The approved time initializes the device
clock for TLS certificate-date validation. CA validation is always enabled.

Only this repository's fixed versioned HTTPS release-asset URL is accepted.
The board/layout, slot size, embedded project/version, actual received length and
exact file-byte SHA-256 are checked before committing the boot selector. MD5 in
the manifest exists for ESPHome shape compatibility; the gateway checks SHA-256.
No eFuses or hardware secure-boot state are changed. This is authenticated OTA,
not a claim that unsigned code cannot be installed with physical USB access.

IR/learning/binding must be idle before installation. An in-flight measurement
can finish; subsequent BLE work is paused. New IR requests fail explicitly rather
than accumulating. Recovery-mode failures restore the maintenance service. In normal mode, the
Bluetooth stack is stopped for transfer; failure/cancellation reboots the unchanged
application to recreate GATT registrations and resume polling. Transfers
have a five-minute bound and 15-second network read timeout. Disconnecting HA
does not orphan the transfer: the device owns it, and the durable request ID is
queried after reconnect/restart. Ambiguous results never trigger automatic reflash.

After reboot, the new slot remains pending verification. Stable local services
and Wi-Fi for at least 60 seconds confirm it; a failed startup or reset before
confirmation lets the bootloader select the previous valid slot. The HA install
finishes only after matching the request, target version, digest and confirmed
boot. A single remote being offline does not make a boot fail.

Three unsuccessful baseline boots activate recovery mode on the next boot.
Recovery runs Wi-Fi, the existing minimal TCP control service and OTA, without
initializing BLE. NVS initialization errors preserve all configuration rather
than erasing it. Firmware schema changes must remain readable by the previous
application until rollback protection is no longer needed.

## Build and publish

Build with ESP-IDF 4.3.7. `PROJECT_VER` can be supplied to CMake; the default is
0.7.3. CI runs sanitizer host tests, builds, checks app and bootloader sizes and
packages `firmware.ota.bin` and `manifest.json`. Pushing a stable `vX.Y.Z` tag
publishes those two public artifacts as a prerelease only after the build succeeds.
After physical validation, promote the candidate with `gh release edit vX.Y.Z
--prerelease=false --latest`; HA only follows stable releases. The publish
job alone receives repository write permission. Device backups must never be
added to releases.

The application supports `ota_status`, `ota_challenge`, `ota_auth`, `ota_start`
and `ota_cancel`. Starts/cancellations/auth checks use HMAC-SHA256 over the
newline-separated contract in `main/OtaPolicy.cpp`; commands remain below the
existing 511-byte TCP input limit. Status never returns the key. An active
request ID is idempotent, and completed/failed IDs cannot start another write.

ESP-IDF 4.3.7 requires Wi-Fi MIN_MODEM while the Bluetooth coexistence controller
is enabled. Normal operation retains that mode (driver value 1). OTA first waits
for radio work to stop, deinitializes Bluedroid/controller, then uses Wi-Fi NONE
(driver value 0). Recovery skips Bluetooth and also uses NONE. Diagnostics read
back the actual driver mode and Wi-Fi RSSI. This avoids changing the old SDK
coexistence contract while keeping the maintenance transfer responsive.
