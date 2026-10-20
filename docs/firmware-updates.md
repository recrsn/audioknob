# Firmware updates

Both chips can be updated from Settings → Web page. About keeps **Choose file…**
for local application `.bin` files and also offers **Check for updates** and
separate download buttons for the main and Bluetooth chips. The device itself
has **Settings → About → Check for updates**.

The S3 checks the latest stable GitHub release after WiFi and clock sync are
ready, then every 24 hours while running. Failed automatic checks retry after
an hour; offline checks wait for connectivity. Each boot starts a fresh check.
Manual checks run immediately when online, or explain that WiFi/clock sync is
still needed. Checks wait while an image is receiving, awaiting confirmation,
or installing. There is no automatic download or installation.

A newer release wakes the display with an **Update available** screen. Press
✓ or the knob to download; Cancel or a long press dismisses it. The same
release stays dismissed for this boot until a manual check is requested. If
both chips have updates, the device offers the main chip first; the web page
lets you choose either. After updating/restarting the main chip, the next
check offers Bluetooth if it still needs updating.

Downloads run in the background so the web page can keep reporting progress.
They follow GitHub's HTTPS redirects with certificate verification, stream
into the existing OTA path, and validate the selected chip before erasing its
slot. The device asks again before installing the verified image. Existing
rollback and Bluetooth UART installation behavior applies to both sources.
Audio pauses while writing and through the install confirmation.

## Publishing a compatible release

The default public repository is `recrsn/audioknob`. Forks can change
`CONFIG_AK_UPDATE_REPOSITORY` in the S3's menuconfig.

Use stable version tags such as `v1.2.3`. Both application images must embed
the matching version (`1.2.3` or `v1.2.3`): ESP-IDF accepts a project-root
`version.txt`, or derives the version from `git describe`. Exact versions
ignore a leading `v`; numeric `major.minor.patch` versions also prevent
announcing a downgrade. A development/git-hash version is treated as
updateable when it differs from the release tag.

Publishing a GitHub release runs `.github/workflows/release-firmware.yml`.
It checks out the exact release tag, writes that tag into both projects'
`version.txt`, builds both chips with PlatformIO 6.2.0, checks partition limits,
and verifies each binary's chip ID and embedded version before attaching
`firmware-s3.bin` and `firmware-esp32.bin` to the release. A failed build or
validation prevents the upload step. Re-running the workflow replaces assets
with the same names.

The workflow also supports **Actions → Release firmware → Run workflow** with
an existing release tag, including a draft release. For immutable releases,
run it against the draft first, verify both assets are attached, then publish;
published immutable releases reject asset changes. The workflow must be on
the default branch for the manual run option to appear.

For a local build, run PlatformIO:

```sh
pio run -d firmware/s3 -e s3
pio run -d firmware/esp32 -e esp32
```

CI attaches these raw application binaries (use the same names for manual uploads):

| Build output | Release asset name | Maximum size |
| --- | --- | --- |
| `firmware/s3/.pio/build/s3/firmware.bin` | `firmware-s3.bin` | 4 MiB |
| `firmware/esp32/.pio/build/esp32/firmware.bin` | `firmware-esp32.bin` | 1 MiB |

A release may contain either or both images. Merged flash dumps, bootloaders,
and partition tables are not OTA application images. Drafts and prereleases
are excluded by GitHub's latest-release endpoint and checked again locally.
The API requires a public repository and no token; rate-limit failures are
shown on manual checks and the web page. Missing or incompatible assets are
reported as errors, rather than claiming the device is up to date.

## Verification

`python3 tests/ota/run.py` runs sanitizer-enabled host tests against ESP-IDF's
cJSON and mocked OTA transports, plus web renderer tests (requires a C compiler,
Node.js, and PlatformIO's ESP-IDF package, or `IDF_PATH`). It covers version
ordering, malformed/untrusted release metadata, partition bounds, concurrent
updates, physical confirmation, wrong-chip images, partial transfers, task
creation failure, daily scheduling, hourly retries, connectivity gates, dismissal,
and web controls. Build the `s3` and `native` environments to
check device integration and the portable UI. The native Settings check shows
a sample available release and simulates its download/confirmation flow.

Hardware checks still required: a real GitHub download through its CDN,
on-device confirmation/decline, daily checks over a 24-hour run, Bluetooth
relay, and rollback after a failed boot.
