"""
Bullet Scrape — high-performance web scraping from Python / Google Colab.

Wraps the native C++ engine (libcurl + zero-copy regex extraction) via ctypes.
No pybind11 / compilation step required at import time if the shared library
is already present (Colab setup builds it once).

Quick start
-----------
>>> import bullet_scrape as bs
>>> result = bs.scrape({
...     "url": "https://example.com",
...     "queries": {
...         "links": {
...             "selector": "a",
...             "extract": [{"name": "href", "rule": {"attribute": "href"}}]
...         }
...     },
...     "output": {"format": "stdout"},
...     "limits": {"max_concurrent": 8},
... })
>>> result.records
>>> result.stats.pages_per_sec
"""

from .core import (
    Scraper,
    ScrapeResult,
    Stats,
    scrape,
    scrape_file,
    extract,
    http_get,
    version,
    backend_info,
    capabilities,
    BulletError,
)
from .export import export_records, SUPPORTED_FORMATS

__version__ = "1.0.0"
__all__ = [
    "Scraper",
    "ScrapeResult",
    "Stats",
    "scrape",
    "scrape_file",
    "extract",
    "http_get",
    "export_records",
    "SUPPORTED_FORMATS",
    "version",
    "backend_info",
    "capabilities",
    "BulletError",
    "__version__",
]
