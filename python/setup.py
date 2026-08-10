#!/usr/bin/env python3
"""
Bullet Scrape — Python package setup.

Install (local / Colab):
    pip install -e ./python

Or from the repo root after `make shared`:
    pip install ./python
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

from setuptools import setup, find_packages
from setuptools.command.build_py import build_py as _build_py
from setuptools.command.develop import develop as _develop
from setuptools.command.install import install as _install


HERE = Path(__file__).resolve().parent


def _try_build_native():
    """Best-effort native build during install. Never fails the pip install —
    the Colab setup script / make shared is the authoritative path."""
    try:
        sys.path.insert(0, str(HERE))
        from bullet_scrape.build_native import build
        build(force=False)
    except Exception as e:
        print(f"[bullet_scrape] note: native build skipped during setup ({e})")
        print("[bullet_scrape] run:  python -m bullet_scrape.build_native")
        print("              or:  bash scripts/colab_setup.sh")


class build_py(_build_py):
    def run(self):
        _try_build_native()
        super().run()


class develop(_develop):
    def run(self):
        _try_build_native()
        super().run()


class install(_install):
    def run(self):
        _try_build_native()
        super().run()


# Package data: ship any prebuilt .so/.dylib sitting in the package dir
package_data = {
    "bullet_scrape": [
        "libbullet_scrape.so",
        "libbullet_scrape.so.*",
        "libbullet_scrape.dylib",
        "*.pyi",
        "py.typed",
    ],
}


setup(
    name="bullet-scrape",
    version="1.0.0",
    description="High-performance C++ web scraper with Python / Colab bindings",
    long_description=(HERE.parent / "README.md").read_text(encoding="utf-8")
        if (HERE.parent / "README.md").is_file()
        else "Bullet Scrape Python bindings",
    long_description_content_type="text/markdown",
    author="Bullet Scrape contributors",
    license="MIT",
    url="https://github.com/gugu8intel-i9/Bullet-Scrape",
    packages=find_packages(),
    package_data=package_data,
    include_package_data=True,
    python_requires=">=3.8",
    install_requires=[],
    extras_require={
        "pandas": ["pandas>=1.3"],
        "parquet": ["pandas>=1.3", "pyarrow>=10"],
        "export": ["pandas>=1.3", "pyarrow>=10"],
        "colab": ["pandas>=1.3", "pyarrow>=10", "matplotlib>=3.5"],
    },
    cmdclass={
        "build_py": build_py,
        "develop": develop,
        "install": install,
    },
    classifiers=[
        "Development Status :: 4 - Beta",
        "Intended Audience :: Developers",
        "License :: OSI Approved :: MIT License",
        "Programming Language :: C++",
        "Programming Language :: Python :: 3",
        "Topic :: Internet :: WWW/HTTP",
        "Topic :: Software Development :: Libraries",
    ],
    zip_safe=False,
)
