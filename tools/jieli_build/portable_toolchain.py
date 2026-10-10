"""Prepare the platform's pinned Windows pi32 toolchain archive."""

from __future__ import annotations

import hashlib
import os
import shutil
import stat
import tempfile
import zipfile
from pathlib import Path, PurePosixPath
from typing import Optional

from .errors import BuildError
from .toolchain import REQUIRED_TOOLCHAIN_TOOLS, resolve_tool_path

PLATFORM_ROOT = Path(__file__).resolve().parents[2]
WINDOWS_TOOLCHAIN_ARCHIVE_SHA256 = (
    "9eba002ad13c43ca11b5866798ecc267f46b98297471fed4b5d0a7734ee75d27"
)
WINDOWS_TOOLCHAIN_ROOTS = (
    "bin",
    "include",
    "lib",
    "libc",
    "pi32v2-include",
    "pi32v2-lib",
    "q32s-include",
    "q32s-lib",
)
WINDOWS_TOOLCHAIN_REQUIRED_TOOLS = (*REQUIRED_TOOLCHAIN_TOOLS, "llvm-ar")


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _missing_contents(root: Path) -> list[str]:
    missing = []
    for name in WINDOWS_TOOLCHAIN_ROOTS:
        resource_root = root / name
        if not resource_root.is_dir() or not any(
            item.is_file() for item in resource_root.rglob("*")
        ):
            missing.append(name)
    bin_dir = root / "bin"
    missing.extend(
        f"bin/{name}"
        for name in WINDOWS_TOOLCHAIN_REQUIRED_TOOLS
        if not (
            bin_dir / "llvm-ar.exe"
            if name == "llvm-ar"
            else resolve_tool_path(bin_dir, name)
        ).is_file()
    )
    return missing


def _safe_member_path(name: str, destination: Path) -> Path:
    if not name or "\\" in name:
        raise BuildError(f"unsafe archive path: {name!r}")
    member = PurePosixPath(name)
    if member.is_absolute() or any(part in ("", ".", "..") for part in member.parts):
        raise BuildError(f"unsafe archive path: {name!r}")
    if any(":" in part for part in member.parts):
        raise BuildError(f"unsafe archive path: {name!r}")
    resolved = destination.joinpath(*member.parts).resolve()
    try:
        resolved.relative_to(destination.resolve())
    except ValueError as exc:
        raise BuildError(f"unsafe archive path: {name!r}") from exc
    return resolved


def _extract_checked(archive: Path, destination: Path) -> None:
    seen: set[str] = set()
    try:
        with zipfile.ZipFile(archive) as bundle:
            for info in bundle.infolist():
                member_path = _safe_member_path(info.filename, destination)
                normalized = member_path.relative_to(destination.resolve()).as_posix()
                if normalized in seen:
                    raise BuildError(f"duplicate archive path: {info.filename!r}")
                seen.add(normalized)
                mode = info.external_attr >> 16
                if stat.S_ISLNK(mode):
                    raise BuildError(f"symbolic links are not allowed in archive: {info.filename!r}")
                if info.is_dir():
                    member_path.mkdir(parents=True, exist_ok=True)
                    continue
                member_path.parent.mkdir(parents=True, exist_ok=True)
                with bundle.open(info, "r") as source, member_path.open("xb") as target:
                    shutil.copyfileobj(source, target)
    except BuildError:
        raise
    except (OSError, zipfile.BadZipFile, RuntimeError) as exc:
        raise BuildError(f"cannot extract bundled Jieli toolchain archive: {exc}") from exc


def prepare_bundled_windows_toolchain(
    module_root: Path = PLATFORM_ROOT,
    archive_path: Optional[Path] = None,
    target_dir: Optional[Path] = None,
    expected_sha256: str = WINDOWS_TOOLCHAIN_ARCHIVE_SHA256,
) -> Optional[Path]:
    """Extract the bundled pi32 archive if present; return its bin directory."""
    module_root = Path(module_root)
    archive = Path(archive_path) if archive_path is not None else (
        module_root / "tools" / "toolchains" / "windows" / "pi32-2.5.2.zip"
    )
    target = Path(target_dir) if target_dir is not None else (
        module_root / ".tools" / "portable-jieli-windows" / "pi32"
    )
    try:
        target.resolve().relative_to(Path(module_root).resolve())
    except ValueError as exc:
        raise BuildError(f"portable toolchain target must stay inside the platform: {target}") from exc

    if target.is_symlink():
        raise BuildError(f"portable toolchain target is a symbolic link: {target}")
    if target.exists():
        missing = _missing_contents(target)
        if not missing:
            return target / "bin"
        raise BuildError(
            f"portable toolchain directory already exists but is incomplete; "
            f"leaving it unchanged: {target}; missing required resources: "
            f"{', '.join(missing)}"
        )
    if not archive.is_file():
        return None

    try:
        actual_sha256 = _sha256(archive)
    except OSError as exc:
        raise BuildError(f"cannot read bundled Jieli toolchain archive: {exc}") from exc
    if actual_sha256.lower() != expected_sha256.lower():
        raise BuildError(
            "bundled Jieli toolchain archive checksum mismatch: expected "
            f"{expected_sha256}, got {actual_sha256}"
        )

    try:
        target.parent.mkdir(parents=True, exist_ok=True)
        staging = Path(tempfile.mkdtemp(prefix=".pi32-extract-", dir=target.parent))
    except OSError as exc:
        raise BuildError(f"cannot create toolchain extraction directory: {exc}") from exc
    try:
        _extract_checked(archive, staging)
        missing = _missing_contents(staging)
        if missing:
            raise BuildError(
                "bundled Jieli toolchain archive is incomplete; missing required "
                f"resources: {', '.join(missing)}"
            )
        if target.exists() or target.is_symlink():
            raise BuildError(
                f"portable toolchain target appeared during extraction; "
                f"leaving it unchanged: {target}"
            )
        os.rename(staging, target)
        return target / "bin"
    except OSError as exc:
        raise BuildError(f"cannot prepare bundled Jieli toolchain: {exc}") from exc
    finally:
        if staging.exists():
            shutil.rmtree(staging, ignore_errors=True)
