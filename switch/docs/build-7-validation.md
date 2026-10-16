# Build #7 — full Switch Vulkan suite passed

Reviewed user-uploaded snapshot:
[`nxvk_run` at `88cca2e561f28539f82ae417f2d7321738b55efd`](https://github.com/berlint-hub/nxvk/tree/88cca2e561f28539f82ae417f2d7321738b55efd/nxvk_run),
uploaded 2026-10-16 at 16:32:21 UTC.

**22 PASS, 0 FAIL, 0 GONE, 0 SKIP.** This is the complete Vulkan ladder,
not just the two focused subtile diagnostics.

## Build attribution and consistency checks

- Embedded metadata identifies source `46972d49478d91b3f968d84922fc621ea4ca5c94`,
  clean tree, from [CI run #7](https://github.com/berlint-hub/nxvk/actions/runs/38929697655).
- Build ID: `e45588d034b5cf03c317adfff5cf279680bb29fee1a5674577436fdc9b3b3cd9`.
  SHA-256 of the uploaded `BUILDINFO.txt` equals that ID, and the summary agrees.
- Linked `libnvk.a` hash recorded in the metadata:
  `9f41ae6c2ab821ccd4b5a237cceeab3d31ef3e596eea1782a74be699aa3fe645`.
- All 71 downloaded files match their Git blob hashes. There are 49 primary
  files and 22 exact copies of app logs in `Add/`; these copies are not counted
  as another test run.
- The summary has exactly the 22 distinct Vulkan apps listed by the suite
  builder. Every app log has a passing verdict, no failing verdict, and the
  verdict text matches the summary.
- The reference `SHA256SUMS` contains the expected 23 NRO names (22 apps plus
  runner) and well-formed hashes. NRO binaries were not uploaded, so their
  on-console bytes have not been independently rehashed here.
- The 22 Mesa log files are empty; no driver diagnostics were captured there.
  `chain.log` contains 22 armed entries. Empty diagnostics are not a substitute
  for the per-app correctness checks.

## Selected observations

| Test | Recorded result |
| --- | --- |
| Smoke / rendering / textures | All basic rendering and readback verdicts pass |
| VI swapchain | 300/300 frames presented, three images at 1280×720 |
| Present modes | FIFO 60.0 and immediate 120.0 frames/s in the minimal present loop; acquire timeout checks pass |
| Descriptor table | 24,576 views created in 54,118 µs; submit mean 29 -> 29 µs after growth; first dirty submit 112 µs |
| Command flush | All four phases have correct readback, including forced CPU-cached full/ranged paths |
| Engine wait | Dependent submit 31 µs; fence wait 126,850 µs; all 1,024 tail words correct |
| Memory churn | 576/576 recycled mappings; C-heap arena remains 14,041,088 bytes and free chunks remain 35 over the measured run |
| Copy engine / host copy | Seven byte-exact copy-engine cases with guards intact; seven host-image-copy cases passed, none skipped |
| B2/fence visibility | All 8,388,608 words of the 32 MiB buffer match after the fence |
| Compression / sector / ZCULL | Both compression phases agree; 17 sector-test levels agree; 10/10 ZCULL scenarios agree |
| Push descriptors / subtiling | All descriptor draws/dispatches match; all nine subtiling configurations have identical colour/compute readbacks |

The descriptor-view creation time is a single observation, not a controlled A/B
measurement of the new fast path. Memory-churn timings vary within this run
(2,989 µs after warmup, then 2,224 / 2,520 / 2,371 µs at the logged checkpoints).
Do not turn differences from earlier, differently identified runs into a claimed
percentage speedup or slowdown.

The legacy subtiling probe in this full suite still uses its original phase
schedule; its absolute rows are not a replacement for the controlled diagnostic
reported separately in [the build #5 analysis](diagnostic-results-2026-10-16.md).
No new clock/governor or emulator work is implied by this review.

## What this establishes

The optimized build has now completed the full supplied regression suite on the
user's Switch without a reported functional failure. This is hardware regression
coverage for the scenarios above, in addition to build/link and native tests.

It does **not** establish Vulkan CTS conformance, correctness for every workload,
long-term stability, or a measured game-FPS improvement. In particular, these
logs do not count duplicate shader-cache puts or SD writes; the deduplication
work reduction remains directly covered by its separate native I/O fixture.
The original native operation-count tests and the hardware pass result must not
be conflated into an end-to-end performance claim.

No additional driver changes or rebuild were needed for this log review.
