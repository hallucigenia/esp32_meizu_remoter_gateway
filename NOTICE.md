# Source attribution and licensing

The Home Assistant integration under `custom_components/meizu_remoter_gateway/`
retains the MIT license and George Zhao attribution in its `LICENSE` file.
New integration changes, `tests/meizu_remoter_gateway/`, `tools/ir_compat.py`,
and `tools/package-ha-release.py` are distributed under the same MIT terms in
`LICENSE.home-assistant`.

The ESP32 firmware originates from
[georgezhao2010/esp32_meizu_remoter_gateway](https://github.com/georgezhao2010/esp32_meizu_remoter_gateway).
The inherited firmware does not contain an explicit project license. This fork
preserves the existing source attributions and does not apply the Home Assistant
MIT license to that inherited firmware. Permission for reuse or relicensing of
the inherited code must be established with its rights holders before upstreaming
it into another project.

`docs/firmware.md` preserves the upstream device and protocol documentation,
with fork-specific behavior and current installation links. Linked upstream
photographs remain hosted at their original public URLs.

The release archives contain application source or CI-built application images,
not configured-device flash dumps. No private APK, original proprietary native
library, household configuration, real household device identity, or maintenance
log is included.
