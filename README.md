# nintysh

This is a vibecoded Windows 98-style Wayland shell for niri.

It is not production software. You probably shouldn't touch this.

No support. No guarantees. It works on my machine.

## Actual documentation

### Screenshots

| Desktop | Launcher |
| --- | --- |
| ![Desktop](docs/screenshots/01-desktop.png) | ![Launcher](docs/screenshots/02-launcher.png) |

| Volume | Clipboard |
| --- | --- |
| ![Volume](docs/screenshots/03-volume.png) | ![Clipboard](docs/screenshots/04-clipboard.png) |

| Calendar | OSD |
| --- | --- |
| ![Calendar](docs/screenshots/05-calendar.png) | ![OSD](docs/screenshots/06-osd.png) |

### Requirements

- Linux with Wayland
- niri
- GTK4 + gtk4-layer-shell
- `wl-clipboard` for text and image clipboard history
- `wpctl` for audio output, microphone and default-device control
- NetworkManager (`nmcli`) for the Wi-Fi network panel
- PAM for the built-in lock screen
- `brightnessctl`, `grim`, `slurp` and `loginctl` for the optional IPC actions
- a system battery exposed through `/sys/class/power_supply` for the battery widget

Nix users get the GTK/JSON/wl-clipboard/audio/screenshot dependencies from the
flake. The shell also owns `org.freedesktop.Notifications` on the session bus,
so stop any other notification daemon first.

### Build and run

```bash
just build
just run
```

With Nix:

```bash
nix build .#
nix run .#
nix develop
```

The shell is a layer-shell client. Start it inside a running niri session.

### Features

- Win98-style taskbar with Start menu, workspaces, window tasks and system tray
- Launcher with typed search, keyboard navigation and app icons
- `/` prefix in the launcher runs a shell command
- Audio panel with output volume/mute, microphone volume/mute, PipeWire device
  selection, and default sink/source switching
- Notification daemon (`org.freedesktop.Notifications`) with separate stacked
  popup cards, optional images (app icon, `image-path`, `image-data`), action
  buttons, notification history, and a tray icon
- MPRIS media panel with album art, progress/seek, play/pause, previous and
  next controls, player selection, plus tray and IPC control
- Per-monitor display panel with brightness and color-temperature sliders;
  internal panels use hardware backlight control, temperature and external
  outputs use wlr-gamma-control, and values are persisted per output
- Calendar panel
- NetworkManager network panel with Wi-Fi toggle, scanning, active connection
  info, signal/security list, saved/open connections and a password prompt for
  new secured networks
- Battery information panel
- Clipboard history panel with text and image entries, thumbnails, search,
  arrow-key navigation and click/Enter-to-copy; history persists across restarts
- Built-in PAM lock screen covering every monitor
- Session menu with lock, suspend, log out, reboot and shut down actions
- Right-click task menu: focus, floating, fullscreen, close
- Per-monitor taskbars
- Multi-monitor window/workspace filtering
- Optional taskbar application buttons (`bar.showApps`)
- Clock, audio, network, battery, notification and keyboard-layout indicators

### CLI control

The running shell listens on:

```text
$XDG_RUNTIME_DIR/nintysh-$WAYLAND_DISPLAY.sock
```

Override with `NINTYSH_SOCKET`.

Commands use an object/action grammar instead of the flat Noctalia verbs:

```text
nintysh msg <object> <action> [argument...]
```

Panels:

```bash
nintysh msg panel open audio
nintysh msg panel close
nintysh msg panel toggle clipboard
nintysh msg panel toggle notifications
```

Panel names: `launcher`, `audio`, `media`, `display`, `clipboard`,
`calendar`, `network`, `power`, `notifications`, `session`.

Audio output and microphone:

```bash
nintysh msg audio open
nintysh msg audio output volume up
nintysh msg audio output volume down
nintysh msg audio output volume set 40
nintysh msg audio output mute
nintysh msg audio output default next
nintysh msg audio output default prev
nintysh msg audio output default "Nothing Headphone"
nintysh msg audio output default 95
nintysh msg audio input volume up
nintysh msg audio input volume set 60
nintysh msg audio input mute
nintysh msg audio input default "Digital Microphone"
```

`audio volume ...`, `audio up`, `audio down`, `audio mute` and `audio set 40`
operate on the output device. `audio input ...` operates on the microphone.
Device selectors accept a `wpctl` ID, an exact label, a label substring,
`next`, or `prev`.

Notifications:

```bash
nintysh msg notifications open
nintysh msg notifications toggle
nintysh msg notifications dismiss 12
nintysh msg notifications dismiss all
nintysh msg notifications clear
```

Network:

```bash
nintysh msg network open
nintysh msg network scan
nintysh msg network connect "OPPO A78"
nintysh msg network disconnect
nintysh msg network refresh
```

Media / MPRIS:

```bash
nintysh msg media open
nintysh msg media play-pause
nintysh msg media play
nintysh msg media pause
nintysh msg media next
nintysh msg media previous
nintysh msg media seek +5
nintysh msg media seek -10
nintysh msg media position 60
```

The media panel follows the currently playing MPRIS player, preferring a
player whose status is `Playing`. Use the player list at the bottom of the
panel to pin a different player; the choice is kept while that player remains
available. Local album art (`file://` or an absolute path) is rendered; remote
artwork URLs are ignored.

Session menu and actions:

```bash
nintysh msg session
nintysh msg session toggle
nintysh msg panel toggle session
nintysh msg session lock
nintysh msg session suspend
nintysh msg session logout
nintysh msg session reboot
nintysh msg session shutdown
```

Desktop actions:

```bash
nintysh msg brightness up
nintysh msg brightness down
nintysh msg screenshot region
nintysh msg screenshot fullscreen
nintysh msg workspace focus 2
nintysh msg layout next
nintysh msg osd show "Hello"
nintysh msg run 'foot -e btop'
```

Niri binds can use these directly:

```nix
"Mod+V".action =
  config.lib.niri.actions.spawn-sh "nintysh msg panel toggle clipboard";
"XF86AudioRaiseVolume".action =
  config.lib.niri.actions.spawn-sh "nintysh msg audio output volume up";
"XF86AudioLowerVolume".action =
  config.lib.niri.actions.spawn-sh "nintysh msg audio output volume down";
"Print".action =
  config.lib.niri.actions.spawn-sh "nintysh msg screenshot region";
```

The old flat verbs (`panel-toggle`, `volume-up`, `mic-up`, `settings-toggle`,
`screenshot-region`, etc.) are still accepted as compatibility aliases.

### Noctalia compatibility

Most Noctalia keybind commands work unchanged after replacing `noctalia` with
`nintysh`:

```bash
nintysh msg panel-toggle launcher
nintysh msg panel-toggle control-center
nintysh msg panel-toggle clipboard
nintysh msg settings-toggle
nintysh msg session lock
nintysh msg volume-up
nintysh msg volume-down
nintysh msg volume-mute
nintysh msg brightness-up
nintysh msg brightness-down
nintysh msg screenshot-region
nintysh msg screenshot-fullscreen
```

Additional compatibility aliases include `settings-open`, `settings-close`,
`session lock-and-suspend`, `volume-set`, `volume-osd`, `mic-volume-*`,
`wifi-enable`, `wifi-disable`, `wifi-toggle`, `media play|pause|toggle|next|previous`,
`clipboard-clear`, `clipboard-copy`, `notification-clear-active`,
`notification-clear-history`, and `osd-show`.

Commands that print state to stdout, such as `volume-osd` returning values or
`wifi-status`, cannot act as a full request/response replacement because the
nintysh IPC socket has no reply channel; they still perform their UI action.

### Configuration

Compile-time settings live in:

- `src/config.h`
- `src/config.user.h`

`config.h` includes `config.user.h` first, then provides defaults guarded by `#ifndef`. You can edit either file directly or provide overrides from Nix.

Example `config.user.h`:

```c
#define NINTYSH_BAR_HEIGHT 30
#define NINTYSH_BAR_SCALE 1.0
#define NINTYSH_PANEL_SCALE 1.0
#define NINTYSH_TASKBAR_APPS 1
#define NINTYSH_FONT_FAMILY "Tahoma"
#define NINTYSH_FONT_PX 12
#define NINTYSH_COLOR_FACE 0xC0C0C0
#define NINTYSH_COLOR_NAVY 0x000080
#define NINTYSH_ICON_SIZE 24
#define NINTYSH_CLIPBOARD_MAX_ENTRIES 50
#define NINTYSH_CLIPBOARD_ROW_HEIGHT 34.0
#define NINTYSH_CLIPBOARD_IMAGE_MAX_BYTES (8 * 1024 * 1024)
#define NINTYSH_AUDIO_STEP 5
#define NINTYSH_NOTIFICATION_MAX_ENTRIES 50
#define NINTYSH_NOTIFICATION_POPUP_WIDTH 380
#define NINTYSH_NOTIFICATION_POPUP_MAX 3
#define NINTYSH_NOTIFICATION_POPUP_MARGIN 12
#define NINTYSH_LOCK_COMMAND "loginctl lock-session"
#define NINTYSH_BRIGHTNESS_UP_COMMAND "brightnessctl set +5%"
#define NINTYSH_BRIGHTNESS_DOWN_COMMAND "brightnessctl set 5%-"
#define NINTYSH_SCREENSHOT_REGION_COMMAND "grim -g \"$(slurp)\" - | wl-copy"
#define NINTYSH_SCREENSHOT_FULLSCREEN_COMMAND "grim - | wl-copy"
```

Configurable groups include font, bar geometry and scale, panel scale, launcher
geometry, icons, OSD, clipboard, audio, notifications, update intervals, IPC
action commands and Win98 colors.

### Nix package helpers

The flake exposes:

```nix
packages.default
packages.nintysh
overlays.default
lib.mkNintysh
```

Custom package:

```nix
inputs.nintysh.lib.mkNintysh pkgs {
  extraConfig = ''
    #define NINTYSH_BAR_HEIGHT 30
    #define NINTYSH_COLOR_FACE 0xE0E0E0
  '';
  wrapperArgs = [ ];
}
```

### Home Manager

Available module outputs:

```nix
inputs.nintysh.modules.homeManager.default
inputs.nintysh.modules.homeManager.nintysh
inputs.nintysh.homeModules.default
inputs.nintysh.homeManagerModules.default
```

Example:

```nix
{
  imports = [ inputs.nintysh.homeModules.default ];

  programs.nintysh = {
    enable = true;
    autostart = true;

    settings = {
      bar.height = 30;
      bar.scale = 1.0;
      bar.showApps = true;
      panels.scale = 1.0;
      font.size = 12;
      launcher.width = 280;
      icons.size = 24;
      clipboard.maxEntries = 50;
      clipboard.rowHeight = 34.0;
      clipboard.imageMaxBytes = 8388608;
      audio.step = 5;
      notifications.maxEntries = 50;
      notifications.timeoutMs = 6000;
      notifications.popupWidth = 380;
      notifications.popupMax = 3;
      osd.timeoutMs = 1800;
      commands.screenshotRegion = "grim -g \"$(slurp)\" - | wl-copy";
      colors.face = "#ffcc00";
      colors.selection = "#000080";
    };

    extraConfig = ''
      #define NINTYSH_CLIPBOARD_MAX_BYTES (128 * 1024)
    '';
  };
}
```

The module installs the package and, with `autostart`, creates a systemd user service tied to `graphical-session.target`.

### Current limitations

- niri only; the Sway backend was not kept
- taskbars are created for every monitor, but menus, panels and OSD still open on the primary monitor
- notification history is in-memory and limited by compile-time config; clipboard history is persisted
- no StatusNotifier tray support
- notification inline replies and notification sound hints are not implemented
- notification `image-data` rendering is best-effort for 8-bit RGB/RGBA
- window icon matching is heuristic
- media control depends on MPRIS; remote album-art URLs are not downloaded
- external display brightness uses gamma dimming; DDC/CI is not implemented
- audio control and default-device switching depend on `wpctl`/PipeWire
- the Wi-Fi panel uses `nmcli`; new secured networks prompt for a password
- `settings-toggle` opens the audio panel; there is no separate settings UI

