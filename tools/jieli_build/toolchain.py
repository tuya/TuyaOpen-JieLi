"""Jieli pi32v2 toolchain discovery and Windows installer handling."""

from __future__ import annotations

import hashlib
import os
import urllib.error
import urllib.request
from pathlib import Path
from typing import Mapping, Optional

from .chip_profiles import JIELI_CHIPS, resolve_chip
from .errors import BuildError

PLATFORM_ROOT = Path(__file__).resolve().parents[2]
WINDOWS_TOOLCHAIN_INSTALLER_URL = "https://jl-update.oss-cn-shenzhen.aliyuncs.com/2.5.2.exe"
WINDOWS_TOOLCHAIN_INSTALLER_NAME = "jieli-windows-toolchains-2.5.2.exe"
# Pinned digest for the official 2.5.2 installer. Jieli does not publish a
# digest, so this detects later CDN corruption or substitution, but cannot
# independently authenticate the first download.
WINDOWS_TOOLCHAIN_INSTALLER_SHA256 = (
    "1ec78e3315a5987d4e82ecd002536c84240e5c832b1875beff7ce55450124ee5"
)

TOOL_ALIASES = {
    "clang": ("clang", "clang.exe"),
    "lto-wrapper": (
        "lto-wrapper",
        "lto-wrapper.exe",
        "pi32v2-lto-wrapper",
        "pi32v2-lto-wrapper.exe",
    ),
    "lto-ar": ("lto-ar", "lto-ar.exe", "pi32v2-lto-ar", "pi32v2-lto-ar.exe"),
    "objcopy": ("objcopy", "objcopy.exe", "llvm-objcopy", "llvm-objcopy.exe"),
    "objdump": ("objdump", "objdump.exe", "llvm-objdump", "llvm-objdump.exe"),
    "objsizedump": (
        "objsizedump",
        "objsizedump.exe",
        "llvm-objsizedump",
        "llvm-objsizedump.exe",
    ),
}

REQUIRED_TOOLCHAIN_TOOLS = (
    "clang",
    "lto-wrapper",
    "lto-ar",
    "objdump",
    "objsizedump",
)


def resolve_tool_path(tool_dir: Path, logical_name: str) -> Path:
    for name in TOOL_ALIASES[logical_name]:
        candidate = tool_dir / name
        if candidate.is_file():
            return candidate
    return tool_dir / TOOL_ALIASES[logical_name][0]

def resolve_tool_dir(
    sdk_root: Path,
    environ: Optional[Mapping[str, str]] = None,
    module_root: Path = PLATFORM_ROOT,
) -> Path:
    env = os.environ if environ is None else environ
    if "JIELI_TOOL_DIR" in env:
        configured = env["JIELI_TOOL_DIR"].strip()
        if not configured:
            raise BuildError("JIELI_TOOL_DIR is set but empty; unset it or set pi32v2/bin")
        explicit = Path(configured).expanduser()
        missing = [
            name
            for name in REQUIRED_TOOLCHAIN_TOOLS
            if not resolve_tool_path(explicit, name).is_file()
        ]
        if missing:
            raise BuildError(
                f"JIELI_TOOL_DIR is set to {explicit} but does not contain the "
                f"required tools: {', '.join(missing)}"
            )
        return explicit

    candidates = []
    if os.name == "nt":
        candidates.append(
            module_root / ".tools" / "portable-jieli-windows" / "pi32" / "bin"
        )
        candidates.append(Path("C:/JL/pi32/bin"))

    toolchain_suffix = Path("ipc_ac7916a/toolchain/jieli-linux-toolchains/pi32v2/bin")
    candidates.extend(
        (base / toolchain_suffix).resolve()
        for base in (sdk_root.parent, *module_root.parents)
    )
    if os.name != "nt":
        candidates.append(Path("/opt/jieli/pi32v2/bin"))

    for candidate in candidates:
        if candidate.is_dir() and all(
            resolve_tool_path(candidate, name).is_file()
            for name in REQUIRED_TOOLCHAIN_TOOLS
        ):
            return candidate

    searched = ", ".join(str(path) for path in candidates)
    if os.name == "nt":
        hint = (
            "or prepare the Windows portable tree at "
            f"{module_root / '.tools' / 'portable-jieli-windows' / 'pi32' / 'bin'}"
        )
    else:
        hint = "or prepare /opt/jieli/pi32v2/bin"
    raise BuildError(
        f"Jieli pi32v2 toolchain not found. Set JIELI_TOOL_DIR to pi32v2/bin {hint}. "
        f"Required tools: {', '.join(REQUIRED_TOOLCHAIN_TOOLS)}. Searched: {searched}"
    )


def _sha256_of(path: Path) -> str:
    """Compute SHA-256 in chunks so the installer is not loaded at once."""
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _verify_installer(path: Path) -> None:
    """Reject installers that do not match the pinned digest."""
    try:
        actual = _sha256_of(path)
    except OSError as exc:
        raise BuildError(f"cannot read the toolchain installer: {exc}") from exc
    if actual != WINDOWS_TOOLCHAIN_INSTALLER_SHA256:
        raise BuildError(
            "toolchain installer checksum mismatch: expected "
            f"{WINDOWS_TOOLCHAIN_INSTALLER_SHA256}, got {actual}"
        )


def download_windows_toolchain_installer(
    module_root: Path = PLATFORM_ROOT,
) -> Path:
    """Download the official Windows installer when the compiler is missing."""
    if os.name != "nt":
        raise BuildError("automatic Jieli toolchain download is only available on Windows")

    download_dir = module_root / ".tools"
    installer = download_dir / WINDOWS_TOOLCHAIN_INSTALLER_NAME
    partial = installer.with_suffix(installer.suffix + ".part")
    download_dir.mkdir(parents=True, exist_ok=True)

    if installer.is_file():
        try:
            _verify_installer(installer)
            return installer
        except BuildError as exc:
            print(f"[JIELI] Discarding the cached installer: {exc}")
            try:
                installer.unlink(missing_ok=True)
            except OSError as unlink_exc:
                raise BuildError(
                    f"cannot discard the cached toolchain installer {installer}: {unlink_exc}"
                ) from unlink_exc

    request = urllib.request.Request(
        WINDOWS_TOOLCHAIN_INSTALLER_URL,
        headers={"User-Agent": "TuyaOpen-JieLi-build/1.0"},
    )
    print(
        "[JIELI] Downloading Windows toolchain installer from "
        f"{WINDOWS_TOOLCHAIN_INSTALLER_URL}"
    )
    downloaded = 0
    try:
        with urllib.request.urlopen(request, timeout=30) as response, partial.open("wb") as output:
            try:
                content_length = int(response.headers.get("Content-Length", "0") or "0")
            except ValueError as exc:
                raise BuildError("toolchain installer response has an invalid Content-Length") from exc
            while True:
                chunk = response.read(1024 * 1024)
                if not chunk:
                    break
                output.write(chunk)
                downloaded += len(chunk)
                if content_length:
                    print(
                        f"[JIELI] Downloaded {downloaded / (1024 * 1024):.1f} / "
                        f"{content_length / (1024 * 1024):.1f} MiB",
                        end="\r",
                        flush=True,
                    )
        if content_length and downloaded != content_length:
            raise BuildError(
                f"incomplete toolchain installer download ({downloaded} of {content_length} bytes)"
            )
        if partial.stat().st_size < 1024:
            raise BuildError("downloaded toolchain installer is implausibly small")
        _verify_installer(partial)
        partial.replace(installer)
    except (urllib.error.URLError, OSError, TimeoutError, BuildError) as exc:
        partial.unlink(missing_ok=True)
        if isinstance(exc, BuildError):
            raise
        raise BuildError(f"failed to download Jieli toolchain installer: {exc}") from exc

    print()
    return installer
