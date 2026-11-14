"""Exercise the installed Preview GUI without inherited Qt SDK library/plugin paths."""
import os
from pathlib import Path
import subprocess
import sys

stage = Path(sys.argv[1]).resolve()
candidates = [stage / "bin/keyboard-ambient", stage / "bin/keyboard-ambient.exe",
              stage / "keyboard-ambient.app/Contents/MacOS/keyboard-ambient"]
executable = next((path for path in candidates if path.is_file()), None)
if executable is None:
    raise SystemExit("Installed application not found")
env = os.environ.copy()
for key in ("QT_PLUGIN_PATH", "QML_IMPORT_PATH", "QML2_IMPORT_PATH", "LD_LIBRARY_PATH", "DYLD_LIBRARY_PATH"):
    env.pop(key, None)
qt_root = env.get("QT_ROOT_DIR", "")
if qt_root:
    env["PATH"] = os.pathsep.join(part for part in env.get("PATH", "").split(os.pathsep)
                                  if Path(qt_root).resolve() not in [Path(part).resolve(), *Path(part).resolve().parents])
env["QT_QPA_PLATFORM"] = "offscreen"
subprocess.run([str(executable), "--smoke-test"], env=env, check=True, timeout=25)
