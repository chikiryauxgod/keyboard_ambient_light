import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def module(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "scripts" / f"{name}.py")
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


metadata, packaging, assets = (module(name) for name in ("release_metadata", "package", "verify_assets"))
SHA = "a" * 40


class MetadataTests(unittest.TestCase):
    def test_semver_release_tag_matches_source_version(self):
        for tag in ("0.1.0", "v0.1.0", "v0.1.0-rc.1", "v0.1.0+build.1"):
            metadata.validate_tag(tag)
            metadata.validate_release_version(tag, "0.1.0")
        for tag in ("v0.2.0", "v00.1.0", "v0.1.0-01", "release/foo"):
            with self.subTest(tag=tag), self.assertRaises(ValueError):
                metadata.validate_release_version(tag, "0.1.0")

    def test_current_version_has_changelog_entry(self):
        version = (ROOT / "VERSION").read_text().strip()
        metadata.validate_release_version("v" + version, version)
        self.assertIn(f"## [{version}]", (ROOT / "changelog.md").read_text(encoding="utf-8"))

    def test_invalid_tags(self):
        for value in ("", "-v1", "a..b", "a.lock", "v1\nother=bad", "a b", "$(touch bad)", "a/../b", "a/", "@{bad}"):
            with self.subTest(tag=value), self.assertRaises((ValueError, subprocess.CalledProcessError)):
                metadata.validate_tag(value)

    def test_resolve_new_existing_and_annotated_tags(self):
        with tempfile.TemporaryDirectory() as directory:
            def git(*args):
                return metadata.git(*args, cwd=directory)
            git("init", "-b", "main")
            git("config", "user.name", "Test")
            git("config", "user.email", "test@example.invalid")
            git("commit", "--allow-empty", "-m", "first")
            first = git("rev-parse", "HEAD")
            git("tag", "-a", "v1.0.0", "-m", "annotated")
            git("commit", "--allow-empty", "-m", "second")
            second = git("rev-parse", "HEAD")
            self.assertEqual(metadata.resolve("v1.0.0", cwd=directory)["commit"], first)
            self.assertEqual(metadata.resolve("release/v2", cwd=directory),
                             {"tag": "release/v2", "commit": second, "existing_tag": "false"})
            self.assertEqual(metadata.resolve("v3", first, cwd=directory)["commit"], first)
            with self.assertRaises(subprocess.CalledProcessError):
                metadata.resolve("v4", "missing", cwd=directory)


class PackageTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.stage, self.out = self.root / "stage", self.root / "out"
        (self.stage / "bin").mkdir(parents=True)
        for name in ("keyboard-ambient", "keyboard-ambient.exe"):
            (self.stage / "bin" / name).write_bytes(b"application")
        (self.stage / "bin/keyboard-ambient").chmod(0o755)

    def test_tar_modes_metadata_and_checksum(self):
        path = packaging.package(self.stage, self.out, "release/v1", "linux-x86_64", SHA)
        with tarfile.open(path) as archive:
            executable = next(item for item in archive.getmembers() if item.name.endswith("/bin/keyboard-ambient"))
            self.assertTrue(executable.mode & 0o111)
            info = next(item for item in archive.getmembers() if item.name.endswith("/BUILD-INFO.json"))
            self.assertEqual(json.load(archive.extractfile(info))["commit"], SHA)
            self.assertTrue(all(not item.name.startswith("/") and ".." not in Path(item.name).parts for item in archive.getmembers()))
        self.assertTrue(path.name.startswith("KeyboardAmbient-release-v1-"))
        with path.open("rb") as file:
            digest = hashlib.file_digest(file, "sha256").hexdigest()
        self.assertEqual(path.with_name(path.name + ".sha256").read_text(), f"{digest}  {path.name}\n")
        with self.assertRaises(FileExistsError):
            packaging.package(self.stage, self.out, "release/v1", "linux-x86_64", SHA)

    def test_unix_archive_entry_point_is_executable_without_host_execute_bits(self):
        (self.stage / "bin/keyboard-ambient").chmod(0o644)
        path = packaging.package(self.stage, self.out, "v0.1.0", "linux-x86_64", SHA)
        with tarfile.open(path) as archive:
            executable = next(item for item in archive.getmembers() if item.name.endswith("/bin/keyboard-ambient"))
            self.assertEqual(executable.mode, 0o755)

    def test_windows_zip(self):
        path = packaging.package(self.stage, self.out, "v1", "windows-x86_64", SHA)
        with zipfile.ZipFile(path) as archive:
            self.assertIsNone(archive.testzip())
            self.assertTrue(any(name.endswith("/bin/keyboard-ambient.exe") for name in archive.namelist()))

    def test_portable_windows_package_rejects_runtime_installer(self):
        (self.stage / "bin/VC_redist.x64.exe").write_bytes(b"installer")
        with self.assertRaises(ValueError):
            packaging.package(self.stage, self.out, "v0.1.0", "windows-x86_64", SHA)

    @unittest.skipIf(os.name == "nt", "Creating symlinks requires privileges on Windows")
    def test_framework_symlinks(self):
        executable = self.stage / "keyboard-ambient.app/Contents/MacOS/keyboard-ambient"
        executable.parent.mkdir(parents=True)
        executable.write_bytes(b"mac binary")
        (self.stage / "current").symlink_to("keyboard-ambient.app", target_is_directory=True)
        path = packaging.package(self.stage, self.out, "v1", "macos-arm64", SHA)
        with tarfile.open(path) as archive:
            self.assertTrue(next(item for item in archive.getmembers() if item.name.endswith("/current")).issym())

    def test_reject_incomplete_or_unsafe_packages(self):
        for version, platform, commit in (("../bad", "linux-x86_64", SHA), ("v1", "other", SHA), ("v1", "linux-x86_64", "HEAD")):
            with self.assertRaises(ValueError):
                packaging.package(self.stage, self.out, version, platform, commit)
        with self.assertRaises(ValueError):
            packaging.package(self.stage, self.stage / "inside", "v1", "linux-x86_64", SHA)
        with self.assertRaises(ValueError):
            packaging.package(self.stage, self.out, "v1", "macos-arm64", SHA)

    def test_complete_release_and_corruption(self):
        for platform in ("linux-x86_64", "windows-x86_64", "macos-arm64"):
            mac = self.stage / "keyboard-ambient.app/Contents/MacOS/keyboard-ambient"
            mac.parent.mkdir(parents=True, exist_ok=True)
            mac.write_bytes(b"mac binary")
            packaging.package(self.stage, self.out, "v1", platform, SHA)
        self.assertEqual(len(assets.verify(self.out)), 3)
        with self.assertRaises(ValueError):
            assets.verify(self.out, "another-tag", SHA)
        with self.assertRaises(ValueError):
            assets.verify(self.out, "v1", "b" * 40)
        self.assertEqual(len((self.out / "SHA256SUMS").read_text().splitlines()), 3)
        next(self.out.glob("*.zip")).write_bytes(b"corrupt")
        with self.assertRaises(ValueError):
            assets.verify(self.out)

    def test_partial_release_is_rejected(self):
        packaging.package(self.stage, self.out, "v1", "linux-x86_64", SHA)
        with self.assertRaises(ValueError):
            assets.verify(self.out)

    def test_mixed_source_commits_are_rejected(self):
        mac = self.stage / "keyboard-ambient.app/Contents/MacOS/keyboard-ambient"
        mac.parent.mkdir(parents=True)
        mac.write_bytes(b"mac binary")
        for platform in ("linux-x86_64", "windows-x86_64", "macos-arm64"):
            commit = "b" * 40 if platform.startswith("windows") else SHA
            packaging.package(self.stage, self.out, "v1", platform, commit)
        with self.assertRaises(ValueError):
            assets.verify(self.out)


if __name__ == "__main__":
    unittest.main()
