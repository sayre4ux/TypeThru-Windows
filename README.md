# TypeThru for Windows

Type your clipboard into VDI and remote desktops where paste does not work.

The Windows edition of [TypeThru for macOS](https://github.com/sayre4ux/TypeThru).

## Features

- **Real key presses**: text is sent as scan-code keyboard input, so remote sessions
  accept it as if you typed it. No driver, no admin rights, no setup.
- **Five speeds**, from an average typist to an unrealistic 600 words per minute.
- **Smart symbols**: bullets, curly quotes, and dashes are typed as plain keys.
- **New lines, not sends**: line breaks are typed as Shift+Enter, and a line break at the
  end is left out, so a copied command never runs by itself.
- **Stops on Esc, a click, or a window change**, with cancellation checked between
  input events and during their waits. Esc also cancels the countdown.
- **Your shortcut**: Ctrl+; by default, or choose your own.
- **The Type button**: full-screen VDI clients grab every key, so the shortcut cannot
  reach your PC. The small always-on-top Type button, marked with the brand arrow,
  always works - click it, then
  click where the text should go. Drag it anywhere; right-click hides it.
- **Lightweight**: a single small executable, no runtime dependencies, no background services.
- **A focused Windows interface**: a prominent clipboard action, clear status and
  speed details, grouped Settings, system light/dark colors, high contrast, and
  scaling for each display. The panel keeps your target app in front.
- **Privacy**: the clipboard is read once when you trigger typing and never written
  anywhere. No logging, no telemetry. The only network request is the daily update
  check against GitHub, which you can turn off.

## Use cases

- **VDI and remote desktops** where the clipboard is turned off.
- **Virtual machine and server consoles**, before guest tools are installed or through a
  web-based KVM.
- **Forms and apps that block paste**, such as confirm-password fields.
- **Long codes**: license keys, recovery keys, and one-time setup codes.
- **Demos and recordings**: natural, human-speed typing for tutorials and live coding.
- **Accessibility**: send prepared text when typing it by hand is hard.

Check your workplace's IT policy before using TypeThru on a managed computer or session.

## Install

Requires Windows 10 21H2 or later, 64-bit.

**Portable** (recommended): download `TypeThru-<version>-win64.zip` from
[Releases](https://github.com/sayre4ux/TypeThru-Windows/releases), unpack it anywhere,
and open `TypeThru.exe`. When Windows shows "protected your PC", choose
**More info → Run anyway**.

**Installer**: `TypeThru-Setup-<version>.exe` does the same and adds shortcuts plus an
optional start-at-sign-in entry (per-user only; no admin needed).

1. Open TypeThru. Its icon sits in the notification area, and the floating Type button
   sits in the corner of the screen.
2. Copy the text you want to send.
3. Click into the text field in your remote session.
4. Press **Ctrl+;**, or - in a full-screen session where the keyboard belongs to the
   remote desktop - click the floating **Type** button first, then click the text field.

Full-screen sessions: keep using the Type button. Some clients give your PC the
keyboard back when you press Ctrl+Alt - avoid it. On some clients the escape
leaves the session believing Alt is still held, and everything typed afterwards
comes out gutted (letters and digits vanish; capitals and punctuation get
through), no matter how it is triggered. If that happens, click back into the
session (or leave and re-enter full screen) so it takes the keyboard again, then
use the Type button.

Heavy apps (browsers, Teams) over a remote link can drop keys at the two fastest
speeds; use them in editors, and keep the average speed for anything critical.

Keep the keyboard layout on this PC and in the remote session the same, as with a real
keyboard, and turn Caps Lock off in both places. TypeThru refuses physical-key typing
when local Caps Lock is on. TypeThru cannot type into windows that run elevated (as administrator) unless
TypeThru itself is elevated.

Open **Settings** for the typing method, smart-symbol conversion, floating button,
shortcut, sign-in, and updates. Scan code keys remain the recommended method.
Posted-window messages are diagnostic only: they can send text to a focused control,
but cannot represent Shift+Enter. Choose another method for protected line breaks.

## Build from source

Visual Studio 2022 with the C++ desktop workload:

```bat
scripts\build.cmd all     :: .build\TypeThru.exe, TypeThruPOC.exe, Tests.exe
.build\Tests.exe          :: unit tests; posts no input
scripts\check.cmd         :: isolated clean build + tests in .build\check\
scripts\package.cmd       :: release zip + update signature (+ installer when Inno Setup 6 is present)
```

`check.cmd` preserves the running app, release files, and signing-key backup in
`.build`. Builds use `/W4 /WX`; compiler warnings fail the check. Set
`TYPETHRU_BUILD_DIR` to select a different build output folder. Packaging always
uses `.build` and writes all release artifacts to `.build\release`.

See [the audit and validation notes](docs/AUDIT.md) and the
[manual acceptance checklist](docs/MANUAL-TESTS.md) before releasing this revision.
The [visual design notes](docs/UI-DESIGN.md) include dark and light offline previews
and the redesigned floating button in 0.2.2.

## Testing against a remote session

`TypeThruPOC.exe` is a standalone proof-of-concept for checking a VDI setup before
installing anything:

```bat
.build\TypeThruPOC.exe --method scancode "abc123"
.build\TypeThruPOC.exe --method unicode --interval 60 "café ☕"
```

Run it, click the remote session's text field, wait for the countdown.

## Privacy

- The clipboard is read once, in memory, when you start typing. It is never saved,
  logged, or sent anywhere.
- TypeThru does not record keystrokes. It polls key states while waiting to type
  and during typing to detect release, cancellation, or held modifiers. Shortcut
  recording receives keys only in the Settings window. While the panel
  is open, a click watcher closes it when you click elsewhere; it sees only click
  positions during those seconds and is removed as soon as the panel closes.
- Automatic update checks contact `api.github.com` at most once every 24 hours,
  including failed attempts and app restarts, and can be turned off
  in Settings. An update is downloaded and installed only when you choose **Install**,
  and only if it carries this project's signature. **Check Now** is an explicit
  additional check. Diagnostics retain counts, reasons and window classes in memory;
  they do not retain clipboard text or window titles.

## License

MIT — see [LICENSE](LICENSE).
