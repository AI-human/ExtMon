#!/usr/bin/env python3
"""extmon — external monitor brightness/tone CLI for GNOME Wayland.

Backend: org.gnome.Mutter.DisplayConfig SetCrtcGamma (1D gamma LUT).
State file is shared with the ExtMon GUI.

Usage:
  extmon.py set <key> <value> [key value ...]   adjust and apply
  extmon.py preset <Name>                       apply a preset
  extmon.py presets                             list presets
  extmon.py save <Name>                         save current state as preset
  extmon.py reset                               neutral settings
  extmon.py status                              show state + live LUT check

Keys: gain|brightness, contrast, lift, gamma, shadows, highlights,
      temp, red, green, blue
"""
import sys, os, json
import gi
gi.require_version('Gio', '2.0')
from gi.repository import Gio, GLib

BUS = 'org.gnome.Mutter.DisplayConfig'
PATH = '/org/gnome/Mutter/DisplayConfig'
IFACE = 'org.gnome.Mutter.DisplayConfig'
CONNECTOR = os.environ.get('EXTMON_CONNECTOR', 'HDMI-1')
STATE = os.path.expanduser('~/.config/extmon.state.json')
PRESETS = os.path.expanduser('~/.config/extmon-presets.txt')
N = 1024

KEYS = {
    'gain': 'gain', 'brightness': 'gain', 'bright': 'gain',
    'contrast': 'contrast', 'lift': 'lift', 'gamma': 'gamma',
    'shadows': 'shadows', 'highlights': 'highlights',
    'temp': 'temp', 'temperature': 'temp', 'warm': 'temp',
    'red': 'tint_r', 'green': 'tint_g', 'blue': 'tint_b',
}

DEFAULTS = dict(gain=1.0, contrast=1.0, lift=0.0, gamma=1.0, shadows=0.0,
                highlights=0.0, temp=0.0, tint_r=1.0, tint_g=1.0, tint_b=1.0)

proxy = Gio.DBusProxy.new_for_bus_sync(
    Gio.BusType.SESSION, Gio.DBusProxyFlags.NONE, None, BUS, PATH, IFACE, None)

def call(method, sig, args):
    return proxy.call_sync(method, GLib.Variant(sig, args),
                           Gio.DBusCallFlags.NONE, 5000, None).unpack()

def get_target():
    serial, crtcs, outputs, modes, _, _ = call('GetResources', '()', ())
    for out in outputs:
        props = out[-1]
        if CONNECTOR in (props.get('connector'), props.get('display-name')):
            if out[2] == 0:
                sys.exit(f'{CONNECTOR} has no active CRTC')
            return serial, out[2], out[0]  # serial, crtc, output id
    sys.exit(f'{CONNECTOR} not connected')

def load():
    try:
        with open(STATE) as f:
            st = json.load(f)
    except Exception:
        st = {}
    d = dict(DEFAULTS)
    d.update({k: float(v) for k, v in st.items() if k in DEFAULTS})
    return d

def save(st):
    with open(STATE, 'w') as f:
        json.dump(st, f, indent=1)

def clamp01(v): return min(1.0, max(0.0, v))

def tone(x, t):
    x = clamp01(x)
    x = x ** (1.0 / max(t['gamma'], 1e-4))
    dark, light = (1 - x) ** 2, x * x
    if t['shadows'] > 0: x += t['shadows'] * 0.6 * dark * (1 - x)
    else:                x *= (1 + t['shadows'] * 0.8 * dark)
    if t['highlights'] > 0: x += t['highlights'] * 0.5 * light * (1 - x)
    else:                   x *= (1 + t['highlights'] * 0.5 * light)
    x = clamp01(x)
    x = t['contrast'] * (x - 0.5) + 0.5
    return clamp01(t['gain'] * x + t['lift'])

def channel_scales(t):
    return ((1 + 0.25 * t['temp']) * t['tint_r'],
            (1 - 0.05 * t['temp']) * t['tint_g'],
            (1 - 0.25 * t['temp']) * t['tint_b'])

def build_ramps(t):
    scales = channel_scales(t)
    ramps = []
    for sc in scales:
        ramps.append([int(round(clamp01(tone(i / (N - 1), t) * sc) * 65535)) for i in range(N)])
    return ramps

def apply(st):
    serial, crtc, oid = get_target()
    r, g, b = build_ramps(st)
    call('SetCrtcGamma', '(uuaqaqaq)', (serial, crtc, r, g, b))
    save(st)
    print(f"applied to {CONNECTOR}: gain={st['gain']:.2f} contrast={st['contrast']:.2f} "
          f"gamma={st['gamma']:.2f} shadows={st['shadows']:.2f} highlights={st['highlights']:.2f} "
          f"temp={st['temp']:.2f} rgb=({st['tint_r']:.2f},{st['tint_g']:.2f},{st['tint_b']:.2f})")

BUILTIN = {
    'Default': {}, 'Bright': dict(gain=1.30, contrast=1.05),
    'Bright+': dict(gain=1.50, contrast=1.10, lift=0.02),
    'Movie': dict(gain=1.10, contrast=1.25, temp=0.15),
    'Vivid': dict(contrast=1.30, gamma=0.95, gain=1.05, temp=0.10),
    'Night': dict(gain=0.95, temp=0.55),
    'Soft': dict(gain=0.92, contrast=0.90, lift=0.05),
    'Cool': dict(temp=-0.40), 'Warm': dict(temp=0.40),
}

ORDER = ('gain', 'contrast', 'lift', 'gamma', 'shadows', 'highlights',
         'temp', 'tint_r', 'tint_g', 'tint_b')

def user_presets():
    d = {}
    try:
        with open(PRESETS) as f:
            for line in f:
                name, _, vals = line.strip().partition('|')
                v = [float(x) for x in vals.split(',')]
                if len(v) == len(ORDER):
                    d[name] = dict(zip(ORDER, v))
    except FileNotFoundError:
        pass
    return d

if __name__ == '__main__':
    if len(sys.argv) < 2 or sys.argv[1] in ('-h', '--help', 'help'):
        print(__doc__); sys.exit(0)
    cmd = sys.argv[1]

    if cmd == 'set':
        st = load()
        args = sys.argv[2:]
        if len(args) < 2 or len(args) % 2:
            sys.exit('usage: extmon.py set <key> <value> [key value ...]')
        for k, v in zip(args[::2], args[1::2]):
            if k not in KEYS: sys.exit(f'unknown key "{k}". keys: {", ".join(sorted(set(KEYS)))}')
            st[KEYS[k]] = float(v)
        apply(st)
    elif cmd == 'status':
        st = load()
        print('state:', json.dumps(st, indent=None))
        serial, crtc, oid = get_target()
        cr, cg, cb = call('GetCrtcGamma', '(uu)', (serial, crtc))
        mid = len(cr) // 2
        print(f"live LUT: len={len(cr)} midpoint R,G,B = {cr[mid]},{cg[mid]},{cb[mid]}")
    elif cmd == 'presets':
        print('builtin:', ', '.join(BUILTIN))
        print('user:   ', ', '.join(user_presets()) or '(none)')
    elif cmd == 'preset':
        name = ' '.join(sys.argv[2:])
        st = dict(DEFAULTS)
        if name in BUILTIN: st.update(BUILTIN[name])
        elif name in user_presets(): st.update(user_presets()[name])
        else: sys.exit(f'preset "{name}" not found')
        apply(st)
    elif cmd == 'save':
        name = ' '.join(sys.argv[2:])
        if not name: sys.exit('usage: extmon.py save <Name>')
        st = load()
        with open(PRESETS, 'a') as f:
            f.write(name + '|' + ','.join(f"{st[k]:.3f}" for k in ORDER) + '\n')
        print(f'saved preset "{name}"')
    elif cmd == 'reset':
        apply(dict(DEFAULTS))
        if os.path.exists(STATE): os.remove(STATE)
        print('reset to neutral')
    else:
        print(__doc__); sys.exit(1)
