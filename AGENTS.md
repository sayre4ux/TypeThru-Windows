# AGENTS.md

Guidance for AI coding agents working on TypeThru for Windows. Read this before
changing code.

TypeThru for Windows is the Windows edition of TypeThru for macOS
(github.com/sayre4ux/TypeThru). It types the clipboard as scan-code key presses into
VDI and remote desktop sessions where paste does not work. It is small (C++17, pure
Win32, no package manager, no project file); `scripts\build.cmd` builds everything.

## Bootstrap

1. Read this file end to end, then `DEVLOG.md` (git-ignored local log) for the
   session history and current state - its top entry is the most recent truth.
2. Run `scripts\check.cmd` (tests + clean build) before touching anything, so
   you start from a known-good state.
3. Hard rules, unchanged: never post real keyboard input (`--type` or
   panel typing actions), never run installers/uninstallers, never commit
   keys or the DEVLOG.
4. The owner runs everything that needs a real screen: panel interactions,
   remote-session tests, setup and uninstall. When a change affects those, list
   the exact manual steps instead of claiming they work.
5. Optional second opinion: an external reviewer CLI may be installed. If used,
   run it single-shot with read-only tool permissions, and remember it cannot
   post approvals in headless mode - give it everything it needs to read.

## Live session findings (a real VDI, keep this true)

These were established experimentally against a Horizon client with a Windows
guest; do not regress them and do not re-litigate them without new evidence:

- Scan-code SendInput reaches the guest at 20 ms per key with zero drops
  (Notepad); Esc and click-stop work through the grab.
- Full-screen keyboard grab consumes every chord before RegisterHotKey, so the
  floating T button is the only trigger guaranteed to work there.
- The Ctrl+Alt escape poisons the session: everything typed afterwards (any
  trigger path) loses its letters and digits while capitals and punctuation
  survive - a phantom Alt in the guest that no external input could clear
  (orphan key-ups are not forwarded; real Ctrl+Alt+Shift pairs make it worse).
  Recovery is letting the session take the keyboard back. Never send modifier
  chords to "fix" input; see DEVLOG 2026-10-24 for the full experiment log.

## Layout

| Path | Role |
|---|---|
| `scripts/build.cmd` | Entry point: `app`, `tests`, `preview`, or `all` into `.build\` |
| `scripts/test.cmd` | Runs `Tests.exe` |
| `scripts/check.cmd` | Tests + a clean build; run before every push |
| `scripts/package.cmd` | Release zip, update signature, installer (needs Inno Setup 6) |
| `scripts/make-key.ps1` | Once per signer: the CNG key that signs updates; writes `src/updatekey.h` |
| `scripts/sign-update.ps1` | Signs a package into `<file>.sig` (base64 raw ECDSA) |
| `src/main.cpp` | Entry point, CLI modes (`--print-map`, `--type`, `--version`, `--finalize-update`) |
| `src/app.cpp` | Tray icon, panel, Settings window, trigger flow, update UI, diagnostics |
| `src/typer.cpp/.h` | The typing engine: four methods, per-character input construction |
| `src/keymap.cpp/.h` | Character → (VK, scan code, Shift) table built from the active layout |
| `src/stopwatcher.cpp/.h` | Esc / click / window-change stop detection on its own thread |
| `src/hotkey.cpp/.h` | Shortcut rules and names (RegisterHotKey is driven from app.cpp) |
| `src/settings.cpp/.h` | HKCU settings store, Run-key login entry, clipboard read |
| `src/shared.cpp/.h` | Logic shared with the macOS app: symbol table, speeds, tests, versions |
| `src/updates.cpp/.h` | GitHub release check, JSON parsing, ECDSA/CNG verify, install |
| `src/updatekey.h` | The update public key; generated, never edited by hand |
| `tools/render-ui.cpp` | Memory-only UI preview renderer; never posts input |
| `tests/tests.cpp` | Unit tests; constructs inputs and inspects them, never posts |
| `installer/typethru.iss` | Inno Setup script (per-user install, no admin) |
| `res/` | Icon, manifest, version resource |
| `.github/workflows/` | CI (build + tests) and CodeQL |

## Commands

```bat
scripts\build.cmd all          :: build TypeThru.exe and Tests.exe
.build\Tests.exe               :: unit tests; posts no input
scripts\check.cmd              :: everything required before a push
.build\TypeThru.exe --print-map "text"    :: keys a text would use; types nothing
.build\TypeThru.exe --version
```

Run `scripts\test.cmd` after every code change and `scripts\check.cmd` before a push.

## What an agent must not do

- **Never post real keyboard input.** Do not run `--type`
  or any UI action that triggers typing. Unit tests construct and inspect INPUT structs;
  they never call SendInput. `--print-map` and `--version` are safe.
- **Never run the installer or uninstaller**, or delete registry keys outside a test.
  Both change the user's system state.
- **Never commit the update private key** (`.build/update-key-private.txt` is
  git-ignored) or any certificate material.
- **Keep the shared tables in sync with the macOS app.** The symbol replacement table,
  the five speeds, the typing-test texts, and the trailing-line-break rule exist on both
  platforms; a change here belongs in the same change there (Sources/main.swift).
- Do not name specific VDI or remote desktop vendors in the UI, README, or code
  comments. Say "VDI and remote desktops".

## Architecture

**Trigger.** Full-screen VDI clients swallow every key before RegisterHotKey runs
(tested live: even Ctrl+Alt+Shift+T does not survive an immersive-mode grab, so
do not try to beat a grab with an exotic chord), so the floating Type button (WS_EX_NOACTIVATE, always on top, click = the panel's
Type Clipboard flow; drag moves it, right-click hides it, position saved in the
registry) is on by default and is the trigger that works everywhere. Otherwise,
RegisterHotKey (Ctrl+; = VK_OEM_1 + MOD_CONTROL, `MOD_NOREPEAT`) fires
WM_HOTKEY on key-down; the run then waits for all modifiers and the key itself to be
released before typing (the remote client receives the physical key-up sequence first).
The panel's "Type Clipboard" waits 1 s (tests 3 s) for the user to click the target
field. The target is the foreground window captured just before typing; typing into
TypeThru itself is refused.

**Typing** (src/typer.cpp) has four methods, in order of preference:
1. **Scancode** — SendInput with KEYEVENTF_SCANCODE (+ EXTENDEDKEY where needed): what
   remote clients accept, because scan codes look like the physical keyboard. Default.
2. **Virtual key** — SendInput by VK only, for diagnosis.
3. **Unicode** — KEYEVENTF_UNICODE UTF-16 units; Enter/Tab stay physical keys.
4. **PostWindow** — WM_KEYDOWN/WM_CHAR posted to the target window; diagnostic only.

Before the first key: a 150 ms warm-up, and the foreground window must have been
the same window for 300 ms (bounded at 2 s, `settledForegroundWindow`) - releasing
a full-screen client's keyboard grab (Ctrl+Alt) leaves focus flickering and keys
sent into the transition get dropped (seen live twice: lowercase letters AND digits vanished in
Teams while capitals and punctuation survived. Neither orphan key-ups nor real
Ctrl+Alt+Shift press-release pairs cured it, and the pairs regressed every
trigger path including the T button into Notepad - the client reads the rapid
chord as the start of another escape and eats the plain letters after it. No
modifier clearing is sent; the T button is the full-screen answer. Final live
experiment: the T button after a Ctrl+Alt escape garbles identically, so the
chord poisons the session state itself - path-independent, unfixable from
outside; recovery is letting the session take the keyboard back). Per character: key down → hold (80 ms or half the interval, `tt::timing`) → key up →
gap. Shifted characters add a left-Shift press/release with a 20 ms margin. Line breaks
settle for 300 ms because remote apps can reorder them. `TimerResolution` raises the
timer to 1 ms while typing. The whole text is checked against the layout before
anything is sent; unsupported characters abort the run. Line breaks at the end are
dropped (`withoutTrailingLineBreaks`) and newlines are typed as Shift+Enter by default
(`Typer::shiftReturn`).

**Key map** (src/keymap.cpp) walks VK 0x20–0xFE twice (unshifted first, so "8" wins
over "*"), maps through ToUnicodeEx with dead-key protection, and skips keypad and
control keys (see `isControlKey`; getting those ranges wrong once emptied the whole
map). `MAPVK_VK_TO_VSC_EX` supplies scan codes; the 0x100 bit marks extended keys.

**Stop** (src/stopwatcher.cpp): a thread polls every 10 ms — Esc down, any mouse button
newly down (buttons already down at start don't count), or the foreground window
changing. There is no accessibility permission on Windows; nothing to request.

**Updates** (src/updates.cpp): daily GitHub releases check (at most; off in Settings).
Trust requires ALL of: the page/file is this repo's github.com URL, the zip's ECDSA
P-256 signature verifies against `updateKeyBlob` (CNG; the private key is a CNG user
key "TypeThru-Update" created by make-key.ps1), and the version is newer. Install =
extract with tar.exe → run the new exe with `--finalize-update` → it waits for the old
one to exit, replaces it, and relaunches. Settings live in HKCU\Software\TypeThru and
survive updates.

**UI** (src/app.cpp): tray icon → panel (WS_EX_NOACTIVATE, so the remote client keeps
focus) with method/speed/tests; Settings window with shortcut recording (hotkey
unregistered while recording), start-at-sign-in (HKCU Run key), update toggle,
uninstall. Diagnostics mirror the macOS fields; nothing records clipboard contents.

## Design rules and why

- **No admin, no driver, no permission prompts.** SendInput at scan-code level is
  indistinguishable enough for VDI clients; this is the whole reason the Windows
  edition needs no setup. Revisit only with evidence from a real session.
- **UIPI**: a non-elevated TypeThru cannot type into elevated windows. Diagnostics
  reports this when the front window is elevated; the README says to run TypeThru
  elevated in that case. Do not "fix" by requesting admin by default.
- **Ctrl-only shortcuts are safest**: Alt, Win, and Shift pass through to the remote
  session (Alt opens menus, Win opens Start). Same reasoning as macOS; the Settings
  caution text mirrors it.
- **The README's Privacy section is a contract**: clipboard read once in memory, no
  logging, no keyboard listening, network only for the update check. The one mouse
  touch is a WH_MOUSE_LL click watcher that exists only while the panel is visible
  (it closes the panel on an outside click, because the panel is NOACTIVATE and so
  gets no focus message); it sees click positions only and unhooks on close. If a change would
  break any of these, stop and ask. If a change alters what the app can see or do,
  update that section in the same change.
- **Signing**: releases may be unsigned or self-signed for now; SmartScreen shows
  "Run anyway". Windows does not tie permissions to the signature, so switching to an
  OV certificate later breaks nothing for users. The update channel is protected by
  the Ed25519-equivalent CNG key regardless.

## Release checklist

1. Bump the version in `src/version.h` (all four defines).
2. `scripts\check.cmd`; then `scripts\package.cmd` (needs the TypeThru-Update CNG key;
   it writes the zip, the `.sig`, and the installer when Inno Setup 6 is installed).
3. Test the zip on a clean account: run, tray panel, shortcut, typing tests into
   Notepad, Settings, Check Now, uninstall. Then the same inside the real remote
   session (only the user can do this).
4. Publish a GitHub pre-release with the zip, its `.sig`, the installer, and the zip's
   SHA-256 in the notes. Without the `.sig`, the app offers no Install for that release.
5. From the previous version, choose Install: it must update, reopen, and keep settings.

## Style

- Match the existing code: compact, few comments, comments explain why rather than what.
- Mark judgment calls with `// DECISION:` and a one-line reason.
- User-facing text is plain and short and tells the user the next step.
- `/W4` clean: no warnings on any target.
