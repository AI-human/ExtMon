# ExtMon — External Monitor Tuner for GNOME Wayland

A lightweight brightness / tone tuner for external monitors, designed for
GNOME on Wayland where `xrandr` gamma/brightness controls do not work.

Instead of `xrandr`, ExtMon talks directly to GNOME's compositor over D-Bus,
using `org.gnome.Mutter.DisplayConfig → SetCrtcGamma` to program a 1D gamma
LUT per CRTC (gain, contrast, lift, gamma, shadows, highlights, temperature,
RGB balance).

> Note on `SetOutputCTM`: the method exists and the call succeeds, but on
> current GNOME (verified on GNOME 50) the matrix is never applied to the
> output. Cross-channel color controls (saturation/vibrance/hue) are
> therefore not part of ExtMon.

By default it only touches the external monitor (`HDMI-1`); the laptop panel is left alone.

## Features

- GTK4 GUI with live sliders and one-click presets
- Python CLI sharing the same state/preset files (scriptable, usable from keybinds)
- Built-in presets: Default, Bright, Bright+, Movie, Vivid, Night, Soft, Cool, Warm
- Save your own presets from the GUI or CLI

## Controls

| Control | Range | Neutral | Notes |
|---|---|---|---|
| Brightness (gain) | 0.5 – 1.6 | 1.0 | LUT |
| Contrast | 0.7 – 1.6 | 1.0 | LUT |
| Black lift | 0 – 0.25 | 0.0 | LUT |
| Gamma | 0.5 – 2.0 | 1.0 | LUT |
| Shadows | −1 – 1 | 0.0 | LUT |
| Highlights | −1 – 1 | 0.0 | LUT |
| Temperature | −1 (cool) – 1 (warm) | 0.0 | LUT |
| Red / Green / Blue | 0.5 – 1.5 | 1.0 | LUT |

## Requirements

- GNOME Shell on Wayland (Mutter `org.gnome.Mutter.DisplayConfig` on the session bus)
- GTK 4 (≥ 4.6)
- For the CLI: Python 3 with PyGObject (`python3-gi`)

## Build & install

With make (installs to `~/.local`):

```sh
make
make install          # PREFIX=~/.local by default; use PREFIX=/usr for system-wide
```

With meson:

```sh
meson setup builddir --prefix=$HOME/.local
meson compile -C builddir
meson install -C builddir
```

## Usage

GUI:

```sh
extmon
```

CLI (installed as `extmon-cli`, sources `src/extmon.py` work standalone too):

```sh
extmon-cli set gain 1.2 contrast 1.1 temp 0.2
extmon-cli preset Movie
extmon-cli save "My Look"
extmon-cli presets
extmon-cli reset
extmon-cli status
```

CLI keys: `gain`/`brightness`, `contrast`, `lift`, `gamma`, `shadows`, `highlights`,
`temp`, `red`, `green`, `blue`.

## Configuration

- Target connector: `EXTMON_CONNECTOR` environment variable (default `HDMI-1`)
- State file: `~/.config/extmon.state.json` (shared by GUI and CLI)
- User presets: `~/.config/extmon-presets.txt`

## Notes

- GNOME Night Light / `gnome-settings-daemon` color management may overwrite the
  gamma LUT; if changes appear to be ignored, disable Night Light.

## License

MIT — see [LICENSE](LICENSE).
