# Bullet Scrape

> **Parse only what you ask for. Fetch only what you need. Ship it fast.**

Bullet Scrape is a high-performance C++ web scraping engine built for speed and precision. Unlike general-purpose scraping frameworks, it does zero unnecessary work: no DOM is built unless you use XPath, HTTP connections are pooled and reused, and extraction runs directly against raw HTML bytes.

| Feature | Bullet Scrape |
|---|---|
| Language | C++17 |
| HTTP | libcurl (connection pooling, HTTP/2, gzip, retries) + POSIX fallback |
| Extraction | Regex (zero-copy) + CSS-selector-lite + XPath (opt-in) |
| Concurrency | Bounded worker pool, configurable parallelism |
| Output | JSON · JSONL · CSV · TXT · Parquet (Python) · stdout · memory |
| Config | Declarative JSON |
| Bindings | CLI · C API · **Python / Google Colab** |
| Build | Make / CMake, static + shared library |

---

## Google Colab (one cell)

Open the notebook, or paste this into a fresh Colab runtime:

```python
!git clone --depth 1 https://github.com/gugu8intel-i9/Bullet-Scrape.git
%cd Bullet-Scrape
!bash scripts/colab_setup.sh

import bullet_scrape as bs
print(bs.version(), bs.backend_info())

result = bs.scrape({
    "url": "https://example.com",
    "queries": {
        "links": {
            "selector": "a",
            "extract": [
                {"name": "href", "rule": {"attribute": "href", "transform": ["urljoin"]}},
                {"name": "text", "rule": {"text": True, "transform": ["trim"]}},
            ],
        }
    },
    "limits": {"max_concurrent": 8, "timeout_ms": 15000},
})
print(result.stats)
print(result.records)

# Export anywhere — format inferred from extension
result.export("links.parquet")   # optimised zstd parquet (needs pyarrow)
result.export("links.csv")
result.export("links.jsonl")
result.export("links.txt")
# df = result.to_dataframe()
```

📓 Full walkthrough (concurrency, benches, pandas):  
[`notebooks/Bullet_Scrape_Colab.ipynb`](notebooks/Bullet_Scrape_Colab.ipynb)

Python API docs: [`python/README.md`](python/README.md)

### What Colab setup does

1. Installs `g++`, `make`, `libcurl4-openssl-dev`
2. Compiles `libbullet_scrape.so` at **`-O3 -ffast-math -march=x86-64-v2`**
3. Enables libcurl → **HTTPS, HTTP/2, gzip, TCP keep-alive**
4. Installs the `bullet_scrape` Python package (ctypes, no pybind11)
5. Smoke-tests offline extraction

Typical cold setup: **30–60 s**. Subsequent runs reuse the `.so`.

---

## Quick start (CLI)

```bash
# 1. Install dependencies
sudo apt install -y libcurl4-openssl-dev cmake g++ make pkg-config  # Ubuntu/Debian
brew install curl cmake                                               # macOS

# 2. Build (Make — simplest)
make -j$(nproc)          # CLI + static lib
make shared              # libbullet_scrape.so for Python
make test
make bench

# Or CMake
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DBULLET_BUILD_TESTS=ON -DBULLET_BUILD_SHARED=ON
cmake --build . -j$(nproc)

# 3. Generate an example config
./bullet-scrape --example > my_scrape.json

# 4. Edit the config, then run
./bullet-scrape my_scrape.json
```

### Python (local)

```bash
make shared -j$(nproc)
pip install -e ./python

python -c "import bullet_scrape as bs; print(bs.version(), bs.backend_info())"
```

---

## Config file

A config is a JSON file. The minimum is a URL and one query:

```json
{
  "name": "quick_test",
  "url": "https://example.com",
  "queries": {
    "links": {
      "selector": "a",
      "extract": [
        { "name": "href", "rule": { "attribute": "href" } }
      ]
    }
  },
  "output": { "format": "stdout" }
}
```

```bash
./bullet-scrape minimal.json
```

Generate a full example:

```bash
./bullet-scrape --example > example.json   # pretty config
./bullet-scrape --bench                    # micro-benchmark
```

## Queries

Bullet Scrape uses a **two-tier** extraction engine:

### Tier 1 — Regex + Tag finding (default, fast)

No DOM tree is built. Queries are resolved by scanning raw HTML bytes.

```json
"queries": {
  "products": {
    "selector": "div.product-card",
    "extract": [
      { "name": "url",    "rule": { "attribute": "href", "transform": ["urljoin"] } },
      { "name": "title",  "rule": { "text": true, "transform": ["trim"] } },
      { "name": "price",  "rule": { "regex": "[$]?([0-9]+\\.[0-9]{2})", "transform": ["float", "first"] } }
    ]
  }
}
```

**Supported selectors:**

| Selector | Meaning |
|---|---|
| `div` | Tag name |
| `.price` | Class |
| `#main` | ID |
| `div.product` | Tag + class |
| `div.card.featured` | Tag + multiple classes |
| `a[href]` | Tag with attribute presence |
| `a[href="/p/1"]` | Tag with attribute value (single or double quotes) |

Selectors are matched against a **single-pass, quote-aware document index** —
no DOM is built. The scanner handles quoted `>` inside attributes, comments,
`<script>`/`<style>` raw-text content (JS strings never leak into matches),
void and self-closing tags, and browser-style implied end tags (`<li>` without
`</li>`, `<p>` closed by a block element, …). Text extraction decodes HTML
entities and collapses whitespace. For compound selectors (`div.card > a`),
the rightmost simple segment is used.

### Tier 2 — XPath (opt-in)

When `xpath` is set, Bullet Scrape parses with pugixml and runs an XPath query. Slower but supports structural queries like `//div[@class="product"]/a[1]`.

```json
"queries": {
  "first_link": {
    "xpath": "//div[@id='content']//a[1]",
    "extract": [
      { "name": "url", "rule": { "attribute": "href" } }
    ]
  }
}
```

### Aggregation

| Operator | Behaviour |
|---|---|
| `join` | Concatenate matches with `join_sep` |
| `count` | Number of matches |
| `unique` | Deduplicated matches |
| `first` | First match |
| `last` | Last match |
| `exists` | `true` if any match |

### Transforms

`trim` · `lowercase` · `uppercase` · `int` · `float` · `urljoin` · `regex_sub`

## Pagination

```json
"pagination": {
  "type": "url_param",
  "param": "page",
  "max_pages": 20
}
```

Supported types: `url_param` · `next_link` · `offset` · `none`.

## Output

```json
"output": {
  "format": "json",
  "path": "results.json"
}
```

Formats:

| Format | Where | Notes |
|---|---|---|
| `json` | CLI / C++ / Python | Pretty JSON array |
| `jsonl` | CLI / C++ / Python | One object per line (streaming) |
| `csv` | CLI / C++ / Python | `csv_fields` optional — auto-inferred if omitted |
| `txt` | CLI / C++ / Python | Human-readable `key: value` blocks |
| `parquet` | **Python** (`result.export`) | Zstd + dictionary encoding via pyarrow |
| `stdout` / `memory` | CLI / C API | Print or in-process capture |

```python
result.export("out.parquet")                         # zstd level 3, dict-encoded
result.export("out.parquet", compression="snappy")
result.export("out.csv")
result.export("out.jsonl")
result.export("out.txt")
result.to_parquet("data/results.pq", compression_level=6)
```

## Limits & resilience

```json
"limits": {
  "max_concurrent": 8,
  "max_retries": 3,
  "retry_delay_ms": 2000,
  "timeout_ms": 30000,
  "requests_per_second": 2.0,
  "proxy": "http://proxy.example.com:8080"
}
```

## Building from source

```bash
# Make
make -j$(nproc) shared test

# CMake
mkdir build && cd build
cmake .. \
  -DCMAKE_BUILD_TYPE=Release \
  -DBULLET_BUILD_TESTS=ON \
  -DBULLET_BUILD_SHARED=ON
cmake --build . -j$(nproc)
ctest --output-on-failure
```

**Windows:** Use vcpkg: `vcpkg install curl pugixml`

**macOS:** `brew install curl cmake && export CMAKE_PREFIX_PATH=$(brew --prefix curl)`

### C API (for bindings)

```c
#include "bullet_scrape/c_api.h"

bullet_scraper_t* s = bullet_scraper_create();
bullet_scraper_load_json(s, "{...}");
char* json = NULL;
bullet_stats_t st;
bullet_scrape_run(s, &json, &st);
/* use json… */
bullet_free(json);
bullet_scraper_destroy(s);
```

Shared library: `make shared` → `libbullet_scrape.so`.

## Performance characteristics

| Path | Typical cost |
|---|---|
| Regex extract, ~90 KB / 500 nodes | ~1.3 ms / page (bench) |
| Offline Python `bs.extract` same page | ~280 pages/s process throughput |
| DOM + XPath | 5–15 ms (tree size dependent) |
| Network | Dominates real scrapes — raise `max_concurrent` |

### High-performance design points

1. **Bounded worker pool** — `max_concurrent` threads, never unbounded `std::async` fan-out.
2. **Thread-local curl easy handles** — keep-alive + HTTP/2 with no lock on the fetch hot path.
3. **Token-bucket rate limiter** — optional global `requests_per_second`.
4. **Thread-safe regex cache** (`shared_mutex`) — compile once, share across workers.
5. **Literal-prefix regex anchoring** — patterns with a literal start (e.g. `class="`) are seeded with `memchr`+`memcmp` and only verified anchored by the regex engine, instead of letting the backtracking NFA re-scan every byte.
6. **Zero-DOM default** — scan raw HTML bytes; XPath is opt-in. The tier-1.5 tag index is a single pass over the document: interned tag names (no per-tag string copies), POD element spans (extraction reads `string_view`s into the page), one shared index for all selector queries on a page.
7. **Colab build flags** — `-O3 -ffast-math -funroll-loops -march=x86-64-v2`.

```python
import os, bullet_scrape as bs
n = min(32, (os.cpu_count() or 2) * 2)
result = bs.scrape(cfg, concurrency=n)
print(result.stats.pages_per_sec, "pages/s")
```

## Design principles

1. **Minimal parsing** — Regex extraction is the default. Don't pay for a DOM you don't use.
2. **Connection reuse** — Persistent curl connections across requests.
3. **Thread-per-connection** — Each worker owns its curl handle; no locks in the hot path.
4. **Declarative config** — Describe what you want, not how to get it.
5. **Fail clearly** — Every error is typed (`Config`, `Http`, `Parse`, `Extract`, `Io`, `Timeout`).

## Project layout

```
Bullet-Scrape/
├── include/bullet_scrape/   # public C++ headers + c_api.h
├── src/core/                # engine + C API
├── src/cli/                 # bullet-scrape binary
├── python/bullet_scrape/    # ctypes bindings
├── scripts/colab_setup.sh   # one-shot Colab/Linux installer
├── notebooks/               # Colab notebook
├── data/                    # example configs
└── tests/
```

## License

MIT
