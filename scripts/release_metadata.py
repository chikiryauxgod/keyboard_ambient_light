"""Resolve a user-supplied release tag to one immutable commit. No remote writes."""
import argparse
import json
import re
import subprocess
from pathlib import Path


def git(*args, cwd=None):
    return subprocess.run(["git", *args], cwd=cwd, text=True, capture_output=True, check=True).stdout.strip()


def validate_tag(tag):
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._/-]{0,127}", tag):
        raise ValueError("Use a tag of 1–128 ASCII letters, digits, '.', '_', '-', '/' (e.g. v1.2.3)")
    subprocess.run(["git", "check-ref-format", f"refs/tags/{tag}"], check=True, capture_output=True)
    return tag


def resolve(tag, fallback="HEAD", cwd=None):
    validate_tag(tag)
    ref = f"refs/tags/{tag}"
    exists = subprocess.run(["git", "show-ref", "--verify", "--quiet", ref], cwd=cwd).returncode == 0
    commit = git("rev-parse", "--verify", f"{ref if exists else fallback}^{{commit}}", cwd=cwd)
    return {"tag": tag, "commit": commit, "existing_tag": str(exists).lower()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--fallback", default="HEAD")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    metadata = resolve(args.tag, args.fallback)
    if args.output:
        with args.output.open("a", encoding="utf-8") as file:
            for key, value in metadata.items():
                file.write(f"{key}={value}\n")
    print(json.dumps(metadata))


if __name__ == "__main__":
    main()
