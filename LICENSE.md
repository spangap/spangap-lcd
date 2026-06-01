# License

This repository, **spangap-lcd** (on-device LVGL launcher + settings shell for
spangap device apps), is released under the **Apache License, Version 2.0**.

Full license text: <https://www.apache.org/licenses/LICENSE-2.0>

Copyright (c) 2026 by spangap project contributors.

## Third-party software

### Vendored in this repository

None.

### Build-time dependencies

Declared in `esp-idf/idf_component.yml`. Additionally, the consumer
buildable straddle (e.g. `hw-tdeck`) must supply LVGL and a touch driver as
managed components when `CONFIG_SPANGAP_LCD=y`. Typical set:

| Component | Source | License |
|---|---|---|
| ESP-IDF (platform) | espressif/esp-idf | Apache-2.0 |
| `lvgl/lvgl` v9     | components.espressif.com / lvgl | MIT |
| `espressif/esp_lcd_touch_gt911` (pulls `esp_lcd_touch`) | components.espressif.com | Apache-2.0 |
