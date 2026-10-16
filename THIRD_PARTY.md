# Third-party code

| Path | Origin | License |
|------|--------|---------|
| `src/s3/airplay/`, `src/s3/airplay1/` | RAOP receiver by Philippe (philippe_44) | MIT |
| `src/s3/airplay2/pair_ap/` | pair_ap by Espen Jürgensen | MIT (see `src/s3/airplay2/pair_ap/LICENSE`) |
| `src/s3/font_inter_*.c` | Inter typeface | SIL OFL 1.1 |
| `src/s3/font_icons_*.c` | Material Symbols | Apache-2.0 |

The AirPlay 1 RSA key in `src/s3/airplay/raop_setup.c` is the publicly
known AirPort Express key used by open-source receivers; it is not a secret
of this project.

Managed components (LVGL, ESP-IDF components) are fetched at build time and
keep their own licenses.
