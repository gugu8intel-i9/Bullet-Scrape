"""
ctypes bindings to libbullet_scrape.so / .dylib.

Designed for Google Colab and local use:
  • Auto-locates the shared library next to this package, in the repo root,
    or via BULLET_SCRAPE_LIB env var.
  • Zero-copy-ish JSON handoff (one malloc on the C side, one decode in Python).
  • Thread-safe: each Scraper owns an independent native handle.
"""

from __future__ import annotations

import json
import os
import sys
import ctypes
import ctypes.util
from ctypes import (
    c_char_p,
    c_void_p,
    c_int,
    c_long,
    c_size_t,
    c_int64,
    c_double,
    c_uint,
    c_char,
    POINTER,
    Structure,
    byref,
)
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, List, Optional, Union, Mapping


# ── Exceptions ───────────────────────────────────────────────────────────────

class BulletError(RuntimeError):
    """Raised when the native engine reports an error."""

    def __init__(self, message: str, code: int = 1):
        super().__init__(message)
        self.code = code


# ── ctypes structures (mirror c_api.h) ───────────────────────────────────────

class _BulletStats(Structure):
    _fields_ = [
        ("total_ms", c_int64),
        ("total_bytes", c_int64),
        ("succeeded", c_int),
        ("failed", c_int),
        ("url_count", c_int),
        ("record_count", c_int),
        ("pages_per_sec", c_double),
        ("mb_per_sec", c_double),
    ]


@dataclass
class Stats:
    total_ms: int = 0
    total_bytes: int = 0
    succeeded: int = 0
    failed: int = 0
    url_count: int = 0
    record_count: int = 0
    pages_per_sec: float = 0.0
    mb_per_sec: float = 0.0

    @classmethod
    def from_c(cls, s: _BulletStats) -> "Stats":
        return cls(
            total_ms=int(s.total_ms),
            total_bytes=int(s.total_bytes),
            succeeded=int(s.succeeded),
            failed=int(s.failed),
            url_count=int(s.url_count),
            record_count=int(s.record_count),
            pages_per_sec=float(s.pages_per_sec),
            mb_per_sec=float(s.mb_per_sec),
        )

    def to_dict(self) -> Dict[str, Any]:
        return {
            "total_ms": self.total_ms,
            "total_bytes": self.total_bytes,
            "succeeded": self.succeeded,
            "failed": self.failed,
            "url_count": self.url_count,
            "record_count": self.record_count,
            "pages_per_sec": self.pages_per_sec,
            "mb_per_sec": self.mb_per_sec,
        }


@dataclass
class ScrapeResult:
    """Result of a scrape or offline extract run."""
    records: List[Dict[str, Any]] = field(default_factory=list)
    stats: Stats = field(default_factory=Stats)
    raw_json: str = ""

    def to_dataframe(self):
        """Convert records to a pandas DataFrame (pandas must be installed)."""
        try:
            import pandas as pd
        except ImportError as e:
            raise ImportError(
                "pandas is required for to_dataframe(). "
                "Install with: pip install pandas"
            ) from e
        return pd.DataFrame(self.records)

    def __len__(self) -> int:
        return len(self.records)

    def __iter__(self):
        return iter(self.records)


# ── Shared library loader ────────────────────────────────────────────────────

_lib = None  # type: ignore
_lib_path: Optional[str] = None


def _candidate_paths() -> List[Path]:
    here = Path(__file__).resolve().parent
    repo = here.parent.parent  # python/bullet_scrape → repo root
    names = []
    if sys.platform == "darwin":
        names = ["libbullet_scrape.dylib", "libbullet_scrape.so"]
    elif sys.platform == "win32":
        names = ["bullet_scrape.dll", "libbullet_scrape.dll"]
    else:
        names = ["libbullet_scrape.so", "libbullet_scrape.so.1"]

    paths: List[Path] = []
    env = os.environ.get("BULLET_SCRAPE_LIB")
    if env:
        paths.append(Path(env))

    for n in names:
        paths.append(here / n)
        paths.append(repo / n)
        paths.append(repo / "build" / n)
        paths.append(repo / "build" / "lib" / n)
        paths.append(Path("/usr/local/lib") / n)
        paths.append(Path("/usr/lib") / n)
        # Colab common locations
        paths.append(Path("/content/Bullet-Scrape") / n)
        paths.append(Path("/content/Bullet-Scrape/python/bullet_scrape") / n)
        paths.append(Path("/content") / n)

    return paths


def _load_lib():
    global _lib, _lib_path
    if _lib is not None:
        return _lib

    last_err: Optional[BaseException] = None
    for p in _candidate_paths():
        if not p.is_file():
            continue
        try:
            lib = ctypes.CDLL(str(p))
            _bind(lib)
            _lib = lib
            _lib_path = str(p)
            return _lib
        except OSError as e:
            last_err = e
            continue

    # Try bare soname (ldconfig / LD_LIBRARY_PATH)
    for name in ("libbullet_scrape.so", "bullet_scrape", "libbullet_scrape"):
        try:
            found = ctypes.util.find_library(name.replace("lib", "").replace(".so", "")) 
            # find_library is unreliable; try CDLL directly
            lib = ctypes.CDLL(name)
            _bind(lib)
            _lib = lib
            _lib_path = name
            return _lib
        except OSError as e:
            last_err = e

    raise BulletError(
        "Could not load libbullet_scrape shared library.\n"
        "Build it with:  make shared   (or run the Colab setup cell)\n"
        "Or set BULLET_SCRAPE_LIB=/path/to/libbullet_scrape.so\n"
        f"Last error: {last_err}"
    )


def _bind(lib) -> None:
    lib.bullet_version.restype = c_char_p
    lib.bullet_version.argtypes = []

    lib.bullet_capabilities.restype = c_uint
    lib.bullet_capabilities.argtypes = []

    lib.bullet_backend_info.restype = c_char_p
    lib.bullet_backend_info.argtypes = []

    lib.bullet_scraper_create.restype = c_void_p
    lib.bullet_scraper_create.argtypes = []

    lib.bullet_scraper_destroy.restype = None
    lib.bullet_scraper_destroy.argtypes = [c_void_p]

    lib.bullet_scraper_load_file.restype = c_int
    lib.bullet_scraper_load_file.argtypes = [c_void_p, c_char_p]

    lib.bullet_scraper_load_json.restype = c_int
    lib.bullet_scraper_load_json.argtypes = [c_void_p, c_char_p]

    lib.bullet_scraper_set_output_path.restype = c_int
    lib.bullet_scraper_set_output_path.argtypes = [c_void_p, c_char_p]

    lib.bullet_scraper_set_concurrency.restype = c_int
    lib.bullet_scraper_set_concurrency.argtypes = [c_void_p, c_int]

    lib.bullet_scrape_run.restype = c_int
    lib.bullet_scrape_run.argtypes = [c_void_p, POINTER(c_void_p), POINTER(_BulletStats)]

    lib.bullet_scrape_extract.restype = c_int
    lib.bullet_scrape_extract.argtypes = [
        c_void_p, c_char_p, c_size_t, c_char_p,
        POINTER(c_void_p), POINTER(_BulletStats),
    ]

    lib.bullet_http_get.restype = c_int
    lib.bullet_http_get.argtypes = [
        c_void_p, c_char_p, POINTER(c_void_p), POINTER(c_long), POINTER(c_size_t),
    ]

    lib.bullet_last_error.restype = c_char_p
    lib.bullet_last_error.argtypes = []

    lib.bullet_free.restype = None
    lib.bullet_free.argtypes = [c_void_p]


def _err(lib) -> str:
    msg = lib.bullet_last_error()
    if not msg:
        return "unknown error"
    return msg.decode("utf-8", errors="replace")


def _take_cstr(lib, ptr) -> str:
    """Copy a malloc'd C string into a Python str and free it."""
    if not ptr:
        return ""
    try:
        raw = ctypes.cast(ptr, c_char_p).value
        return raw.decode("utf-8", errors="replace") if raw else ""
    finally:
        lib.bullet_free(ptr)


def _encode(s: Optional[str]) -> Optional[bytes]:
    if s is None:
        return None
    return s.encode("utf-8")


# ── Public helpers ───────────────────────────────────────────────────────────

def version() -> str:
    lib = _load_lib()
    v = lib.bullet_version()
    return v.decode() if v else "unknown"


def backend_info() -> str:
    lib = _load_lib()
    v = lib.bullet_backend_info()
    return v.decode() if v else "unknown"


def capabilities() -> Dict[str, bool]:
    lib = _load_lib()
    caps = int(lib.bullet_capabilities())
    return {
        "curl": bool(caps & 1),
        "xpath": bool(caps & 2),
        "connection_reuse": bool(caps & 4),
        "library_path": _lib_path or "",
    }


# ── Scraper class ────────────────────────────────────────────────────────────

class Scraper:
    """
    High-level scraper handle.

    Example
    -------
    >>> s = Scraper()
    >>> s.load({"url": "https://example.com", "queries": {...}, "output": {"format": "stdout"}})
    >>> result = s.run()
    >>> print(result.stats)
    """

    def __init__(self) -> None:
        self._lib = _load_lib()
        handle = self._lib.bullet_scraper_create()
        if not handle:
            raise BulletError(_err(self._lib) or "failed to create scraper")
        self._handle = handle
        self._loaded = False

    def __del__(self) -> None:
        try:
            if getattr(self, "_handle", None) and getattr(self, "_lib", None):
                self._lib.bullet_scraper_destroy(self._handle)
                self._handle = None
        except Exception:
            pass

    def __enter__(self) -> "Scraper":
        return self

    def __exit__(self, *exc) -> None:
        if self._handle:
            self._lib.bullet_scraper_destroy(self._handle)
            self._handle = None

    # -- config ----------------------------------------------------------------

    def load(self, config: Union[str, Mapping[str, Any], Path]) -> "Scraper":
        """
        Load configuration from:
          • dict / mapping  → JSON string
          • Path / str path ending in .json that exists → file
          • raw JSON string
        """
        if isinstance(config, Path):
            return self.load_file(str(config))

        if isinstance(config, Mapping):
            return self.load_json(json.dumps(config))

        if isinstance(config, str):
            # Existing file path?
            if config.endswith(".json") and os.path.isfile(config):
                return self.load_file(config)
            # Looks like JSON object/array
            stripped = config.lstrip()
            if stripped.startswith("{") or stripped.startswith("["):
                return self.load_json(config)
            # Otherwise treat as path
            return self.load_file(config)

        raise TypeError(f"unsupported config type: {type(config)!r}")

    def load_file(self, path: str) -> "Scraper":
        rc = self._lib.bullet_scraper_load_file(self._handle, _encode(path))
        if rc != 0:
            raise BulletError(_err(self._lib), code=rc)
        self._loaded = True
        # Default: capture results in memory for Python (no stdout spam)
        self._lib.bullet_scraper_set_output_path(self._handle, b"")
        return self

    def load_json(self, json_str: str) -> "Scraper":
        # Force memory output so the engine does not print JSON to stdout;
        # Python receives records via the C API return buffer.
        try:
            obj = json.loads(json_str)
            if isinstance(obj, dict):
                out = obj.get("output") or {}
                if not isinstance(out, dict):
                    out = {}
                if not out.get("path"):
                    out["format"] = "memory"
                    out["path"] = ""
                    obj["output"] = out
                    json_str = json.dumps(obj)
        except Exception:
            pass
        rc = self._lib.bullet_scraper_load_json(self._handle, _encode(json_str))
        if rc != 0:
            raise BulletError(_err(self._lib), code=rc)
        self._loaded = True
        self._lib.bullet_scraper_set_output_path(self._handle, b"")
        return self

    def set_output(self, path: str) -> "Scraper":
        rc = self._lib.bullet_scraper_set_output_path(self._handle, _encode(path))
        if rc != 0:
            raise BulletError(_err(self._lib), code=rc)
        return self

    def set_concurrency(self, n: int) -> "Scraper":
        rc = self._lib.bullet_scraper_set_concurrency(self._handle, int(n))
        if rc != 0:
            raise BulletError(_err(self._lib), code=rc)
        return self

    # -- run -------------------------------------------------------------------

    def run(self) -> ScrapeResult:
        if not self._loaded:
            raise BulletError("no config loaded — call load() first")

        out_ptr = c_void_p()
        st = _BulletStats()
        rc = self._lib.bullet_scrape_run(self._handle, byref(out_ptr), byref(st))
        if rc != 0:
            raise BulletError(_err(self._lib), code=rc)

        raw = _take_cstr(self._lib, out_ptr)
        records = json.loads(raw) if raw else []
        if not isinstance(records, list):
            records = [records]
        return ScrapeResult(records=records, stats=Stats.from_c(st), raw_json=raw)

    def extract(
        self,
        html: Union[str, bytes],
        base_url: str = "",
    ) -> ScrapeResult:
        """Offline extraction — no network. Great for benches & tests."""
        if not self._loaded:
            raise BulletError("no config loaded — call load() first")

        if isinstance(html, str):
            html_b = html.encode("utf-8")
        else:
            html_b = html

        out_ptr = c_void_p()
        st = _BulletStats()
        rc = self._lib.bullet_scrape_extract(
            self._handle,
            html_b,
            len(html_b),
            _encode(base_url) if base_url else None,
            byref(out_ptr),
            byref(st),
        )
        if rc != 0:
            raise BulletError(_err(self._lib), code=rc)

        raw = _take_cstr(self._lib, out_ptr)
        records = json.loads(raw) if raw else []
        if not isinstance(records, list):
            records = [records]
        return ScrapeResult(records=records, stats=Stats.from_c(st), raw_json=raw)

    def http_get(self, url: str) -> Dict[str, Any]:
        """Fetch a single URL (no extraction). Returns {body, status, bytes}."""
        out_ptr = c_void_p()
        status = c_long(0)
        nbytes = c_size_t(0)
        rc = self._lib.bullet_http_get(
            self._handle, _encode(url), byref(out_ptr), byref(status), byref(nbytes)
        )
        if rc != 0:
            raise BulletError(_err(self._lib), code=rc)
        body = _take_cstr(self._lib, out_ptr)
        return {"body": body, "status": int(status.value), "bytes": int(nbytes.value)}


# ── Module-level convenience ─────────────────────────────────────────────────

def scrape(
    config: Union[str, Mapping[str, Any], Path],
    *,
    concurrency: Optional[int] = None,
    output: Optional[str] = None,
) -> ScrapeResult:
    """One-shot scrape. Preferred entry point for notebooks."""
    with Scraper() as s:
        s.load(config)
        if concurrency is not None:
            s.set_concurrency(concurrency)
        if output is not None:
            s.set_output(output)
        return s.run()


def scrape_file(
    path: str,
    *,
    concurrency: Optional[int] = None,
    output: Optional[str] = None,
) -> ScrapeResult:
    return scrape(path, concurrency=concurrency, output=output)


def extract(
    config: Union[str, Mapping[str, Any]],
    html: Union[str, bytes],
    base_url: str = "",
) -> ScrapeResult:
    """Offline extract against raw HTML."""
    with Scraper() as s:
        s.load(config)
        return s.extract(html, base_url=base_url)


def http_get(url: str) -> Dict[str, Any]:
    with Scraper() as s:
        # Minimal config so the handle is valid; http_get works without load
        # but set_concurrency etc. need a live handle — create is enough.
        return s.http_get(url)
