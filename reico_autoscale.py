bl_info = {
    "name": "REICO Auto UI Scale",
    "description": "Scales Blender's whole interface with the window width, so small windows aren't crammed",
    "author": "REICO",
    "version": (1, 0),
    "blender": (4, 0, 0),
    "category": "Interface",
}

# Blender's whole interface (panels, tabs, text) scales with the window
# width, so a small Blender window shows the same layout, just smaller -
# nothing gets crammed together. Real Blender rendering, so it stays sharp
# and mouse/keyboard work normally.
#
# Full screen width = your normal Resolution Scale. Half the width = half the
# scale (never below MIN_FACTOR).
#
# Install: Edit > Preferences > Add-ons > (top-right arrow) Install from Disk >
# pick this file, then tick "REICO Auto UI Scale".

import bpy
import ctypes

MIN_FACTOR = 0.5    # smallest UI, as a fraction of your normal scale
CHECK_EVERY = 0.25  # seconds

_base = None  # your normal Resolution Scale, read once at startup
_last = None


def _screen_width():
    try:
        return ctypes.windll.user32.GetSystemMetrics(0)  # primary monitor, real pixels
    except Exception:
        return 1920


def _tick():
    global _base, _last
    try:
        prefs = bpy.context.preferences
        wm = bpy.context.window_manager
        if not wm or not wm.windows:
            return CHECK_EVERY
        if _base is None:
            _base = prefs.view.ui_scale

        width = wm.windows[0].width
        factor = max(MIN_FACTOR, min(1.0, width / _screen_width()))
        scale = round(_base * factor, 2)

        if scale != _last:
            dirty = prefs.is_dirty
            prefs.view.ui_scale = scale
            prefs.is_dirty = dirty  # don't let auto-save store the shrunk value as your default
            _last = scale
            print(f"[REICO autoscale] window width {width} -> ui scale {scale}", flush=True)
    except Exception as e:
        print(f"[REICO autoscale] {e}")
    return CHECK_EVERY


def register():
    if not bpy.app.timers.is_registered(_tick):
        bpy.app.timers.register(_tick, first_interval=1.0, persistent=True)


def unregister():
    if bpy.app.timers.is_registered(_tick):
        bpy.app.timers.unregister(_tick)
    if _base is not None:
        bpy.context.preferences.view.ui_scale = _base


if __name__ == "__main__":  # run with: blender --python reico_autoscale.py
    register()
