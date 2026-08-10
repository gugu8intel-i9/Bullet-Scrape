#!/usr/bin/env bash
# =============================================================================
# Bullet Scrape — Google Colab / Linux one-shot setup
# =============================================================================
# Installs build deps, compiles a high-performance shared library with libcurl,
# and installs the Python package into the active environment.
#
# Usage (Colab):
#   !bash scripts/colab_setup.sh
#
# Usage (local):
#   bash scripts/colab_setup.sh
#
# Environment knobs:
#   BULLET_JOBS=8          parallel compile jobs (default: nproc)
#   BULLET_WITH_PANDAS=1   also pip-install pandas (default: 1 on Colab)
#   BULLET_SKIP_APT=1      skip apt-get (deps already present)
#   BULLET_FORCE_REBUILD=1 always recompile
# =============================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

JOBS="${BULLET_JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)}"
IS_COLAB=0
if [[ -d /content ]] && python3 -c "import google.colab" 2>/dev/null; then
  IS_COLAB=1
fi

echo "╔══════════════════════════════════════════════════════════╗"
echo "║         Bullet Scrape — high-performance setup           ║"
echo "╚══════════════════════════════════════════════════════════╝"
echo "  root : $ROOT"
echo "  jobs : $JOBS"
echo "  colab: $IS_COLAB"
echo ""

# ── 1. System packages ───────────────────────────────────────────────────────
if [[ "${BULLET_SKIP_APT:-0}" != "1" ]]; then
  if command -v apt-get >/dev/null 2>&1; then
    echo "→ Installing build dependencies (apt)…"
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq
    apt-get install -y -qq --no-install-recommends \
      g++ make pkg-config \
      libcurl4-openssl-dev \
      ca-certificates \
      > /tmp/bullet-apt.log 2>&1 || {
        echo "  apt failed — see /tmp/bullet-apt.log"
        tail -20 /tmp/bullet-apt.log || true
        echo "  continuing; will fall back to POSIX HTTP if curl headers missing"
      }
  elif command -v brew >/dev/null 2>&1; then
    echo "→ Ensuring curl via Homebrew…"
    brew list curl >/dev/null 2>&1 || brew install curl
  fi
fi

# ── 2. Compiler sanity ───────────────────────────────────────────────────────
if ! command -v g++ >/dev/null 2>&1 && ! command -v clang++ >/dev/null 2>&1; then
  echo "ERROR: no C++ compiler found (need g++ or clang++)" >&2
  exit 1
fi
CXX="${CXX:-$(command -v g++ || command -v clang++)}"
echo "→ compiler: $CXX ($($CXX --version | head -1))"

# ── 3. Detect libcurl ────────────────────────────────────────────────────────
HAS_CURL=0
if printf '%s\n' '#include <curl/curl.h>' 'int main(){return 0;}' | $CXX -x c++ -fsyntax-only - >/dev/null 2>&1; then
  HAS_CURL=1
  CURL_VER="$(pkg-config --modversion libcurl 2>/dev/null || echo present)"
  echo "→ libcurl: $CURL_VER  (HTTPS + HTTP/2 + gzip ENABLED)"
else
  echo "→ libcurl: NOT FOUND  (POSIX HTTP fallback — HTTP only)"
fi

# ── 4. Build shared library ──────────────────────────────────────────────────
SO_NAME="libbullet_scrape.so"
if [[ "$(uname -s)" == "Darwin" ]]; then
  SO_NAME="libbullet_scrape.dylib"
fi

NEED_BUILD=1
if [[ "${BULLET_FORCE_REBUILD:-0}" != "1" && -f "$ROOT/$SO_NAME" ]]; then
  # Rebuild if any source is newer than the .so
  NEWER=$(find "$ROOT/src" "$ROOT/include" -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' \) -newer "$ROOT/$SO_NAME" 2>/dev/null | head -1 || true)
  if [[ -z "${NEWER}" ]]; then
    NEED_BUILD=0
    echo "→ shared library up to date: $ROOT/$SO_NAME"
  fi
fi

if [[ "$NEED_BUILD" == "1" ]]; then
  echo "→ Compiling native engine (-O3, $JOBS jobs)…"
  make -C "$ROOT" clean-objs >/dev/null 2>&1 || true
  # x86-64-v2 = SSE4.2/POPCNT — safe on Colab VMs and modern laptops
  MARCH_FLAG=""
  if $CXX -march=x86-64-v2 -E -x c++ /dev/null >/dev/null 2>&1; then
    MARCH_FLAG="-march=x86-64-v2"
  fi
  make -C "$ROOT" shared -j"$JOBS" \
    OPT="-O3 -DNDEBUG -ffast-math -funroll-loops ${MARCH_FLAG}"
  echo "→ built $ROOT/$SO_NAME"
fi

# Stage into the Python package
mkdir -p "$ROOT/python/bullet_scrape"
cp -f "$ROOT/$SO_NAME" "$ROOT/python/bullet_scrape/$SO_NAME"
echo "→ staged python/bullet_scrape/$SO_NAME"

# ── 5. Python package ────────────────────────────────────────────────────────
PY="${BULLET_PYTHON:-python3}"
echo "→ Installing Python package (editable)…"
$PY -m pip install -q --upgrade pip setuptools wheel >/dev/null 2>&1 || true
$PY -m pip install -q -e "$ROOT/python"

WITH_PANDAS="${BULLET_WITH_PANDAS:-}"
if [[ -z "$WITH_PANDAS" ]]; then
  WITH_PANDAS=$IS_COLAB
fi
if [[ "$WITH_PANDAS" == "1" ]]; then
  echo "→ Installing pandas (optional DataFrame export)…"
  $PY -m pip install -q "pandas>=1.3" || true
fi

# ── 6. Smoke test ────────────────────────────────────────────────────────────
echo "→ Smoke test…"
$PY - <<'PY'
import bullet_scrape as bs
print(f"  version : {bs.version()}")
print(f"  backend : {bs.backend_info()}")
print(f"  caps    : {bs.capabilities()}")

html = """
<html><body>
  <div class="product"><a href="/p/1">Widget</a><span class="price">$9.99</span></div>
  <div class="product"><a href="/p/2">Gadget</a><span class="price">$19.50</span></div>
</body></html>
"""
cfg = {
    "url": "https://example.com",
    "queries": {
        "items": {
            "selector": "div.product",
            "extract": [
                {"name": "title", "rule": {"text": True, "transform": ["trim"]}},
                {"name": "href",  "rule": {"attribute": "href"}},
            ],
        }
    },
    "output": {"format": "stdout"},
}
r = bs.extract(cfg, html, base_url="https://example.com")
assert len(r.records) >= 1, r
print(f"  extract : {len(r.records)} records in {r.stats.total_ms} ms  ✓")
PY

echo ""
echo "╔══════════════════════════════════════════════════════════╗"
echo "║  Setup complete.                                         ║"
echo "║                                                          ║"
echo "║  import bullet_scrape as bs                              ║"
echo "║  result = bs.scrape({...})                               ║"
echo "║  result.to_dataframe()   # if pandas installed           ║"
echo "╚══════════════════════════════════════════════════════════╝"
