# KNX_IP Xiaozhi_MCP and License_Mgnt maintenance review

This review covers the three usermods on `UserModsDev`, including KNX transport and conversion helpers, MCP transport and trust-store code, and the license validator called in the WLED core. The cleanup improves readability and removes unused implementation code. Existing protocol, configuration, diagnostic, LED, and license behavior is preserved. The findings below describe existing defects or risks requiring separate behavior changes; this cleanup is not a security sign-off.

The starting checkout already contained staged changes in five files. Those staged entries remain intact. The additional maintenance changes are unstaged. No commit, push, PR, issue, or reviewer reply was created. AI assistance is disclosed in the source refactoring blocks.

## Architecture and compatibility boundaries

| Usermod | Responsibilities | Behavior to preserve |
| --- | --- | --- |
| KNX_IP | KNXnet/IP discovery and multicast routing; per-address callbacks; global and segment LED controls; relative adjustments; time updates; temperature feedback; settings and GA table | Address parsing and zero-address sentinel, registration order, wire payloads, rounding, LED capability decisions, main-segment versus segment-zero choices, publish timing, repeat/dedup settings, JSON keys, labels and diagnostics |
| Xiaozhi_MCP | Six MCP tools registered after connection; JSON-RPC dispatch on WLED's main loop; queue-based networking worker; certificate validation and reconnect handling | Tool names/schemas/replies, setup-time alias capture, callback order, queue/session ownership, 15 KiB transport limit, existing smaller JSON document capacities, TLS policy, retry/heartbeat intervals, power and brightness effects |
| License_Mgnt | Display device identity and key status; periodically validate `/DEVICE_KEY`; count unlicensed intervals; warn and reboot | Read-only configuration values, key file bytes and status codes, strict 61-second polling boundary, cumulative invalid-interval counter, warning effect and next-check reboot, pending-reboot precedence over revalidation |

KNX validates incoming frame lengths and most handler payload lengths before dispatch. MCP isolates network operations from LED mutations through queues and session tags, and retires a worker before starting its replacement. These are useful boundaries to retain during later refactoring. License validation is a core dependency: a usermod-only fix cannot fully address its allocation and logging issues.

## Cleanup plan and implementation

1. **Establish the baseline.** Read `AGENTS.md`, contributor/build/C++/web/security guidance, inspect staged changes, trace configuration, ingress, state mutation, persistence and shutdown paths, and compile the existing combined firmware.
2. **Normalize formatting.** Apply two-space indentation, attached braces, consistent spacing and readable statement layout across the 13 C++ source/header files. Preserve include order, preprocessor conditions and string contents. Check token equality before any further edits.
3. **Remove confirmed unused implementation.** Delete private KNX clock/white helpers with no callers, unused KNX fields and a write-only preset tracker; remove MCP's unused private escaping helper and unused local brightness calculations; remove License_Mgnt's empty MQTT polling stub. Preserve public compatibility helpers and optional test entry points.
4. **Clarify contracts.** Add `override` to existing KNX and Xiaozhi usermod hooks, make immutable timeout constants `static constexpr`, translate KNX discovery comments into English, and correct stale comments about transport queue acceptance, temperature providers and existing registration behavior. Preserve existing per-module logging gates and explain why redirecting them to `WLED_DEBUG` would change output.
5. **Verify and review.** Compare active function bodies with the starting source, review all non-formatting changes, run the Node tests, rebuild the web assets, and compile `esp32dev` plus the existing combined release/debug environments. Keep device-level acceptance separate from compilation.

The staged parser/clamp, MCP effect-response and periodic-license helper extractions were retained. Source markers identify those refactoring blocks. No algorithms were imported from an external source; the cleanup is based on this repository's existing implementation. Existing source attribution and dependency references remain.

The cleanup deliberately avoids a shared KNX/MCP LED mutation helper: the modules currently differ in auto-power behavior, white-channel handling and notification calls. Unifying them without explicit policy would change behavior. Likewise, replacing the three license checks with a cached status could change when a newly imported key becomes visible.

## Existing findings requiring separate changes

All findings below are based on traced source paths. Device failures have not been reproduced on hardware. Source links refer to the post-cleanup files; line numbers are provided to make review concrete. Severity follows `docs/hardening.instructions.md` and `docs/securecode.instructions.md`.

### Critical credential handling

**C1 — A credential-bearing MCP endpoint is embedded in the default configuration.** `Xiaozhi_MCP.h` initializes `mcpEndpoint` with a complete token-bearing URL. Enabling the usermod without replacing that value uses the shared endpoint. Treat the credential as exposed; its current server-side validity was not tested. Rotate/revoke it, remove it from defaults, and require a provisioned endpoint. This changes first-use behavior and is outside the compatibility-only cleanup. The token is intentionally not copied into this report. Sources: [Xiaozhi_MCP.h:54](../usermods/Xiaozhi_MCP/Xiaozhi_MCP.h#L54).

**C2 — Debug output discloses endpoint credentials and license keys.** `Xiaozhi_MCP::checkMcpConfig()` prints the entire endpoint with `MCP_UM_DEBUG`; `License_Mgnt::setup()` prints the stored key with `LM_UM_DEBUG`. Core `validateDeviceKey()` also prints both computed and stored keys with `WLED_DEBUG`, including the expected key for an invalid imported file. The combined debug environment enables these paths. Remove sensitive values while retaining status-only diagnostics, including in the core validator. The transport's own logging precautions do not cover these usermod/core logs. Sources: [Xiaozhi_MCP.cpp:174](../usermods/Xiaozhi_MCP/Xiaozhi_MCP.cpp#L174), [License_Mgnt.cpp:15](../usermods/License_Mgnt/License_Mgnt.cpp#L15), [util.cpp:1617](../wled00/util.cpp#L1617).

### Important license robustness

**I1 — License reads allocate from an unchecked file size and ignore short reads.** `License_Mgnt::getStoredDeviceKey()` and core `validateDeviceKey()` allocate `new char[size + 1]`, use the result without an allocation check and ignore `readBytes()`'s return value. A large file can exhaust heap; a short read leaves bytes before the final terminator uninitialized. The file is locally provisioned, so this is a constrained-memory/corruption robustness issue rather than an unauthenticated network exploit. Centralize bounded reads in the core, enforce the actual key format's maximum size, check allocation and read length, and return a defined error without applying partial data. Preserve exact-byte comparison unless a separate migration explicitly permits whitespace. Sources: [License_Mgnt.cpp:179](../usermods/License_Mgnt/License_Mgnt.cpp#L179), [util.cpp:1591](../wled00/util.cpp#L1591).

### Important KNX correctness and resource handling

**I2 — HSV application uses values outside its converter's normalized domain.** `applyHSV()` calls `hsvToRgb(hDeg, 101 - s01, 101 - v01, ...)`, while callers construct saturation/value in `0..1`. The converter uses those values directly. For example, hue 0 with saturation 0 and value 0 produces saturated red after clamping rather than black. Pass normalized values after agreeing the intended mapping and add tests through the actual command path. Correcting this is a visible color change, so the expression remains unchanged here. Sources: [usermod_knx_ip.cpp:1450](../usermods/KNX_IP/usermod_knx_ip.cpp#L1450).

**I3 — Address changes can leave runtime registrations stale.** `readFromConfig()` loads segment offsets and time/temperature addresses, but `anyGAChanged` compares only a subset of the GA settings and excludes segment offsets. If KNX is running and the compared fields are unchanged, `rebuildNeeded` is false. Changing only an excluded setting then takes the lightweight socket path, leaving callbacks or cached temperature GAs on the old mapping. Build one explicit registration snapshot that includes every registration-affecting setting; compare it before rebuilding. Include segment-count changes in the lifecycle design rather than assuming every change is a light-capability change. Sources: [usermod_knx_ip.cpp:2950](../usermods/KNX_IP/usermod_knx_ip.cpp#L2950), [usermod_knx_ip.cpp:3177](../usermods/KNX_IP/usermod_knx_ip.cpp#L3177), [usermod_knx_ip.cpp:3271](../usermods/KNX_IP/usermod_knx_ip.cpp#L3271).

**I4 — Conflict notification can count registered segment addresses as collisions with themselves.** `getAllUsedGAs()` includes the current segment arrays. `hasGAConflicts()` compares the proposed segment addresses against that combined list and exempts only addresses matching the central set. After successful multi-segment registration, `checkGAConflictsAndNotifyGUI()` can therefore report each segment beyond zero as conflicting with its own existing address. Compare prospective ownership records instead of bare GA values; allow only an address's own owner and the intentional segment-zero alias. Test the same valid plan both before and after registration. Sources: [usermod_knx_ip.cpp:213](../usermods/KNX_IP/usermod_knx_ip.cpp#L213), [usermod_knx_ip.cpp:350](../usermods/KNX_IP/usermod_knx_ip.cpp#L350).

**I5 — Conflict errors use the wrong core error number.** Three assignment sites and the clear condition use literal `33`, although `ERR_KNX_GA_CONFLICT` is `89` and the web UI handles KNX details under `case 89`. Use the named constant consistently when making the behavior fix, and verify the Info/UI message and clearing path. This cleanup preserves `33` rather than silently changing displayed error behavior. Sources: [usermod_knx_ip.cpp:454](../usermods/KNX_IP/usermod_knx_ip.cpp#L454), [const.h:504](../wled00/const.h#L504), [index.js:1598](../wled00/data/index.js#L1598).

**I6 — RX enhancement tracks arbitrary destinations before checking registration.** `_handleIncoming()` inserts `_rxBitCache[dst]` for any one-byte write when enhancement is enabled, before `_gos.find(dst)`. Distinct unregistered destination addresses can grow a map with no eviction up to the address-space limit, which is excessive for an ESP32. That same branch applies the one-bit toggle throttle to every one-byte DPT, so successive brightness/channel values can be dropped merely because their low bit changes. Check registration and the DPT before tracking; bound or expire state. Keep this a separate change because accepted traffic changes. Sources: [esp-knx-ip.cpp:668](../usermods/KNX_IP/esp-knx-ip.cpp#L668), [esp-knx-ip.cpp:679](../usermods/KNX_IP/esp-knx-ip.cpp#L679).

**I7 — Relative increase STOP currently performs a positive step.** `DPT.h` names `0x08` as `DPT_3_007_INCREASE_STOP`, but `knx_step_delta()` only returns early for the complete nibble `0x00`. With `0x08`, step percentage is zero and the minimum-magnitude rule changes it to `+1`. Test both STOP encodings and gate on the step bits before applying the minimum step. The current mapping is documented and preserved in this cleanup. Sources: [DPT.h:28](../usermods/KNX_IP/DPT.h#L28), [usermod_knx_ip.cpp:1027](../usermods/KNX_IP/usermod_knx_ip.cpp#L1027).

**I8 — KNX publish deadlines are not rollover-safe.** `_nextTxAt = now + txRateLimitMs` is tested with `millis() >= _nextTxAt`. A deadline wrapping past the 32-bit clock boundary can appear immediately due; a computed zero also collides with the unscheduled sentinel. Use an elapsed-time representation or a wrap-safe deadline plus explicit pending state, with boundary tests. This would change scheduling around rollover and remains deferred. Sources: [usermod_knx_ip.cpp:2672](../usermods/KNX_IP/usermod_knx_ip.cpp#L2672), [usermod_knx_ip.cpp:2834](../usermods/KNX_IP/usermod_knx_ip.cpp#L2834).

**I9 — Segment allocation failure has no defined recovery path.** `registerSegmentKOs()` deletes the old six arrays, allocates replacements with unchecked `new[]`, then writes through each pointer. Under heap pressure, allocation failure can abort or dereference null depending on the toolchain. Allocate a complete replacement first with checked WLED allocators/ownership, retain the previous working registration on failure, and publish a suitable error. This needs lifecycle design and fault-injection testing rather than a formatting-only substitution. Sources: [usermod_knx_ip.cpp:806](../usermods/KNX_IP/usermod_knx_ip.cpp#L806).

### Important MCP interoperability

**I10 — MCP JSON serialization loses IDs and fails to escape values.** `handleJsonRpcMessage()` inserts string-converted IDs into hand-built JSON without JSON quoting, and converts tool-call IDs to `int`. It concatenates tool descriptions containing the configurable alias without escaping. `formatJsonString()` also quotes string values without escaping their contents. String request IDs and aliases containing quotes/backslashes can therefore produce mismatched IDs or invalid JSON. Use typed JSON serialization while retaining the original ID type, then test string/numeric IDs and escaped aliases. Merely wiring up the deleted unused escaping helper would not solve all these cases. The existing README already documents part of this limitation. Sources: [WebSocketMCP.cpp:729](../usermods/Xiaozhi_MCP/WebSocketMCP.cpp#L729), [WebSocketMCP.cpp:768](../usermods/Xiaozhi_MCP/WebSocketMCP.cpp#L768), [WebSocketMCP.cpp:780](../usermods/Xiaozhi_MCP/WebSocketMCP.cpp#L780), [WebSocketMCP.cpp:937](../usermods/Xiaozhi_MCP/WebSocketMCP.cpp#L937).

**I11 — MCP reconfiguration is stored without restarting the active client.** `readFromConfig()` updates the endpoint and alias, but `loop()` only calls `setup()` when `isSetupDone` is false. A connected instance therefore keeps its old endpoint and setup-time alias until it is restarted or disabled and re-enabled. If live configuration is intended, compare active versus requested settings and perform replacement on the main loop through the existing retirement mechanism; do not call transport lifecycle APIs directly from the settings callback. Tool re-registration must also update metadata, because `registerTool()` currently replaces only callbacks for an existing name. Sources: [Xiaozhi_MCP.cpp:151](../usermods/Xiaozhi_MCP/Xiaozhi_MCP.cpp#L151), [Xiaozhi_MCP.cpp:98](../usermods/Xiaozhi_MCP/Xiaozhi_MCP.cpp#L98), [WebSocketMCP.cpp:842](../usermods/Xiaozhi_MCP/WebSocketMCP.cpp#L842).

## Further review boundaries

KNX's transmit API uses fixed 64-byte cEMI storage without a general payload-size guard, but the traced usermod send paths use small fixed payloads. Treat broader public API bounds as defense in depth, not a demonstrated remote overflow. RX parsing ignores nonzero cEMI additional-info length; a protocol change should add packet fixtures with that field populated. Current device/date parsers also need protocol-specific fixtures before changing their mappings.

The transport trust-store and worker ownership code received source review and formatting only. This does not certify TLS behavior, worker stack margin, heap stability or interoperability under hardware/network faults. No blanket requirement for TLS or authentication was applied to KNX multicast, consistent with WLED's documented threat model.

The existing `test/knx_pure_test.cpp` copies algorithms instead of compiling the production implementation; its relative-step calculation truncates while production rounds. Those tests cannot establish equivalence for the real command path. A future behavioral refactor should move pure production helpers behind a testable interface, then test them directly without compiling the entire firmware or maintaining parallel implementations.

## Validation

| Check | Result |
| --- | --- |
| Locked Node dependency installation and web build | Passed |
| `npm test` | 16 tests passed; an initial run failed the existing 850 ms file-age assertion, and the separate rerun passed without changing tests |
| `pio run -e esp32dev` | Passed |
| Combined release build | Passed; all three requested usermods confirmed linked |
| Combined debug build | Passed; all three requested usermods confirmed linked |
| Formatting verification | All 13 source/header files retained identical non-comment tokens and literal contents during the formatting-only pass |
| Active code comparison | 132 active function bodies retain identical tokens; five unused functions removed and seven localized body edits reviewed separately |
| Protocol/worker comparison | Complete KNX transport, DPT conversion/declarations, MCP networking worker and trust store retain identical non-comment tokens |
| Staged work preservation | Git index matches the starting index byte-for-byte |
| Whitespace validation | `git diff --check` passed |

The seven localized body edits are removal of an empty MQTT branch, removal of two unused local brightness calculations, removal of a write-only preset assignment, simplification of two identical-arm conditional expressions, and removal of redundant parentheses in MCP configuration checks. The original string-equality and length checks are both retained: Arduino strings can have an embedded null, so replacing the pair with only a length check would not preserve every input case.

The combined release build uses 88,256 bytes of static RAM and 1,524,791 bytes of flash, versus the starting build's 88,280 and 1,524,835 bytes. These are linker totals, not measurements of runtime heap or stack usage. The combined debug build uses 1,569,379 of 1,572,864 flash bytes: only 3,485 bytes remain. Existing `esp_dmx` string-bound warnings remain outside this cleanup. The KNX debug flag/logging macro redefinition is made explicit with `#undef` before the same function-like definition; this preserves logging behavior.

Build configuration and dependency versions were not changed. The successful `esp32dev` build alone does not exercise these usermods; the two combined environments provide that compilation coverage. ESP8266 is explicitly unsupported by the three modules. No firmware was flashed, and no hardware KNX/MCP/license smoke test was run.

Local verification scripts, source snapshots and build logs are under the ignored `.cache/usermod-cleanup/` directory. The repository's Node tests temporarily change web source files and restore them; their restored content was checked before delivery.

## Device acceptance before release

Use a spare board and the existing settings before deploying the cleanup. Check KNX discovery, global/segment power and brightness, absolute/relative color/CCT/effect/preset commands, duplicate handling, network reconnect and time updates. Compare GA feedback and Info output with the baseline firmware. Exercise a valid multi-segment address plan to distinguish the pre-existing conflict issue from a regression.

For MCP, run the existing `tools/xiaozhi-mcp-smoke.js` against the board for initialization, listing, read-only calls, fragmentation and reconnect cycles; also check the provisioned secure endpoint, certificate/time failures, disable/re-enable and memory under LED load. Color/effect tools must leave an off strip off.

For licensing, check missing, invalid and valid key files; verify status updates at the existing polling boundary and the warning/reboot sequence. Importing a valid key after the warning must retain the current pending-reboot behavior in a compatibility run. Test malformed/oversized files only as a separate fault-injection exercise for the future robustness fix.

_You are interacting with an AI system_
