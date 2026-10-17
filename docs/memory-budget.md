# Radio internal-memory regression (static analysis)

## Evidence

After flashing the Talk/inbox build, the saved `/api/state` readings showed
15–18 KiB of free internal RAM and over 6 MiB free PSRAM. The earlier verified
radio firmware had about 40 KiB of free internal RAM. The user subsequently
reported running out of memory while starting radio. No failure log was
available for this analysis, so the exact failed allocation is unconfirmed.

The radio start path reserved two stacks explicitly from internal RAM:

| Allocation | Size | Lifetime |
| --- | ---: | --- |
| `messages` stack | 6 KiB | Always allocated after inbox initialization |
| `note_store` stack | 6 KiB | Always allocated after voice-note initialization |
| `radio_net` stack | 8 KiB | While a radio session is running |
| `radio_dec` stack | 8 KiB | While a radio session is running |
| Talk connection stacks | 6 KiB each, at most two | Only while connected |

Task control blocks, message queues, locks, SPIFFS buffers and network state
consume additional memory. Total free bytes do not guarantee a contiguous
8 KiB block for a task stack. The two radio task-creation failure paths both
report `Out of memory`.

The large radio ring, decoder PCM buffers, codec allocation hooks and LVGL
allocator already prefer/use PSRAM. The filesystem's cache and work-buffer sizes
are determined by `max_files` (5) and the 256-byte page size, rather than the
partition's capacity. Enlarging the flash partition does not allocate a
similarly sized RAM buffer.

The current sdkconfig also prefers internal memory for ordinary allocations up
to 16 KiB, with fallback to other eligible heaps. Thus small HTTP allocations
add pressure; explicit internal-only task stacks cannot fall back to PSRAM.

## Minimal fix

Keep each radio worker's 8 KiB stack depth, priority and core affinity, but
allocate both stacks in PSRAM with `xTaskCreatePinnedToCoreWithCaps`. Their exits
already call `vTaskDeleteWithCaps`. This removes a 16 KiB internal-stack demand
when starting radio; it does not change the idle heap reading.

The radio workers perform network reads, read-only TLS certificate handling,
codec operations and I2S output. They do not write partitions, NVS or files.
The project already enables external task stacks and uses them for Talk and
voice-note codec workers. Storage/message workers and incoming Talk connection
handlers still need internal stacks because they write flash.

No global malloc policy, partition layout, storage format, audio buffer size or
codec stack depth changes are needed for this fix.

## Verification limits

The S3 build checks the patched task-creation code against the configured IDF.
Host LAN voice checks cover source arbitration and persistence but cannot model
ESP32 capability heaps or prove radio performance with PSRAM stacks. MP3/AAC
startup, repeated source changes, largest free internal block and minimum free
internal heap still need board verification. No new device connection or flash
is required for the static analysis.
