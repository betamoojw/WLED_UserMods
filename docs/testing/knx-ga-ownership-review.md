# KNX GA Ownership Review

Review date: 2026-11-17. Local baseline: `UserModsDev`,
`3dd35f4ee9c684f27316a33fd51253d66852a74c`.
This review examines the local production source; it does not independently
certify the contents or merge date of remote PR #130.
AI-assisted changes; not committed, pushed, uploaded, or tested on a KNX network.

## Findings and Changes

| Path | Before | Patch |
| --- | --- | --- |
| Conflict detection | Registered segment caches were mixed with central GAs; later validation could collide with itself. | Validate a fresh target plan with `(GA, object index, segment index)` ownership. Historical registrations do not participate. |
| Segment zero | Broad exemptions and a single-segment early return hid distinct-object duplicates. | Represent segment zero as the corresponding central object, once. Other objects sharing the GA remain conflicts. |
| Central coverage | CCT, relative controls, time/date, temperature and other fields were omitted. | One complete central configuration list feeds inventory and validation. |
| Registration lifecycle | Clearing arrays left objects/callbacks registered; offset-only changes and segment count changes did not reliably rebuild. | Remove segment-owned objects and callbacks, rebuild on segment count changes, and compare the complete parsed central address list and offsets directly. |
| Restart recovery | A permanent startup-success flag could suppress startup after an offline rebuild. | Retry when transport is stopped and network/IP are ready; rate-limit failed attempts to once per second. |
| Diagnostic consistency | Debug analysis used the old cache-based heuristic and skipped single-segment duplicates. | Reuse the production ownership detector and report its conflict details. |
| Registration ordering | Central registrations could overwrite each other before segment validation. | Validate before registering any central objects; stop transport and leave the registry empty on a conflicting complete plan. |
| Error identity | Conflict code `33` overlapped `ERR_LOW_MEM`; clearing it could clear an unrelated error. | Use `ERR_KNX_GA_CONFLICT` (`89`), clear only that error, and preserve unrelated error details. |
| UI lifetime | Core JSON serialization consumes global errors; an unresolved conflict could disappear on the next state update. | Keep module-owned conflict state/details and re-emit through `addToJsonState`; do not override another serialized error. Clear on successful validation or disable. |

The address formula remains component-wise:
`(main + L * segment, middle + M * segment, sub + N * segment)`.
There is no carry between components. Invalid strings, `0/0/0`, and component
overflow return zero and are not registered. Limits remain 31/7/255 and 32
segments. An overflowing segment address does not invalidate other valid objects.

All valid, nonempty configured central addresses are reserved, including fields
whose color capability is currently disabled. Distinct input/output objects
sharing a GA are rejected rather than silently overwriting registry entries.
This is deliberately stricter than the old incomplete detector. Clear unused
fields or assign distinct addresses. Segment zero retains its existing central
callback semantics; this patch does not change it into an independent segment
control callback.

## Regression Evidence

The initial 26-assertion production-code host test failed five assertions before
the production edits: self-conflict, repeated validation, KNX error identity,
warning recovery, and single-segment distinct-object duplication.
The same initial assertions passed after the first production fix.

Follow-up review reproduced a structural collision in the existing UI hash:
time/date `1/7/1`, `11/7/2` and `1/7/11`, `1/7/2` both hash to `1510812505`
in the test configuration. The new rebuild regression failed before replacing
the hash-based decision and passed afterward. The UI hash itself is unchanged;
registration correctness no longer depends on it.

Final checks:

| Check | Result |
| --- | --- |
| `node --test tools/knx-ga-test.js` | 60 C++ assertions, zero failures; UI branch test passed; zero skips. |
| `npm test` | 18 tests passed, zero failures/skips, including all 60 C++ GA assertions. |
| `python tools/test_knx_control.py` | Production control: 760 assertions; production integration: 55 assertions; zero failures. |
| `npm run build` | Passed. |
| `pio run -e esp32dev` | Passed using the managed Python 3.11 PlatformIO entry point; flash 1,333,347 / 1,572,864 bytes. |
| `pio run -e custom_esp32dev_knx_ip_xiaozhi_mcp` | Passed; linkage includes KNX_IP; flash 1,519,015 / 1,572,864 bytes (96.6%). |
| `git diff --check` | Passed. |

Coverage includes initial and repeated registration, repeated validation, legal
segment-zero aliases, central/segment and cross-segment conflicts, distinct
object duplicates, adding/removing segments, stale callback deletion, offset-only
reloads, time-only reloads, invalid configuration addresses, overflow, unrelated
errors, persistent JSON warnings, recovery, disabling, and UI error 89/details
display and subsequent Info-state clearing. Follow-up assertions cover equal-hash
address changes, diagnostic output for self-addresses and single-segment
duplicates, offline segment rebuilds, connected-without-IP waiting, network
recovery, no redundant start while running, failed-start rate limiting and later
successful startup. Production extraction now fails explicitly if required
source markers disappear or are out of order.

## Reproduction and Boundaries

Set `CXX` to a host C++17 compiler; `zig c++` is supported. Run
`node --test tools/knx-ga-test.js`. Without a compiler, the C++ test explicitly
skips, so inspect skip counts. This run used Zig from the temporary npm cache,
without changing project dependencies. The runner compiles extracted production
functions with the existing Arduino/WLED boundary stubs and bundled ArduinoJson;
it does not copy the conflict algorithm. Source CRLF is normalized for extraction.
The original UI error branch executes in a Node VM with UI rendering boundaries
stubbed, not a browser or physical WLED device.

GA parsing, calculation, ownership detection, diagnostic formatting, validation,
segment registration, JSON warning emission, config loading, startup retry policy
and the loop's segment-change branch are production code. Full `setup()`, network
readiness, socket startup, LED hardware and callback actions are replaced at the
host boundary. The ESP32 Wi-Fi stabilization branch is compiled by the firmware
build, not exercised by the host tests. The host checks do not validate actual multicast
membership, callback payload effects, or full device startup ordering; firmware
compilation checks the complete device implementation. Legacy manually invoked
diagnostic/self-test routines are not an automated hardware test suite.

Both firmware builds used
`C:/Users/moj1/.platformio/python3/python.exe -m platformio` after the system
Python 3.14 entry point failed during platform virtual-environment setup.
The follow-up combined build reported an `esp32dev` failure without a retained
cause; a separate rerun reported SUCCESS and native process exit code zero.
Builds emitted existing dependency warnings. The KNX-enabled target has little
flash headroom. `npm ci` reported two moderate dependency audit findings; no
dependency upgrades were made. VS Code retained a temporary generated-test-include
indexing diagnostic even though explicit host compilation passed.

## ETS and Device Checks Still Required

- Confirm central and segment-zero behavior using ETS Group Monitor, including
  switch, brightness and effect command/status DPTs.
- After adding/removing segments or changing offsets, verify old GAs no longer
  invoke callbacks and new GAs invoke exactly the intended segment once.
- Reload valid and conflicting configurations, including reboot and temporarily
  disconnected Wi-Fi; verify registrations and multicast recovery on the router.
- Verify repeated UI polling and reconnection preserve unresolved warnings,
  another error takes priority, and recovery/disable clears the KNX warning.
- Check the stricter configured-address reservation policy against the intended
  ETS project, especially previously shared command/status GAs and inactive color
  objects.
- Exercise the maximum practical segment count, invalid addresses and repeated
  rebuilds while measuring free heap, fragmentation, effect responsiveness and
  transport restart behavior. Host assertions and compilation do not establish
  long-duration hardware reliability or reproduce a field failure.

## Patch Inspection

Use the VS Code working-tree diff or `git diff -- usermods/KNX_IP test/knx_control`.
Also inspect the new untracked `test/knx_control/ga_test.cpp` and
`tools/knx-ga-test.js`; plain `git diff` does not include untracked files.
No global build configuration, WLED core error definitions, frontend source,
generated headers, or dependency lockfiles were changed.