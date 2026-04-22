"""PyInstaller runtime hook — bundled data path + 'press Enter to exit' pause.

The pause keeps the console window open when a user double-clicks an
exe directly from Explorer (otherwise the window vanishes the moment
the script finishes and the user can't read the output).

When launched from one of our .bat wrappers, the bat sets the env
var FLASHY_FROM_BAT=1 so we skip the pause here — the bat's own
`pause` line handles it. That avoids double-pause prompts.
"""
import atexit
import os
import sys


# PyInstaller sets _MEIPASS to the temp extraction directory
if hasattr(sys, '_MEIPASS'):
    base = sys._MEIPASS
    sys.path.insert(0, base)
    gm5byte_dir = os.path.join(base, 'gm5byte')
    if os.path.isdir(gm5byte_dir):
        sys.path.insert(0, gm5byte_dir)


def _flashy_pause_on_exit():
    """Block on input() so the console window stays visible."""
    # Only meaningful when launched into its own console (frozen exe).
    if not getattr(sys, 'frozen', False):
        return
    # Skip when our .bat wrapper already plans to pause.
    if os.environ.get('FLASHY_FROM_BAT') == '1':
        return
    # Skip in non-interactive contexts (piped, redirected, CI).
    try:
        if not sys.stdin or not sys.stdin.isatty():
            return
    except Exception:
        return
    try:
        print()
        input('Press Enter to close this window...')
    except (EOFError, KeyboardInterrupt):
        pass


atexit.register(_flashy_pause_on_exit)
