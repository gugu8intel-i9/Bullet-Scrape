"""
Build the native shared library in-place.

Used by:
  • `pip install` (setup.py build_ext hook)
  • Colab setup cell: `python -m bullet_scrape.build_native`
  • CI

Strategy
--------
1. Prefer an already-built .so next to this package or in the repo root.
2. Otherwise compile sources with g++ -O3 -shared, auto-detecting libcurl.
3. Stage the .so into python/bullet_scrape/ so ctypes can find it.
"""

from __future__ import annotations

import os
import sys
import shutil
import subprocess
import tempfile
from pathlib import Path


PACKAGE_DIR = Path(__file__).resolve().parent
# python/bullet_scrape → python → repo
REPO_ROOT = PACKAGE_DIR.parent.parent
if not (REPO_ROOT / "src" / "core").is_dir():
    # Installed wheel layout: sources may be bundled under package
    REPO_ROOT = PACKAGE_DIR


SOURCES = [
    "src/core/config.cpp",
    "src/core/http_client.cpp",
    "src/core/extractor.cpp",
    "src/core/output.cpp",
    "src/core/scraper.cpp",
    "src/core/c_api.cpp",
]


def _so_name() -> str:
    if sys.platform == "darwin":
        return "libbullet_scrape.dylib"
    if sys.platform == "win32":
        return "bullet_scrape.dll"
    return "libbullet_scrape.so"


def find_existing() -> Path | None:
    name = _so_name()
    candidates = [
        PACKAGE_DIR / name,
        REPO_ROOT / name,
        REPO_ROOT / "build" / name,
        Path(os.environ["BULLET_SCRAPE_LIB"]) if os.environ.get("BULLET_SCRAPE_LIB") else None,
    ]
    for c in candidates:
        if c and c.is_file():
            return c
    return None


def _has_curl(cxx: str) -> bool:
    cmd = [cxx, "-x", "c++", "-fsyntax-only", "-"]
    try:
        r = subprocess.run(
            cmd,
            input=b"#include <curl/curl.h>\nint main(){return 0;}\n",
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )
        return r.returncode == 0
    except FileNotFoundError:
        return False


def _detect_cxx() -> str:
    for c in (os.environ.get("CXX"), "g++", "clang++", "c++"):
        if not c:
            continue
        if shutil.which(c):
            return c
    raise RuntimeError("No C++ compiler found (g++ / clang++)")


def build(force: bool = False, jobs: int | None = None) -> Path:
    """
    Build libbullet_scrape and copy it into the package directory.
    Returns the path to the staged shared library.
    """
    name = _so_name()
    staged = PACKAGE_DIR / name

    if not force:
        existing = find_existing()
        if existing is not None:
            if existing.resolve() != staged.resolve():
                shutil.copy2(existing, staged)
                print(f"[bullet_scrape] using existing {existing} → {staged}")
            else:
                print(f"[bullet_scrape] already built: {staged}")
            return staged

    # Prefer Makefile when we're in a full checkout
    makefile = REPO_ROOT / "Makefile"
    if makefile.is_file() and (REPO_ROOT / "src" / "core" / "c_api.cpp").is_file():
        print("[bullet_scrape] building via Makefile (shared)…")
        env = os.environ.copy()
        nproc = jobs or max(1, (os.cpu_count() or 2))
        subprocess.check_call(
            ["make", "-C", str(REPO_ROOT), "shared", f"-j{nproc}"],
            env=env,
        )
        built = REPO_ROOT / name
        if not built.is_file():
            raise RuntimeError(f"make shared did not produce {built}")
        shutil.copy2(built, staged)
        print(f"[bullet_scrape] staged {staged}")
        return staged

    # Fallback: direct g++ invocation (sources next to package)
    cxx = _detect_cxx()
    src_root = REPO_ROOT
    srcs = []
    for rel in SOURCES:
        p = src_root / rel
        if not p.is_file():
            # try under package
            p = PACKAGE_DIR / "native" / Path(rel).name
        if not p.is_file():
            raise FileNotFoundError(f"missing source: {rel} (looked in {src_root})")
        srcs.append(str(p))

    include = src_root / "include"
    if not include.is_dir():
        include = PACKAGE_DIR / "include"

    has_curl = _has_curl(cxx)
    defs = ["-DBULLET_HAVE_CURL", "-DHAVE_CURL"] if has_curl else []
    libs = ["-lcurl"] if has_curl else []
    print(f"[bullet_scrape] compiling with {cxx}  curl={has_curl}")

    out = staged
    cmd = [
        cxx, "-std=c++17", "-O3", "-DNDEBUG",
        "-ffast-math", "-funroll-loops",
        "-fPIC", "-fvisibility=hidden", "-fvisibility-inlines-hidden",
        "-shared",
        f"-I{include}",
        *defs,
        *srcs,
        "-o", str(out),
        "-pthread",
        *libs,
    ]
    if sys.platform == "darwin":
        cmd.extend(["-dynamiclib", f"-install_name", f"@rpath/{name}"])

    print("[bullet_scrape]", " ".join(cmd[:8]), "…")
    subprocess.check_call(cmd)
    print(f"[bullet_scrape] built {out}")
    return out


def ensure_built() -> Path:
    """Idempotent: return path to usable shared library, building if needed."""
    existing = find_existing()
    if existing is not None:
        staged = PACKAGE_DIR / _so_name()
        if existing.resolve() != staged.resolve() and existing.is_file():
            try:
                shutil.copy2(existing, staged)
            except Exception:
                return existing
            return staged
        return existing
    return build(force=True)


def main(argv: list[str] | None = None) -> int:
    import argparse
    p = argparse.ArgumentParser(description="Build Bullet Scrape native library")
    p.add_argument("--force", action="store_true", help="rebuild even if present")
    p.add_argument("-j", "--jobs", type=int, default=None)
    args = p.parse_args(argv)
    path = build(force=args.force, jobs=args.jobs)
    print(path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
