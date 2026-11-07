# Usermod credential and license review

Scope: `UserModsDev`, based on `c1e4a89c9cd851b1122530af01be08ad9ba5f1bc`.
Reviewed `KNX_IP`, `Xiaozhi_MCP`, `License_Mgnt`, their core license helper,
and the HTTP paths that expose their configuration. No real device, endpoint,
token, or license was contacted or used. Examples and regression data are synthetic.

## Findings and patch

| Severity | Source and previous behavior | Change |
| --- | --- | --- |
| High | `usermods/Xiaozhi_MCP/Xiaozhi_MCP.h`: shipped a credential-bearing endpoint default | Empty default; an enabled but unconfigured instance does not start transport |
| High | `Xiaozhi_MCP::checkMcpConfig`: printed the complete endpoint in module debug mode | Retain only the existing configuration-status message |
| High | `License_Mgnt::setup` and `validateDeviceKey`: logged stored and derived keys | Delete the diagnostic-only second file read; emit status messages without either value |
| Medium | `WebSocketMCP` RPC/tool diagnostics: echoed peer-controlled IDs and names | Operation/result messages only; wire replies and tool dispatch remain unchanged |
| High | `serveJson` and `handleFileRead`: config reads bypassed the settings PIN gate | Apply the existing config-write PIN policy to `/json/cfg` and static `cfg.json`/`cfg.json.gz` downloads |
| High | `handleFileRead`: `/DEVICE_KEY` was a downloadable static asset | Reject license-file paths, including compressed copies, before touching the filesystem |
| High | `handleBootLoop` calls `dumpFilesToSerial`: recovery dumped configuration and backup contents even without debug flags | Exclude config, bkp/rst config copies, security and license files; retain ordinary JSON diagnostics |
| Medium | `validateDeviceKey`: allocation sized from the file, unchecked read count | Read at most 43 bytes into a 44-byte stack buffer; reject incomplete reads |

The public HTTP exposure is traced in source, not demonstrated against a real device:
`initServer`'s `onNotFound` handler calls `handleStaticContent`, which calls
`handleFileRead`; the `/json` GET handler calls `serveJson`. The native suite calls
the actual two final handlers with simulated request/response sinks.

There are no production file splits or alternate production/test implementations.
The native runner extracts complete RPC, `validateDeviceKey`, `serveJson`,
`handleFileRead`, `dumpFilesToSerial` and `getJsonValue` definitions from their current source files into
temporary headers, failing if the extraction boundaries change. The two complete
usermod implementations are included directly. This executes real RPC code without
starting sockets, TLS or tasks. ArduinoJson is the bundled production library.
Arduino strings, filesystem,
clock, reset, LEDs, HTTP responses and transport I/O are host fakes.

## Compatibility and intentional changes

- MCP config object and field names, endpoint storage, alias handling and enabled
  state retain their existing format. An already saved endpoint is preserved,
  including when a partial update omits it. There is no automatic credential rotation.
- Newly configured devices must supply their own endpoint. Devices that previously
  depended on the embedded default without saving it must configure one explicitly.
- Device ID and key derivation functions are byte-for-byte unchanged against the
  baseline. Missing/open-error/invalid/valid status codes remain `-1/-2/-3/0`.
  Exact 42-character keys remain valid; case changes, newline suffixes and non-NUL
  suffixes remain invalid. Legacy acceptance of a NUL immediately after a valid key
  is deliberately retained. Incomplete filesystem reads now explicitly fail closed.
- License polling, trial countdown, warning effect, pending reboot order and status
  text are unchanged. Importing a valid key after a reboot was already scheduled
  does not cancel that pending reboot; this existing behavior is not redesigned here.
- KNX source, group objects, ETS parameters, packet encoding and WLED state
  publication are unchanged. KNX does not itself validate a license; the license
  usermod's reboot policy affects the whole device.
- With a configured and locked settings PIN, config reads return HTTP 401 with the
  existing `ERR_DENIED` code. Existing tools must unlock settings before reading.
  After unlock they receive the original format. State/info APIs and ordinary static
  assets retain their previous access policy. License files always remain unavailable
  through the public static-file path; the existing PIN-protected editor/import path
  is unchanged. Raw license-download integrations must use that administrative path.
- Static paths containing `cfg.json` use the PIN gate, including backup and gzip
  names. Matching before a suffix prevents a decoded NUL from bypassing the gate
  when a downstream filesystem API treats the path as a C string. This is deliberately
  conservative for other filenames that contain `cfg.json`.
- No PIN still means public config access, as in WLED's existing configuration-write
  policy. WLED's `correctPIN` is a **device-wide unlock state**, not per-client
  authorization. An unlock can therefore allow other LAN clients to read configuration.
  This patch does not claim per-client isolation or encrypted credential storage.

## Remaining security boundaries

The MCP endpoint is still stored in ordinary configuration, and the license remains
in `/DEVICE_KEY`, to preserve the requested on-device formats. Protect backups,
filesystem images and physical access; do not publish config exports. Configure a
settings PIN and isolate the management network. A future protected usermod secret
store would require a shared core interface and migration design, rather than
masking `addToConfig` (which would also destroy persisted values).

MCP transport still accepts both `ws` and `wss` for compatibility. Its WSS trust
store, certificate verification and synchronized-time requirements are unchanged.
A credential-bearing `ws` connection remains plaintext on the network; use a
provider-supported `wss` endpoint for those credentials. Provider/proxy URL logging
is outside the firmware's logging boundary.

The legacy license algorithm derives a deterministic digest locally using an
embedded constant. That constant is part of the existing public algorithm, not a
private signing key. Leaving it unchanged is necessary for legacy compatibility.
Removing logs and restricting downloads does not make the license scheme resistant
to forgery or provide revocation. Stronger licensing requires a versioned signed
license format, an issuer-held private key, firmware verification with a public key,
and an explicit migration/revocation policy. Such a redesign is outside this patch.

KNX diagnostics can broadcast operational data on UDP port 5140 when
`KNX_UDP_DEBUG` is enabled; telegram payload diagnostics also expose bus data.
Source review found no path passing MCP credentials or license key material into
these KNX diagnostics. Protocol-defined multicast authentication is unchanged.
Treat debug captures as private operational data even after this patch.

## Rotation required from the operator

1. Treat the former shared endpoint credential as disclosed. Revoke it using the
   provider's administrative interface, issue per-device replacements and update
   each device's saved endpoint. Coordinate the cutover because revocation breaks
   old connections. Restart/reconnect and verify the new configuration manually.
2. Review previous firmware, logs, config backups, screenshots and repository history
   for exposure. Restrict or remove copies where possible. Deleting the source
   initializer does not revoke a token or erase history. Do not paste credentials
   into a bug report, test command, screenshot or validation log.
   Share the redacted review artifact instead of a raw Git diff: the deleted side
   of the original initializer still contains the historical credential.
3. Previously logged license material should also be treated as disclosed, but
   generating the same deterministic key is **not rotation**. Coordinate any true
   license replacement/revocation with a versioned verifier migration. This patch
   intentionally does not invalidate installed licenses or contact an issuer.

## Automated verification

Run `python tools/test_usermod_security.py` with a C++17 compiler (`CXX` may be
`c++` or `zig c++`). The suite runs normal, full debug, and module-only debug modes.
It captures logs in memory and fails using labels only; neither inputs nor log
contents are printed. Only synthetic `.invalid` endpoints and non-hex test keys
are used. The transport boundary cannot open sockets.

Coverage includes empty/default config, existing config round trips and partial
updates, setup transport arguments, public status output, RPC success/error/invalid
JSON, peer-supplied IDs/names/arguments, license setup and periodic checks, all status
codes, boundary lengths/NUL/newline/short-read/huge-file cases, PIN locked/unlocked/no
PIN behavior, gzip/repeated-slash/dot-path/decoded-NUL file variants and unaffected public APIs.
Recovery-dump coverage uses both basename and slash-prefixed names, config backups,
license/security files and a non-sensitive preset to verify retained diagnostics.
The pre-existing KNX suite additionally executes real command/state/publish paths.

Local validation on 2026-11-07 (no device flashing):

- Security production-path checks: normal 143, debug 145, module-only debug 143; all passed.
- Existing KNX checks: 760 control and 55 integration checks passed; 16,777,216 RGB round trips completed.
- `npm ci`, forced web UI generation and all 16 Node tests passed.
- Known former endpoint/token byte sequences were absent from all final BIN/ELF artifacts and validation logs. Safe debug messages were present in the debug firmware; the removed sensitive format strings were absent.
- Source comparison confirmed unchanged device/key derivation, persisted MCP fields, license lifecycle/status mapping and non-log RPC behavior; KNX source is unchanged.

| Environment | Result | Flash used / limit | Flash | RAM used / limit |
| --- | --- | --- | --- | --- |
| `esp32dev` | Passed | 1,333,367 / 1,572,864 B | 84.8% | 87,128 / 327,680 B |
| `custom_esp32dev_knx_ip_xiaozhi_mcp` | Passed | 1,517,659 / 1,572,864 B | 96.5% | 88,168 / 327,680 B |
| `custom_esp32dev_knx_ip_xiaozhi_mcp_debug` | Passed | 1,562,143 / 1,572,864 B | 99.3% | 88,352 / 327,680 B |

The debug image remains close to the application partition limit; this is not a firmware-size reduction project. Existing third-party compiler warnings remain. `npm ci` also reported two moderate dependency advisories; dependency versions were not changed in this patch.

Validation logs, firmware hashes, the source snapshot and a redacted review diff are in `docs/temp/usermod-security-validation/` (ignored by Git). At the end of local validation, before publication approval, no commit, push or PR had been created.

## Device acceptance (operator-run)

1. On an isolated test device, retain a private backup. Record license **status only**
   and current KNX behavior, install each normal/debug firmware without erasing FS,
   and confirm the existing saved endpoint and license still work. Never send raw
   backups or serial logs containing credentials to a reviewer.
2. Configure a settings PIN, explicitly lock settings, and verify `/json/cfg`,
   `/cfg.json`, `/cfg.json.gz`, `/bkp.cfg.json`, and `/rst.cfg.json` are denied. Unlock through the normal settings
   flow and verify settings/export still work; relock and repeat. Use two clients to
   understand that unlock is device-wide. Confirm no-PIN config access is unchanged.
   With settings locked, also check encoded variants such as `/%63fg.json` and
   `/cfg.json%00suffix`; rejection is acceptable, but config contents must not return.
3. Verify `/DEVICE_KEY` and `/DEVICE_KEY.gz` return no license data both before and
   after unlock. Verify `/json/state`, `/json/info`, presets and normal control still
   work. If license import is needed, use the existing authorized editor/import flow.
4. Using a disposable test endpoint and test license material, exercise startup,
   reconnect, config validation, RPC ping/tool success/tool error, and license
   missing/invalid/valid states. Inspect normal and debug serial output locally;
   check complete credentials **and distinctive fragments** are absent. Do not use
   real credentials as adversarial RPC IDs/names/arguments.
5. Confirm valid devices retain their status after reboot. On a dedicated unlicensed
   test device, verify the existing countdown, warning and scheduled reboot behavior.
   Recheck KNX RGB/HSV/relative dimming and state feedback using the existing
   `knx-color-control.md` checklist. Physical LEDs, real TLS/MCP peers, HTTP transport
   decoding and UART/UDP captures require this device acceptance; host tests do not
   replace it.
