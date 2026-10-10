import hashlib
import tempfile
import unittest
import zipfile
from pathlib import Path

from tools.jieli_build.errors import BuildError
from tools.jieli_build.portable_toolchain import prepare_bundled_windows_toolchain


RESOURCE_DIRS = (
    "include",
    "lib",
    "libc",
    "pi32v2-include",
    "pi32v2-lib",
    "q32s-include",
    "q32s-lib",
)
TOOL_FILES = (
    "clang.exe",
    "lto-wrapper.exe",
    "lto-ar.exe",
    "objdump.exe",
    "objsizedump.exe",
    "llvm-ar.exe",
)


class BundledToolchainTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.archive = self.root / "pi32-2.5.2.zip"
        self.target = (
            self.root / ".tools" / "portable-jieli-windows" / "pi32"
        )

    def _write_archive(self, entries=None):
        if entries is None:
            entries = {
                f"bin/{name}": b"tool" for name in TOOL_FILES
            } | {f"{directory}/resource.dat": b"resource" for directory in RESOURCE_DIRS}
        with zipfile.ZipFile(self.archive, "w", zipfile.ZIP_DEFLATED) as bundle:
            for name, contents in entries.items():
                bundle.writestr(name, contents)
        return hashlib.sha256(self.archive.read_bytes()).hexdigest()

    def _prepare(self, expected_sha256):
        return prepare_bundled_windows_toolchain(
            module_root=self.root,
            archive_path=self.archive,
            target_dir=self.target,
            expected_sha256=expected_sha256,
        )

    def test_extracts_verified_archive_to_canonical_tool_tree(self):
        digest = self._write_archive()

        result = self._prepare(digest)

        self.assertEqual(result, self.target / "bin")
        self.assertTrue((result / "clang.exe").is_file())
        for directory in RESOURCE_DIRS:
            self.assertTrue((self.target / directory / "resource.dat").is_file())

    def test_reuses_valid_cached_tool_tree_without_archive(self):
        digest = self._write_archive()
        self._prepare(digest)
        self.archive.unlink()

        result = self._prepare(digest)

        self.assertEqual(result, self.target / "bin")

    def test_default_archive_is_resolved_from_module_root(self):
        digest = self._write_archive()
        bundled = self.root / "tools" / "toolchains" / "windows" / "pi32-2.5.2.zip"
        bundled.parent.mkdir(parents=True)
        self.archive.replace(bundled)

        result = prepare_bundled_windows_toolchain(
            module_root=self.root,
            target_dir=self.target,
            expected_sha256=digest,
        )

        self.assertEqual(result, self.target / "bin")

    def test_rejects_bad_hash_without_creating_target(self):
        self._write_archive()

        with self.assertRaisesRegex(BuildError, "checksum mismatch"):
            self._prepare("0" * 64)

        self.assertFalse(self.target.exists())

    def test_rejects_zip_path_traversal_without_writing_outside_target(self):
        digest = self._write_archive({"../outside.txt": b"escape"})

        with self.assertRaisesRegex(BuildError, "unsafe archive path"):
            self._prepare(digest)

        self.assertFalse((self.root / "outside.txt").exists())
        self.assertFalse(self.target.exists())

    def test_rejects_windows_absolute_archive_path(self):
        digest = self._write_archive({"C:/outside.txt": b"escape"})

        with self.assertRaisesRegex(BuildError, "unsafe archive path"):
            self._prepare(digest)

        self.assertFalse(self.target.exists())

    def test_incomplete_archive_leaves_existing_directory_untouched(self):
        self.target.mkdir(parents=True)
        marker = self.target / "user-file.txt"
        marker.write_text("preserve", encoding="utf-8")
        digest = self._write_archive({"bin/clang.exe": b"clang"})

        with self.assertRaisesRegex(BuildError, "missing required resources"):
            self._prepare(digest)

        self.assertEqual(marker.read_text(encoding="utf-8"), "preserve")
        self.assertFalse((self.target / "bin" / "clang.exe").exists())


if __name__ == "__main__":
    unittest.main()
