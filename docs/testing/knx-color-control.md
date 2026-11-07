# KNX color, dimming and state-publication regression review

## Scope and disposition

Baseline: `UserModsDev` commit `3e3114120480d0e52677d6ca87de41f1ee2b8513`.
Review date: 2026-11-07. The maintainer authorized committing and pushing this
revision after reporting successful manual firmware compilation.

The original HSV normalization, both-direction STOP and configured-white-channel
fixes are retained. This revision addresses the four follow-up findings in the
actual WLED update and KNX routing/publication paths:

| Finding | Correction and production source |
| --- | --- |
| Old legacy colors overwrite direct segment edits | `knx_color_control.cpp` uses `stateUpdated(mode, sourceSegment)` instead of `colorUpdated()` after editing a segment. |
| Composite feedback depends on enabled channel GAs | `knx_state.cpp` detects raw state changes independently of output-address configuration, then checks each GA at transmission. Individual H/S/V are included in channel modes. |
| Communication enhancement filters DPT3 as a switch | `esp-knx-ip-routing.cpp` applies the one-bit toggle throttle only to registered `DPT_1xx` objects. Payload deduplication is retained. |
| Main segment, segment 0 and first-selected segment disagree | Central color/CCT/effect commands and feedback use the main segment. WLED synchronizes legacy fields from the explicitly changed segment before notifying. Main-segment changes refresh the central feedback snapshot. |

The multi-segment correction concerns the source of **central** status objects
and WLED notifications. It does not implement a new publisher for every
`GA_SEG_OUT_*` object. Existing per-segment registration and addressing remain
unchanged; their handlers now pass their target index to the WLED update entry.
Per-segment object commissioning remains a separate hardware acceptance item.

## Production-path design

The code is separated into normal firmware translation units, which the native
fixtures also compile directly:

- `usermods/KNX_IP/knx_color_control.cpp`: color/white/effect command handlers,
  RGB/HSV conversion and existing color callback registration.
- `usermods/KNX_IP/knx_control_helpers.h`: live configured color and relative-step
  helpers. Both `0x00` and `0x08` stop when their three step bits are zero.
- `usermods/KNX_IP/knx_state.cpp`: actual state-change detection, pending flags,
  coalescing deadline service and outbound publication.
- `usermods/KNX_IP/esp-knx-ip-routing.cpp`: actual incoming frame parsing,
  enhancement filtering, callback dispatch, APCI/cEMI encoding and send path.
  Shared transport constants/debug helper are in `knx_ip_protocol.h`.
- `wled00/src/led_state.cpp`: WLED state synchronization, brightness transitions,
  interface-update scheduling and the legacy `colorUpdated()` implementation.

The WLED core overload is necessary because calling the old one-argument update
would reload values from the first selected segment before notifications. Fixing
`colPri` afterwards would be too late. The new overload accepts a segment index;
invalid indices fall back to the first selected segment. Existing one-argument
callers retain that original source and the original legacy `colorUpdated()`
behavior. No KNX-specific branch or test-only branch is added to WLED production
code. An unused local in the moved brightness routine was removed; the remaining
brightness/transition algorithm is unchanged.

The integration fixture includes these production files and real class headers.
It supplies fixed incoming KNXnet/IP packets, invokes the real receive path, and
captures complete outbound datagrams at `sendto()`. It does not copy the HSV,
step, update, scheduler, routing-filter or publication algorithms.

| Boundary | Executed production implementation | Remaining substitute or limitation |
| --- | --- | --- |
| Command and conversion | Real registration callbacks, converters and handlers | Startup/license/profile gating and settings loading are not executed. Some white/CCT checks call the handler directly. |
| WLED state | Real `stateUpdated()`, `colorUpdated()`, legacy synchronization and brightness logic | Segment storage/setters and renderer are fakes; notification/network consumers record calls. Actual pixel rendering and GUI serialization are not exercised. |
| KNX receive | Real cEMI/APCI parsing, DPT1 filter, deduplication and callback dispatch | Socket receive, router and Wi-Fi lifecycle are outside the fixture. |
| KNX publish | Real scheduler, deadline service, publisher, DPT conversion and packet encoding | Clock, sensor reads and socket send are fakes. Full device `loop()` is not run; it delegates to the tested deadline service. |
| Multiple segments | Main differs from first-selected/segment 0, CCT/white/effect source and runtime main switch | Per-segment KO setup/handlers in the main usermod file and physical segment output are not covered by this fixture. |

## Automated evidence

Run from the repository root with Python 3 and a C++17 compiler:

```sh
python tools/test_knx_control.py
python tools/test_knx_control.py --examples
```

The default command runs both suites. `--unit-only` and `--integration` run one
suite. `CXX` defaults to `c++`; a quoted compiler path or `path/to/zig.exe c++`
is also accepted. The runner enables warnings as errors and uses temporary
executables. Local validation used Zig 0.13.0, installed only in ignored
`docs/temp/`; no new firmware dependency was added.

Last successful observed results during this revision:

| Check | Result |
| --- | --- |
| Focused production-command suite | 760 assertions, 0 failures |
| Production WLED/KNX integration suite | 55 assertions, 0 failures |
| Exhaustive RGB conversion sweep | 16,777,216 RGB values; maximum channel error 0 through float HSV, 3 through quantized KNX HSV bytes |
| Existing web build tests (`npm test`) | 16 tests, 0 failures |

The final native rerun, after comment cleanup and restoring the original pending
deadline comparison, did not complete: Zig reported `Unexpected`; a retry with a
fresh cache also reported failure to write `builtin.zig` and failed its runtime
sub-compilations. No new assertion results were produced. The table records the
earlier successful run, not a successful final rerun. Re-run both native suites
locally before accepting the final patch. The web-test result is unaffected.

The initial integration suite had 28 failures in 37 assertions before the four
follow-up fixes, and then passed all 37. It was subsequently expanded to 55.
The earlier focused suite had 167 failures in 760 assertions before the original
HSV/STOP/configured-white fixes. These are native source-path reproductions,
not reported KNX hardware incidents. Old `knx_pure_test*.cpp` copies of algorithms
are not used as evidence.

Checks cover HSV zero/endpoints/hue sectors, partial S/V, white retention,
auto-enable, all 16 relative-step encodings, STOP on every relative callback,
old-color overwrite, output modes 0/1/2 with missing channel GAs, main-segment
selection, rapid DPT3 traffic with enhancement enabled, retained DPT1 throttling
and duplicate suppression, actual feedback payloads, first-command publication,
coalescing, short/truncated input rejection and idle versus queued STOP traffic.

A native-test job is added to `.github/workflows/build.yml`, using read-only
repository permission. No GitHub Actions result for this revision is recorded
in this review; consult the workflow run associated with the resulting commit.

The existing web test's fixed 850 ms file-age assertion falsely failed on this
machine: a force build took 9,258 ms, advanced the generated file timestamp, but
the file was already 1,330 ms old when checked. `tools/cdata-test.js` now compares
the generated file's timestamp before and after rebuilding. This preserves the
regeneration check without requiring a particular build speed; all 16 tests pass.

Local evidence files (ignored, not for a PR):
`docs/temp/knx-integration-before.log`,
`docs/temp/knx-state-native-tests.log` (latest failed native rerun),
`docs/temp/knx-state-test-results.json` (record of the successful session output),
`docs/temp/knx-web-tests.log`,
`docs/temp/knx-state-review-audit.json`, and
`docs/temp/knx-color-control-fix.patch`.

## Input/output examples

HSV values below are raw KNX bytes. Initial W=77, brightness=0 and automatic
on-brightness=128. These original-fix examples are from production-handler tests.

| Input | Original result: RGBW / brightness | Corrected result: RGBW / brightness |
| --- | --- | --- |
| HSV `[0,0,0]` | `[255,0,0,77]` / 128 | `[0,0,0,77]` / 0 |
| HSV `[0,0,255]` | `[255,0,0,77]` / 128 | `[255,255,255,77]` / 128 |
| HSV `[0,128,128]` | `[255,0,0,77]` / 128 | `[128,64,64,77]` / 128 |
| DPT3 `0x08`, brightness starts at 100 | 101, update scheduled | 100, no new update |
| DPT3 `0x00`, brightness starts at 100 | 100 | 100 |
| DPT3 `0x0F` / `0x07`, independently from 100 | 103 / 97 | 103 / 97 |

Follow-up integration cases:

| Setup/input | Before this revision | Corrected expectation checked in production path |
| --- | --- | --- |
| Selected main RGBW `[120,80,40,77]`; RGB relative `[8,15,7]` | Legacy write-back restores the old color | `[120,83,37,77]` remains after WLED update; RGB feedback contains `[120,83,37]` |
| Explicit RGBW `[1,2,3,4,0,0]` | Old selected color is restored | New RGBW persists and the six-byte output is `[1,2,3,4,0,0]` |
| Only composite GAs configured; green HSV `[85,255,255]` | No composite color telegram | RGB `[0,255,0]` and HSV `[85,255,255]` in modes 1/2 |
| Enhancement enabled; brightness 100; `0x0F`, then `0x0E` after 20 ms | DPT1-style toggle filter suppresses the second step | 103 then 111; STOP `0x08` leaves 111; `0x07` then yields 108 |
| Main=1, selected=0; main W=99; HSV `[170,255,128]` | Update/feedback may use segment 0 | Main `[0,0,128,99]`; segment 0 unchanged; matching legacy notification and RGBW feedback |
| Main=1; CCT=6500 K | Segment 0 is changed | Main CCT=255, segment 0 unchanged; CCT feedback bytes `[0x19,0x64]` |

## Compatibility and intentional behavior changes

Default group addresses, DPT identifiers, registered payload lengths, configuration
keys, ETS-facing object layouts, profile gating, and address offsets are unchanged.
Configuration serialization/loading and settings-UI function bodies were checked
against the baseline. No ETS mapping migration is introduced. The source comparison
also found no missing original functions across the moved modules.

Composite HSV retains the project's existing three-byte H/S/V convention labelled
DPT 232; this patch does not claim it is a standard KNX HSV datapoint. RGBW remains
six bytes `[R,G,B,W,0,0]`. Global power/brightness remain global. Brightness feedback
still rounds through integer percent: brightness 103 becomes 40%, raw DPT5 byte
102. Corrected color targets naturally change feedback values.

Changes that a commissioning engineer should observe:

- Direct segment edits survive WLED notification and no longer overwrite other
  selected segments through the legacy color-update route.
- Central RGBW, CCT and effect feedback follows the main segment, including a
  runtime main-segment change. WLED's legacy fields used for notifications follow
  the explicitly changed segment; other callers retain the original API behavior.
- Output mode 0 sends channels (including individual H/S/V); mode 1 sends only
  RGB/HSV/RGBW composites; mode 2 sends both. Mode 1 previously also sent individual
  H/S/V when configured. This correction can reduce those telegrams in mode 1;
  choose mode 2 if both forms are required.
- The first observed state is now queued instead of silently initializing the
  scheduler snapshot. Startup can therefore emit an initial feedback burst.
- DPT1 toggle throttling remains. Distinct rapid DPT3 commands are no longer
  classified as toggles; exact repeated payloads within the configured dedup
  window are still suppressed. This patch does not redefine that duplicate policy.
- STOP means no additional discrete step. It does not cancel a prior WLED
  transition or already queued feedback. After pending traffic drains, STOP alone
  produces neither a new WLED update nor a new color/brightness packet.

No license, MCP, partition, `platformio.ini`, or generated web-header change is
included. Actual UDP delivery, rendering, transitions on the target, reboot/settings
round-trip and deployed KNX interoperability still require device acceptance.

## Firmware compilation confirmation and reproduction

**Maintainer-reported result: manual firmware compilation succeeded.** The
maintainer confirmed this on 2026-11-07 and authorized committing and pushing
the reviewed patch. The latest `knx-validation-*` logs were not present in the
agent-visible workspace, so per-target results, current Flash/RAM usage and
firmware hashes have not been independently checked here.

The agent's earlier build attempt stopped before compilation while acquiring
PlatformIO's external `platforms.lock`, after automatic approval timed out twice.
The maintainer's subsequent manual-build confirmation supersedes the earlier
handoff status. It does not establish a new native-test result or hardware
acceptance. Do not substitute older binaries or earlier build logs for the
current revision's measurements.

Run in PowerShell from the repository root (stop if any step fails):

```powershell
# Optional: use the host compiler already installed locally for this review.
$env:CXX = 'E:/WLED_Projs/WLED_UserMods/docs/temp/knx-host-tools/ziglang/zig.exe c++'
$env:ZIG_GLOBAL_CACHE_DIR = Join-Path $PWD 'docs/temp/knx-host-tools/manual-cache'
python tools/test_knx_control.py
if ($LASTEXITCODE -ne 0) { throw 'KNX native tests failed' }
npm ci
if ($LASTEXITCODE -ne 0) { throw 'Dependency installation failed' }
npm run build
if ($LASTEXITCODE -ne 0) { throw 'Web build failed' }
npm test
if ($LASTEXITCODE -ne 0) { throw 'Web tests failed' }
$pioExe = 'C:/Users/Maxwell_Dev/.platformio/penv/Scripts/platformio.exe'
& $pioExe run -e esp32dev -e nodemcuv2 -e custom_esp32dev_knx_ip_xiaozhi_mcp -e custom_esp32dev_knx_ip_xiaozhi_mcp_debug
if ($LASTEXITCODE -ne 0) { throw 'Firmware compilation failed' }
Get-FileHash .pio/build/custom_esp32dev_knx_ip_xiaozhi_mcp_debug/firmware.bin -Algorithm SHA256
```

| Target | Why required | Per-target log verification |
| --- | --- | --- |
| `esp32dev` | Common WLED core build | Log unavailable |
| `nodemcuv2` | ESP8266 compatibility of the shared WLED core; does not include KNX | Log unavailable |
| `custom_esp32dev_knx_ip_xiaozhi_mcp` | Combined KNX, MCP and license release build | Log unavailable |
| `custom_esp32dev_knx_ip_xiaozhi_mcp_debug` | Combined debug build and Flash-capacity check | Log unavailable |

Verify all four environments succeed, the linker finds the new source modules,
and custom-target post-link checks retain all five usermods (including KNX_IP,
Xiaozhi_MCP and License_Mgnt). Record current Flash/RAM and firmware SHA-256.
The **previous** debug build used 1,562,219 / 1,572,864 application bytes (99.3%);
this is historical, not this revision's measured size. Confirm this revision fits
before choosing a candidate. No device flashing or GitHub operation is authorized
by the commands above.

## Real KNX acceptance checklist

All rows are **NOT RUN**. A host test or successful compile cannot mark them passed.
For each row save before/after `/json/state`, raw ETS Group Monitor traffic
(timestamp, direction, GA, APCI, payload), and a short physical-output observation.
Record PASS / FAIL / NOT APPLICABLE and the capture filename.

Device record: tester/date ___; board/LED type ___; candidate SHA-256 ___;
previous firmware ___; valid-license state ___; KNX router/interface ___;
ETS project/export ___; PA/input-output GA table ___; LED profile ___;
main/first-selected segment ___; transitions/effect ___; output mode ___;
TX interval ___; enhancement/resends/dedup ___; periodic setting ___; MCP ___ .
Do not collect license keys, tokens or passwords in the report.

Begin with saved configuration/presets, valid license, static effect, transitions
and periodic feedback disabled, main=first-selected=0, W=77, CCT=127, and
communication enhancement disabled. Retain existing ETS mappings. Repeat the
relevant rows with deployed settings afterwards.

| ID | Action | Acceptance criterion | Result |
| --- | --- | --- | --- |
| A01 | Save config/GA table; reboot and reload settings | PA/GAs/DPTs and configured feature availability unchanged; verify candidate hash | NOT RUN |
| A02 | Send HSV examples above, six hue sectors, H endpoint 255, individual S=0/V=0 | Correct target RGB in JSON/UI and LEDs; W=77 and CCT retained | NOT RUN |
| A03 | Auto-enable off/on; brightness 0/42; on-brightness 91/0 | Black HSV stays off with W retained; nonblack follows configuration; brightness 42 retained | NOT RUN |
| A04 | From brightness 100 independently send every DPT3 code; repeat STOP at 0/100/255 | `0x0F` -> 103; `0x07` -> 97; `0x00` and `0x08` unchanged; clipping correct | NOT RUN |
| A05 | Both STOPs on every relative object and all-STOP composites; physical rocker press/release | No added target step or auto-on; drain earlier traffic before expecting silence | NOT RUN |
| A06 | Selected main `[120,80,40,77]`; RGB relative `[8,15,7]`; RGBW relative `[8,0,8,15]` independently | `[120,83,37,77]` and `[120,80,40,80]` persist after WLED update; matching feedback | NOT RUN |
| A07 | Absolute RGBW/W, WW/CW relative and CCT commands on an appropriate CCT/RGBCCT profile | Intentional white/CCT changes persist; HSV retains independent W; unsupported profile objects remain gated | NOT RUN |
| A08 | Modes 0/1/2; empty all R/G/B/W/CCT/WW/CW output GAs and configure only composite or H/S/V outputs | Channels only / composites only / both as documented; correct GAs, byte lengths and target values | NOT RUN |
| A09 | First command after boot; two colors within 200 ms; idle STOP and STOP with feedback pending | First command reported; coalescing emits latest state; idle STOP adds nothing; prior feedback is not canceled | NOT RUN |
| A10 | Main=1, selected=0 with distinct colors/W/CCT/effects; then switch main at runtime | Central writes, legacy notification fields and feedback use same main; unrelated segment remains unchanged; also commission existing per-segment objects separately | NOT RUN |
| A11 | Enable enhancement; send `0x0F`, `0x0E`, `0x08`, `0x07`, `0x00` 20 ms apart from brightness 100 | 103,111,111,108,108; capture both STOP encodings; verify exact-repeat dedup and unchanged DPT1 throttle separately | NOT RUN |
| A12 | Restore transitions, real effects, periodic setting and real rocker; exercise MCP/license/reboot/preset | No unexplained target jumps; distinguish ongoing transitions from new steps; KNX/MCP/license features remain usable | NOT RUN |

A 200 ms setting is a scheduling delay, not a hard real-time network-delivery
guarantee. RGB black does not clear W, so preserved W can still emit light when
global brightness is nonzero. Do not use apparent camera color as the sole
conversion-precision measurement.
