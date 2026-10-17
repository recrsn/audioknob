# S3 flash layout and fresh installation

The S3 has 16 MiB of flash. Each OTA application slot is 4 MiB; the separate
ESP32 firmware staging area on the S3 is 1 MiB. The ESP32 chip's own 4 MiB flash
keeps its existing two 0x1f0000-byte OTA slots: the 1 MiB limit here applies to
the image staged on the S3 for transfer over UART.

| Region | Start | End (exclusive) | Size |
| --- | --- | --- | --- |
| Bootloader reservation | 0x000000 | 0x008000 | 32 KiB |
| Partition table | 0x008000 | 0x009000 | 4 KiB |
| NVS | 0x009000 | 0x00f000 | 24 KiB |
| OTA selection | 0x00f000 | 0x011000 | 8 KiB |
| PHY initialization | 0x011000 | 0x012000 | 4 KiB |
| Application alignment gap | 0x012000 | 0x020000 | 56 KiB |
| OTA 0 | 0x020000 | 0x420000 | 4 MiB |
| OTA 1 | 0x420000 | 0x820000 | 4 MiB |
| ESP32 staging | 0x820000 | 0x920000 | 1 MiB |
| Core dump | 0x920000 | 0x930000 | 64 KiB |
| Saved data (SPIFFS) | 0x930000 | 0x1000000 | 6.8125 MiB |

All app offsets are 64 KiB aligned; all data partitions are sector aligned.
The system partitions retain the conventional offsets. Shrinking both OTA
slots and staging moves the core dump and grows saved data by 5 MiB.
The complete CSV is `firmware/s3/partitions_s3.csv`.

Settings → About → Storage used shows used/total mounted capacity, as does the
web About section. These values exclude SPIFFS overhead. Writes preserve the
last quarter for garbage collection; it is not all available for new files.
Unavailable storage is labelled explicitly. The API reports exact bytes as
`storageUsedBytes` and `storageTotalBytes` in `GET /api/state`.

## Fresh flash

This migration requires a full erase followed by bootloader, partition table,
OTA selection and application installation. An application-only OTA cannot
change the table. The erase removes WiFi credentials, settings, Bluetooth
bonds, messages and recordings. Set up the device again afterward.

Build and check first:

```sh
pio run -d firmware/s3 -e s3
pio run -d firmware/esp32 -e esp32
python3 tests/partitions/check.py
```

Connect each chip's USB bridge and use its explicit port. For the S3:

```sh
pio run -d firmware/s3 -e s3 -t erase --upload-port S3_PORT
pio run -d firmware/s3 -e s3 -t upload --upload-port S3_PORT
```

For the ESP32 Bluetooth MCU:

```sh
pio run -d firmware/esp32 -e esp32 -t erase --upload-port ESP32_PORT
pio run -d firmware/esp32 -e esp32 -t upload --upload-port ESP32_PORT
```

The build-generated `flasher_args.json` records the exact offsets. In a
packaged artifact bundle, merged factory images include those same boot and
application segments and are written at **0x0** after erasing, using esptool
5.x (from PlatformIO's tool-esptoolpy package, or a Python environment with
esptool installed):

```sh
python3 -m esptool --chip esp32s3 --port S3_PORT erase-flash
python3 -m esptool --chip esp32s3 --port S3_PORT write-flash 0x0 s3-factory.bin
python3 -m esptool --chip esp32 --port ESP32_PORT erase-flash
python3 -m esptool --chip esp32 --port ESP32_PORT write-flash 0x0 esp32-factory.bin
```

Saved data starts erased; the firmware formats it on first boot. Subsequent
ordinary OTA updates preserve it. Damaged non-erased data is never silently
formatted. Hardware checks remain pending until a board is freshly flashed.
