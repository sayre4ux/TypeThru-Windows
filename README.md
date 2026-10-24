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
- **Stops instantly** when you press Esc, click, or switch windows.
- **Your shortcut**: Ctrl+; by default, or choose your own.
- **The Type button**: full-screen VDI clients grab every key, so the shortcut cannot
  reach your PC. The small always-on-top "T" button always works - click it, then
  click where the text should go. Drag it anywhere; right-click hides it.
- **Lightweight**: a single 1 MB executable, no runtime dependencies, no background services.
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

1. Open TypeThru. Its icon sits in the notification area, and the "T" Type button
   sits in the corner of the screen.
2. Copy the text you want to send.
3. Click into the text field in your remote session.
4. Press **Ctrl+;**, or - in a full-screen session where the keyboard belongs to the
   remote desktop - click the **T** button first, then click the text field.

Full-screen sessions: some clients give your PC the keyboard back when you press
Ctrl+Alt. If you use that, click the text field inside the session once before
pressing Ctrl+; - the mouse always works - so the session has settled when the
text starts. The T button needs none of this.

Heavy apps (browsers, Teams) over a remote link can drop keys at the two fastest
speeds; use them in editors, and keep the average speed for anything critical.

Keep the keyboard layout on this PC and in the remote session the same, as with a real
keyboard. TypeThru cannot type into windows that run elevated (as administrator) unless
TypeThru itself is elevated.

## Build from source

Visual Studio 2022 with the C++ desktop workload:

```bat
scripts\build.cmd all     :: .build\TypeThru.exe, TypeThruPOC.exe, Tests.exe
.build\Tests.exe          :: unit tests; posts no input
scripts\package.cmd       :: release zip + update signature (+ installer when Inno Setup 6 is present)
```

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
- TypeThru sends key presses; it does not listen to the keyboard. While the panel
  is open, a click watcher closes it when you click elsewhere; it sees only click
  positions during those seconds and is removed as soon as the panel closes.
- The update check contacts `api.github.com` at most once a day and can be turned off
  in Settings. An update is downloaded and installed only when you choose **Install**,
  and only if it carries this project's signature.

## License

MIT — see [LICENSE](LICENSE).
