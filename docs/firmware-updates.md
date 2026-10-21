# Firmware updates

Both chips can be updated from Settings → Web page. About keeps **Choose file…**
for local application `.bin` files and also offers **Check for updates** and
separate download buttons for the main and Bluetooth chips. The device itself
has **Settings → About → Check for updates**.

The S3 checks the latest stable GitHub release once after WiFi and clock sync
are ready at startup, and when manually requested. Offline manual requests
coalesce and remain queued until readiness. There is no daily polling or hourly
release retry: a finished error stays visible until the next manual check or
boot. Checks wait while an image is receiving, awaiting confirmation, or
installing, with a 60-second deferral while the worker blocks. There is no
automatic download or installation.

Discovery, reserved downloads, location, weather and location persistence share
one worker with a
static internal stack and task control block, reused between jobs. A timer
dispatches ready jobs without performing network or flash work itself. The
manifest, notice and last-check timestamp remain available between jobs.
`nextCheck` remains in the JSON API as zero (no periodic deadline); the page
shows the last check and offers Check Now.

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

Use stable version tags such as `v1.2.3`. The main image embeds the release
version (`1.2.3` or `v1.2.3`). Bluetooth keeps its own version until its sources
or shared protocol change. For local builds, ESP-IDF accepts a project-root
`version.txt`, or derives the version from `git describe`. Exact versions
ignore a leading `v`; numeric `major.minor.patch` versions also prevent
announcing a downgrade. A development/git-hash version is treated as
updateable when it differs from the release tag.

Publishing a GitHub release runs `.github/workflows/release-firmware.yml`.
It checks out the exact release tag and always builds the main image. It builds
Bluetooth only when `firmware/esp32/` or `firmware/lib/common/` differs from the
previous stable release containing a Bluetooth asset. The first release builds
both images. When Bluetooth is unchanged, CI copies the verified prior binary
without changing its embedded version. Thus a main-only release does not ask
current devices to update Bluetooth, while devices that skipped earlier
releases can still fetch the latest Bluetooth image from the current release.

CI labels each asset `version:<embedded-version>`. The updater uses each
asset's label when comparing versions, falling back to the release tag for
older unlabeled releases such as `v0.1.0`. The device prompt and web download
buttons show the selected chip's own version.

Before attaching anything, CI verifies each binary's chip ID, embedded version,
size and appended image checksum. It also publishes `SHA256SUMS` covering the
complete files, including reused Bluetooth firmware. For manual downloads:

```sh
sha256sum --check SHA256SUMS  # macOS: shasum -a 256 --check SHA256SUMS
```

The device requires a valid SHA-256 digest from GitHub's asset metadata,
retrieved over verified HTTPS. It hashes the complete downloaded file while
streaming and rejects any mismatch before marking the image ready for physical
confirmation. Missing, malformed or non-SHA-256 asset digests fail the release
check. Existing image validation and rollback run in addition to this check.
A rerun of the workflow replaces assets with the same names.

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
Node.js, OpenSSL development libraries via `pkg-config`, and PlatformIO's ESP-IDF
package, or `IDF_PATH`). It covers version
ordering, malformed/untrusted release metadata, partition bounds, concurrent
updates, physical confirmation, wrong-chip images, partial transfers, task
creation failure, startup/manual scheduling, readiness and OTA gates, dismissal,
web controls, independent Bluetooth versioning, release source-change detection,
missing/malformed asset digests, corrupted downloads, published checksums, and
GitHub CDN redirects whose signed request lines exceed ESP-IDF's default
512-byte transmit buffer. The release HTTP client reserves 2048 bytes for
these request lines and logs the failing stage if a request cannot be opened. Build the `s3` and `native` environments to
check device integration and the portable UI. The native Settings check shows
a sample available release and simulates its download/confirmation flow.

Hardware checks still required: a real GitHub download through its CDN,
on-device confirmation/decline, startup/manual checks and retained notices, Bluetooth
relay, and rollback after a failed boot.
