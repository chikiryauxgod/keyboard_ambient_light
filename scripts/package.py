"""Archive an installed desktop tree, preserving Unix modes and framework symlinks."""
import argparse
import hashlib
import json
import re
import tarfile
import zipfile
from pathlib import Path


def package(stage, output, version, platform, commit):
    stage, output = Path(stage).resolve(), Path(output).resolve()
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._/+-]{0,127}", version):
        raise ValueError("Invalid artifact version")
    if platform not in {"linux-x86_64", "windows-x86_64", "macos-arm64"}:
        raise ValueError("Unknown target platform")
    if not re.fullmatch(r"[a-fA-F0-9]{40}", commit):
        raise ValueError("A full source commit SHA is required")
    executable = {"linux-x86_64": "bin/keyboard-ambient", "windows-x86_64": "bin/keyboard-ambient.exe",
                  "macos-arm64": "keyboard-ambient.app/Contents/MacOS/keyboard-ambient"}[platform]
    if not (stage / executable).is_file():
        raise ValueError(f"Missing installed executable: {executable}")
    if output == stage or stage in output.parents:
        raise ValueError("Output directory must be outside the installed tree")
    if list(stage.rglob("VC_redist*.exe")):
        raise ValueError("Portable archives must bundle runtime DLLs, not a redistributable installer")
    output.mkdir(parents=True, exist_ok=True)
    (stage / "BUILD-INFO.json").write_text(json.dumps({"tag": version, "commit": commit, "platform": platform}, indent=2) + "\n", encoding="utf-8")
    launch = {"linux-x86_64": "bin/keyboard-ambient", "windows-x86_64": "bin/keyboard-ambient.exe",
              "macos-arm64": "keyboard-ambient.app"}[platform]
    (stage / "RUNNING.txt").write_text(f"Keyboard Ambient\n\nExtract the whole archive and run {launch}.\nNo installer, system service or automatic startup is required.\n", encoding="utf-8")
    name = f"KeyboardAmbient-{version.replace('/', '-')}-{platform}"
    archive = output / (name + (".zip" if platform.startswith("windows") else ".tar.gz"))
    if archive.exists():
        raise FileExistsError(f"Refusing to overwrite {archive}")
    if platform.startswith("windows"):
        with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as file:
            for path in sorted(stage.rglob("*")):
                file.write(path, Path(name) / path.relative_to(stage))
    else:
        with tarfile.open(archive, "w:gz", dereference=False) as file:
            file.add(stage, arcname=name)
    with archive.open("rb") as file:
        digest = hashlib.file_digest(file, "sha256").hexdigest()
    archive.with_name(archive.name + ".sha256").write_text(f"{digest}  {archive.name}\n", encoding="utf-8")
    return archive


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("stage", "output", "version", "platform", "commit"):
        parser.add_argument("--" + name, required=True)
    args = parser.parse_args()
    print(package(args.stage, args.output, args.version, args.platform, args.commit))


if __name__ == "__main__":
    main()
