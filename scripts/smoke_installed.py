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
if executable.suffix == ".exe":
    for name in ("Qt6Core.dll", "hidapi.dll", "msvcp140.dll", "vcruntime140.dll"):
        if not (executable.parent / name).is_file():
            raise SystemExit(f"Portable Windows runtime is incomplete: {name}")
version_file = stage / "share/keyboard-ambient/VERSION"
if version_file.exists():
    actual = subprocess.check_output([str(executable), "--version"], text=True, timeout=10).strip()
    expected = "Keyboard Ambient " + version_file.read_text().strip()
    if actual != expected:
        raise SystemExit(f"Version mismatch: {actual!r} != {expected!r}")
env = os.environ.copy()
for key in ("QT_PLUGIN_PATH", "QML_IMPORT_PATH", "QML2_IMPORT_PATH", "LD_LIBRARY_PATH", "DYLD_LIBRARY_PATH"):
    env.pop(key, None)
qt_root = env.get("QT_ROOT_DIR", "")
if qt_root:
    env["PATH"] = os.pathsep.join(part for part in env.get("PATH", "").split(os.pathsep)
                                  if Path(qt_root).resolve() not in [Path(part).resolve(), *Path(part).resolve().parents])
env["QT_QPA_PLATFORM"] = "offscreen"
env["QT_FORCE_STDERR_LOGGING"] = "1"
env["QT_LOGGING_TO_CONSOLE"] = "1"
try:
    result = subprocess.run([str(executable), "--smoke-test"], env=env, check=True,
                            timeout=25, capture_output=True, text=True)
except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as error:
    for stream, output in ((sys.stdout, error.stdout), (sys.stderr, error.stderr)):
        if output:
            stream.write(output.decode(errors="replace") if isinstance(output, bytes) else output)
    raise
else:
    sys.stdout.write(result.stdout)
    sys.stderr.write(result.stderr)
