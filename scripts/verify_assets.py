"""Verify the complete desktop release set before publishing anything."""
import argparse
import hashlib
import json
import tarfile
import zipfile
from pathlib import Path


def manifest(path):
    if path.name.endswith(".zip"):
        with zipfile.ZipFile(path) as archive:
            names = [name for name in archive.namelist() if name.endswith("/BUILD-INFO.json") and len(Path(name).parts) == 2]
            if len(names) != 1:
                raise ValueError("Missing or ambiguous build manifest")
            return json.loads(archive.read(names[0]))
    with tarfile.open(path) as archive:
        members = [member for member in archive.getmembers() if member.name.endswith("/BUILD-INFO.json") and len(Path(member.name).parts) == 2]
        if len(members) != 1 or not members[0].isfile():
            raise ValueError("Missing or ambiguous build manifest")
        with archive.extractfile(members[0]) as file:
            return json.load(file)


def verify(directory, expected_tag=None, expected_commit=None):
    directory = Path(directory)
    archives = sorted([*directory.glob("*.tar.gz"), *directory.glob("*.zip")])
    expected = {"linux-x86_64.tar.gz", "windows-x86_64.zip", "macos-arm64.tar.gz"}
    actual = {next((suffix for suffix in expected if path.name.endswith("-" + suffix)), "unknown") for path in archives}
    if len(archives) != 3 or actual != expected:
        raise ValueError("Expected exactly Linux x86_64, Windows x86_64 and macOS arm64 archives")
    lines = []
    source = None
    for path in archives:
        with path.open("rb") as file:
            digest = hashlib.file_digest(file, "sha256").hexdigest()
        line = f"{digest}  {path.name}\n"
        if path.with_name(path.name + ".sha256").read_text(encoding="utf-8") != line:
            raise ValueError(f"Checksum mismatch: {path.name}")
        info = manifest(path)
        identity = (info["tag"], info["commit"])
        if source is not None and source != identity:
            raise ValueError("Release assets have different source tags or commits")
        if expected_tag is not None and info["tag"] != expected_tag:
            raise ValueError("Artifact tag does not match requested release")
        if expected_commit is not None and info["commit"] != expected_commit:
            raise ValueError("Artifact commit does not match built source")
        if f"{info['platform']}.{'zip' if info['platform'].startswith('windows') else 'tar.gz'}" not in actual:
            raise ValueError("Artifact platform metadata does not match release set")
        if not path.name.endswith("-" + info["platform"] + (".zip" if info["platform"].startswith("windows") else ".tar.gz")):
            raise ValueError("Artifact filename does not match platform metadata")
        source = identity
        lines.append(line)
    (directory / "SHA256SUMS").write_text("".join(lines), encoding="utf-8")
    return archives


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--tag")
    parser.add_argument("--commit")
    args = parser.parse_args()
    verify(args.directory, args.tag, args.commit)
