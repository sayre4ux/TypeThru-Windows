# Windows port audit — 0.2.0

The subsequent 0.2.1 display fixes and macOS-inspired component refresh are
documented in [the UI design notes](UI-DESIGN.md). The core audit below describes
the 0.2.0 work; UI acceptance remains an owner task.

Scope: the C++ input engine, key mapping, cancellation and trigger flow, clipboard
boundary, saved settings, update trust chain, build/release scripts, installer
metadata, and native panel/Settings. Compared against the local macOS implementation.
This is a source review and automated regression pass, not a claim of verified
delivery into every remote desktop or a completed installation test.

## Findings addressed

| Area | Original failure | Change |
| --- | --- | --- |
| Build safety | `check.cmd` recursively deleted `.build`, including release artifacts and the private-key export location. It also failed when the app was running. | Isolated `.build/check` output; clean only build file extensions there; preserve `.build` contents. |
| Compiler gate | A dead placeholder emitted a `/W4` warning while the log claimed zero warnings. | Removed the placeholder and enabled `/WX`. |
| Keyboard mapping | `ToUnicodeEx` used flag value `2`, which does not preserve dead-key state. | Use bit 2 (`0x4`); US International dead-key regression. |
| Clipboard boundary | A clipboard allocation without a terminating UTF-16 NUL could be read past its end. | Parse within `GlobalSize`, reject malformed/oversized allocations (16 MiB maximum). |
| Injection failure | Surrogate pairs ignored injection results; a partial shifted sequence could leave an injected key held. | Unified checked event execution; best-effort reverse release of only successfully injected key-downs. No modifier-clearing chords. |
| Cancellation | Cancellation was checked only between characters, and Esc could be missed during startup delays. | Check throughout startup and event waits in 10 ms slices; keep cancellation sticky. Also watch side mouse buttons. |
| Target selection | The hotkey path could type into a different window selected during its delay. | Pin the trigger window; require stable foreground and matching keyboard layout before starting. Recheck focus before each event. |
| Lock/modifier state | Caps Lock could invert letter case; newly pressed Ctrl/Alt/Win could turn text into shortcuts. | Refuse physical typing with local Caps Lock enabled; stop on modifier or Caps Lock presses. Never toggle lock state automatically. |
| Unicode | Broken pairs and some newline forms had inconsistent preflight/execution; control codes could act as editing commands. | Validate pairs, reject non-text control codes, represent newline forms through the selected Enter/Shift+Enter rule. |
| Posted-window diagnostics | Sending mapped `WM_KEYDOWN` to the top-level window lost case and missed child edit controls. | Post text to the focused child; explicitly refuse Shift+Enter because posted messages cannot represent it reliably. |
| Preflight cost | Duplicate unsupported-character detection was quadratic. | Fixed 65,536-bit membership set for linear scanning. |
| Settings | Invalid VK/modifier values could survive registry loading. | Reject invalid keys, modifier-only shortcuts, and unknown modifier bits. |
| Update policy | The in-memory last-check value was never advanced, so the promised daily limit did not hold. | Persist every attempt, including failures; 24-hour automatic limit across restarts; one check at a time. |
| Update input | Permissive URL/JSON parsing, missing recursion bounds, and a dropped query string weakened validation and reliability. | Strict bounded parsing, same-repository/tag asset validation, query-preserving HTTPS requests and timeouts. |
| Update execution | Temporary-directory creation was unchecked; extraction used `cmd` and PATH; an old signed executable could run without a version check. | Exclusive random staging, direct system `tar.exe`, verify product/version before launch, require a newer matching version, atomic replacement with recovery backup. |
| Diagnostics | Window titles can contain document text. | Retain only window class, counts, and reasons. |
| Release integration | CI expected the old check path; installer output was outside release artifacts, allowed unsupported 32-bit machines, and offered elevation despite per-user design. A standalone installer build could use a stale fallback version. | Correct paths, constrain x64-compatible installation, keep per-user privileges, and read installer version from the executable resource. |

## Preserved behavior

- Default scan-code method and five speed values; existing key-hold and Shift margins.
- Shared symbol replacements, typing-test texts, and trailing-line-break behavior
  remain aligned with the macOS source. No changes to the macOS repository.
- Non-activating panel, floating T trigger, shortcut release wait, Esc/click/focus stop.
- No modifier recovery chords, driver, default elevation, telemetry, clipboard
  persistence, or private-key changes.
- Signed updates still use the existing embedded ECDSA P-256 public key. Product
  version checks supplement the archive signature; they do not replace it.

## Validation and remaining acceptance

The regression suite uses fake input transports and inspects `INPUT` structs. It
does not call the production typing entry point or post keyboard input. Coverage
includes every failure position in shifted/surrogate sequences, mid-hold stops,
malformed clipboard buffers, dead keys, URL/JSON attacks, and signature tampering.
The signature tampering test itself was corrected to mutate an in-bounds byte;
the old test requested a byte beyond its fixture.

Final validation: `scripts/check.cmd` passed with all targets built under
`/W4 /WX`; **173 checks, zero failures**. Safe `--version` and `--print-map` smoke
checks passed, executable resources report 0.2.0, and a preservation probe survived
the clean check. `.build/review/TypeThru.exe` is a byte-identical copy of the checked
application. See the [manual acceptance checklist](MANUAL-TESTS.md) for owner-only tests.
No installers, uninstallers, or real input tests were run during this audit.

Release remains conditional on that manual acceptance, especially the non-activating
panel/dropdowns, multi-monitor DPI, accessibility, real remote-session delivery,
and a signed upgrade from the previous version. A successful `SendInput` call
only proves that Windows accepted an event, not that the remote app received it.

Cancellation is polling-based. There is an unavoidable race between checking
foreground state and Windows dispatching an input event; key release after a
permission/focus change is best effort. Remote Caps Lock/layout/grab state is not
observable reliably from outside the remote client. The updater trusts the release
signer and the user's account; defending against arbitrary same-user process
tampering is outside this application's security boundary.

Final UI review also corrected Settings exceeding smaller work areas, recording
keys being delivered to child controls, high-DPI nonclient sizing, and a floating
button drag returning to its starting point being mistaken for a click. Settings
now has native vertical scrolling, keyboard focus reveal, and resizable width.
Visual appearance, native-themed controls in dark/high-contrast modes, and real
interaction remain pending the owner's acceptance. The installer script and
packaging changes were reviewed; no installer compilation or install was performed.

## Primary API references

- [ToUnicodeEx flags and dead-key state](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-tounicodeex)
- [GetKeyState and thread message-queue state](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getkeystate)
- [Supported DWM window appearance attributes](https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/ne-dwmapi-dwmwindowattribute)
- [Inno Setup executable-version metadata](https://jrsoftware.org/ishelp/topic_getstringfileinfo.htm)
