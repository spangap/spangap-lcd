# License

This repository, **spangap-lcd** (on-device LVGL launcher + settings shell for
spangap device apps), is released under the **Apache License, Version 2.0**.

Full license text: <https://www.apache.org/licenses/LICENSE-2.0>

Copyright (c) 2026 by spangap project contributors.

## Third-party software

### Vendored in this repository

| What | Source | License |
|---|---|---|
| `esp-idf/src/lcd_ui/lcd_keygrid.{c,h}`, `lcd_keygrid_private.h` | LVGL v9 `lv_buttonmatrix` | MIT |
| `esp-idf/src/lcd_ui/lcd_keys.{c,h}`, `lcd_keys_private.h` | LVGL v9 `lv_keyboard` | MIT |

Both are **forks, not copies held at arm's length**: the on-screen keyboard
needs key markings, long-press alternates and per-key hit margins that the
upstream widgets do not offer, so they were taken into this repository to be
changed. Each file names its origin and lists how it has diverged.

### Build-time dependencies

Declared in `esp-idf/idf_component.yml`. Additionally, the consumer
buildable straddle (e.g. `hw-lilygo-tdeck`) must supply LVGL and a touch driver as
managed components when `CONFIG_SPANGAP_LCD=y`. Typical set:

| Component | Source | License |
|---|---|---|
| ESP-IDF (platform) | espressif/esp-idf | Apache-2.0 |
| `lvgl/lvgl` v9     | components.espressif.com / lvgl | MIT |
| `espressif/esp_lcd_touch_gt911` (pulls `esp_lcd_touch`) | components.espressif.com | Apache-2.0 |
