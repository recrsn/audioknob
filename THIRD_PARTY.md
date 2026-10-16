# Third-party code

| Path | Origin | License |
|------|--------|---------|
| `firmware/s3/src/airplay/shared/`, `firmware/s3/src/airplay/v1/` | RAOP receiver by Philippe (philippe_44) | MIT |
| `third_party/pair_ap/` | pair_ap by Espen Jürgensen | MIT (see `third_party/pair_ap/LICENSE`) |
| `firmware/s3/src/assets/font_inter_*.c` | Inter typeface | SIL OFL 1.1 |
| `firmware/s3/src/assets/font_icons_*.c` | Material Symbols | Apache-2.0 |

The AirPlay 1 RSA key in `firmware/s3/src/airplay/shared/raop_setup.c` is the publicly
known AirPort Express key used by open-source receivers; it is not a secret
of this project.

Managed components (LVGL, ESP-IDF components) are fetched at build time and
keep their own licenses.
