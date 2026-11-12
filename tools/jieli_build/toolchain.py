"""Jieli pi32v2 toolchain discovery and automatic installation."""

from __future__ import annotations

import hashlib
import os
import shutil
import sys
import tarfile
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

# https://pkgman.jieliapp.com/s/linux-toolchain redirects here. The versioned
# URL is pinned so a build does not silently change compilers; the Linux SDK
# release it came from is the same one toolchain_file.cmake and
# jieli_ranlib.sh already look for.
LINUX_TOOLCHAIN_VERSION = "jieli-linux-toolchains-20260730.1"
LINUX_TOOLCHAIN_URL = (
    f"https://jl-update.oss-cn-shenzhen.aliyuncs.com/{LINUX_TOOLCHAIN_VERSION}.tar.xz"
)
LINUX_TOOLCHAIN_NAME = f"{LINUX_TOOLCHAIN_VERSION}.tar.xz"
LINUX_TOOLCHAIN_SIZE = 26027508
LINUX_TOOLCHAIN_SHA256 = (
    "a1545cddbc451a06bac43eb5cd96d831470c42a97c5c0b1f98879a20fc7647bd"
)
LINUX_TOOLCHAIN_CACHE_DIR = "portable-jieli-linux"
LINUX_TOOLCHAIN_OFFLINE_ENV = "TOS_JIELI_NO_DOWNLOAD"

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

    if sys.platform.startswith("linux"):
        candidates.append(linux_toolchain_bin_dir(module_root))
        # Same layout toolchain_file.cmake and jieli_ranlib.sh already accept,
        # so a tree prepared by hand is used before anything is downloaded.
        for base in (module_root, module_root.parent, *module_root.parents):
            for version in (LINUX_TOOLCHAIN_VERSION, None):
                parts = [base, "tools", "jieli-toolchain"]
                if version is not None:
                    parts.append(version)
                parts.extend(("pi32v2", "bin"))
                candidates.append(Path(*parts))

    toolchain_suffix = Path("ipc_ac7916a/toolchain/jieli-linux-toolchains/pi32v2/bin")
    candidates.extend(
        (base / toolchain_suffix).resolve()
        for base in (sdk_root.parent, *module_root.parents)
    )
    if os.name != "nt":
        candidates.append(Path("/opt/jieli/pi32v2/bin"))

    seen = set()
    for candidate in candidates:
        key = str(candidate)
        if key in seen:
            continue
        seen.add(key)
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
        hint = (
            "or let the build download it to "
            f"{linux_toolchain_bin_dir(module_root)} "
            f"(set {LINUX_TOOLCHAIN_OFFLINE_ENV}=1 to skip the download)"
        )
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


def linux_toolchain_bin_dir(module_root: Path = PLATFORM_ROOT) -> Path:
    """Where install_linux_toolchain() puts the compiler it downloads."""
    return (
        module_root
        / ".tools"
        / LINUX_TOOLCHAIN_CACHE_DIR
        / LINUX_TOOLCHAIN_VERSION
        / "pi32v2"
        / "bin"
    )


def linux_toolchain_cache_root(module_root: Path = PLATFORM_ROOT) -> Path:
    return module_root / ".tools" / LINUX_TOOLCHAIN_CACHE_DIR


def _missing_tools(tool_dir: Path) -> list[str]:
    if not tool_dir.is_dir():
        return list(REQUIRED_TOOLCHAIN_TOOLS)
    return [
        name
        for name in REQUIRED_TOOLCHAIN_TOOLS
        if not resolve_tool_path(tool_dir, name).is_file()
    ]


def _verify_linux_archive(path: Path) -> None:
    try:
        size = path.stat().st_size
    except OSError as exc:
        raise BuildError(f"cannot read the Jieli Linux toolchain archive: {exc}") from exc
    if size != LINUX_TOOLCHAIN_SIZE:
        raise BuildError(
            f"Jieli Linux toolchain archive is {size} bytes, expected "
            f"{LINUX_TOOLCHAIN_SIZE}"
        )
    actual = _sha256_of(path)
    if actual != LINUX_TOOLCHAIN_SHA256:
        raise BuildError(
            "Jieli Linux toolchain checksum mismatch: expected "
            f"{LINUX_TOOLCHAIN_SHA256}, got {actual}"
        )


def download_linux_toolchain(module_root: Path = PLATFORM_ROOT) -> Path:
    """Download the pinned Linux toolchain archive, reusing a verified cache."""
    if not sys.platform.startswith("linux"):
        raise BuildError("the Linux toolchain is only used on a Linux host")

    download_dir = module_root / ".tools"
    archive = download_dir / LINUX_TOOLCHAIN_NAME
    # A per-process name keeps two concurrent builds from writing the same file.
    partial = archive.with_suffix(archive.suffix + f".{os.getpid()}.part")
    download_dir.mkdir(parents=True, exist_ok=True)

    if archive.is_file():
        try:
            _verify_linux_archive(archive)
            return archive
        except BuildError as exc:
            print(f"[JIELI] Discarding the cached Linux toolchain: {exc}")
            archive.unlink(missing_ok=True)

    request = urllib.request.Request(
        LINUX_TOOLCHAIN_URL,
        headers={"User-Agent": "TuyaOpen-JieLi-build/1.0"},
    )
    print(f"[JIELI] Downloading Linux toolchain from {LINUX_TOOLCHAIN_URL}")
    downloaded = 0
    try:
        with urllib.request.urlopen(request, timeout=30) as response, partial.open("wb") as output:
            try:
                content_length = int(response.headers.get("Content-Length", "0") or "0")
            except ValueError as exc:
                raise BuildError(
                    "toolchain response has an invalid Content-Length"
                ) from exc
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
        print()
        if downloaded != LINUX_TOOLCHAIN_SIZE:
            raise BuildError(
                f"incomplete toolchain download ({downloaded} of "
                f"{LINUX_TOOLCHAIN_SIZE} bytes)"
            )
        _verify_linux_archive(partial)
        partial.replace(archive)
    except (urllib.error.URLError, OSError, TimeoutError, BuildError) as exc:
        partial.unlink(missing_ok=True)
        if isinstance(exc, BuildError):
            raise
        raise BuildError(f"failed to download the Jieli Linux toolchain: {exc}") from exc

    return archive


def _extract_linux_toolchain(archive: Path, cache_root: Path) -> None:
    """Unpack <cache_root>/<version> from the vendor archive, atomically."""
    tmp_root = cache_root / f".extract-{os.getpid()}"
    if tmp_root.exists():
        shutil.rmtree(tmp_root, ignore_errors=True)
    cache_root.mkdir(parents=True, exist_ok=True)

    try:
        with tarfile.open(archive, mode="r:xz") as tar:
            try:
                tar.extractall(tmp_root, filter="data")
            except TypeError:
                # Python < 3.12 has no extraction filters.
                tar.extractall(tmp_root)
    except (tarfile.TarError, OSError) as exc:
        shutil.rmtree(tmp_root, ignore_errors=True)
        try:
            archive.unlink(missing_ok=True)
        except OSError:
            pass
        raise BuildError(f"cannot unpack the Jieli Linux toolchain: {exc}") from exc

    source = tmp_root / LINUX_TOOLCHAIN_VERSION
    if not source.is_dir():
        shutil.rmtree(tmp_root, ignore_errors=True)
        raise BuildError(
            f"the Linux toolchain archive has no {LINUX_TOOLCHAIN_VERSION} directory"
        )

    target = cache_root / LINUX_TOOLCHAIN_VERSION
    if target.exists():
        shutil.rmtree(target, ignore_errors=True)
    try:
        try:
            os.replace(source, target)
        except OSError:
            # A concurrent build may have installed the same version first.
            if _missing_tools(target / "pi32v2" / "bin"):
                raise
    finally:
        shutil.rmtree(tmp_root, ignore_errors=True)

    for name in REQUIRED_TOOLCHAIN_TOOLS:
        tool = resolve_tool_path(target / "pi32v2" / "bin", name)
        if tool.is_file():
            tool.chmod(tool.stat().st_mode | 0o111)


def install_linux_toolchain(module_root: Path = PLATFORM_ROOT) -> Path:
    """Make sure the pinned Linux toolchain is available and return its bin dir."""
    if not sys.platform.startswith("linux"):
        raise BuildError("the Linux toolchain is only used on a Linux host")

    tool_dir = linux_toolchain_bin_dir(module_root)
    missing = _missing_tools(tool_dir)
    if not missing:
        return tool_dir

    if os.environ.get(LINUX_TOOLCHAIN_OFFLINE_ENV, "").strip().lower() not in (
        "",
        "0",
        "n",
        "no",
        "false",
    ):
        raise BuildError(
            f"Jieli Linux toolchain is missing ({', '.join(missing)}) and "
            f"{LINUX_TOOLCHAIN_OFFLINE_ENV} is set. Install it at {tool_dir} or "
            "set JIELI_TOOL_DIR."
        )

    archive = download_linux_toolchain(module_root)
    _extract_linux_toolchain(archive, linux_toolchain_cache_root(module_root))

    missing = _missing_tools(linux_toolchain_bin_dir(module_root))
    if missing:
        raise BuildError(
            "the downloaded Linux toolchain is incomplete, missing: "
            f"{', '.join(missing)}"
        )
    print(f"[JIELI] Linux toolchain ready: {linux_toolchain_bin_dir(module_root)}")
    return linux_toolchain_bin_dir(module_root)
