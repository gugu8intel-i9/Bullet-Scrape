# Bullet Scrape — Architecture & Design

## Philosophy

Scrapling and similar tools are too heavy. They build full DOM trees, support every possible selector language, and spend more time in abstraction than in extraction. Bullet Scrape takes the opposite approach:

> **Parse only what you ask for. Fetch only what you need. Ship it fast.**

The entire tool is built around four principles:

1. **Minimal parsing** — Don't build a DOM unless XPath is explicitly requested. Regex extraction operates directly on raw HTML bytes.
2. **Connection reuse** — Persistent curl connections, configurable pool size, HTTP/2 where available.
3. **Composable pipeline** — Fetch → Parse → Extract → Output. Each stage is independent. Add middleware at any stage.
4. **Declarative configuration** — JSON config describes *what* to extract, not *how*. The engine figures out the fastest path.

---

## Architecture

```
┌──────────────────────────────────────────────────────┐
│                    CLI / Entry                        │
└───────────────────────┬──────────────────────────────┘
                        │ reads config
┌───────────────────────▼──────────────────────────────┐
│              ScraperConfig (JSON)                    │
│ url / method / headers / queries / output / limits   │
└───────────────────────┬──────────────────────────────┘
                        │
┌───────────────────────▼──────────────────────────────┐
│              RequestScheduler                         │
│ thread pool · concurrent limit · rate limiter         │
│ retry queue · session/cookie jar                     │
└──────┬──────────────┬──────────────┬─────────────────┘
       │              │              │
  ┌────▼────┐   ┌────▼────┐   ┌────▼────┐
  │  Fetch  │   │  Fetch  │   │  Fetch  │  ← parallel
  │  Page 1 │   │  Page 2 │   │  Page 3 │
  └────┬────┘   └────┬────┘   └────┬────┘
       │              │              │
  ┌────▼──────────────▼──────────────▼────┐
  │         ExtractionEngine              │
  │  • regex extract (fast, zero-copy)   │
  │  • xpath extract (when needed)       │
  │  • attribute pluck                    │
  │  • aggregate: join, count, unique    │
  └─────────────────┬────────────────────┘
                    │
  ┌─────────────────▼────────────────────┐
  │           OutputWriter                │
  │  json · csv · stdout · file          │
  └───────────────────────────────────────┘
```

### Layers

| Layer          | Responsibility                                          | Cost model                        |
|----------------|--------------------------------------------------------|-----------------------------------|
| HTTP Engine    | Persistent connections, concurrent fetch, retries      | ~1 RTT per new host               |
| Parser         | DOM tree OR raw string scanning                        | O(n) where n = page size          |
| Extractor      | Pattern matching → typed records                       | O(patterns × document size)       |
| Aggregator     | Dedupe, count, join, sort, filter                      | O(results)                        |
| Output         | Serialize to JSON / CSV / stdout                       | O(results)                        |

---

## HTTP Engine

Built on libcurl. Key features:

- **Connection pooling**: Reuse TCP connections across requests to the same host (curl `HTTPGET` + keep-alive). Pool size = `max_concurrent`.
- **curl_multi**: Non-blocking async dispatch for true parallelism within a thread.
- **HTTP/2**: Enabled automatically if libcurl was built with nghttp2.
- **Session persistence**: Cookie jar (file or in-memory), custom headers, default headers from config.
- **Retries**: Configurable `max_retries`, `retry_delay_ms`, retry on 429/500/502/503/504.
- **Timeout**: Per-request `timeout_ms`, overall `timeout_ms`.
- **Compression**: `Accept-Encoding: gzip, br, deflate` automatic.
- **Proxies**: Single proxy or per-request proxy from config. SOCKS5 + HTTP proxy support.
- **User-Agent**: Configurable, with a sane default.

---

## Parsing Strategy

Bullet Scrape uses a **two-tier** parser. The engine chooses the cheaper strategy automatically:

### Tier 1 — Regex Extract (the default, the fast path)

No DOM is built. The raw HTML bytes are scanned with `std::regex` (ECMAScript syntax). For each query:

- `regex` — the pattern to run
- `attribute` — optional; if set, capture group 1 is treated as a tag, then we scan for the attribute value
- `aggregate` — `join`, `count`, `unique`, `first`, `last`
- `transform` — `trim`, `lowercase`, `uppercase`, `int`, `float`, `urljoin(base)`

Regex is fast because:
- No tree construction
- No allocator churn per node
- The entire document is one contiguous block
- A thread-safe pattern cache (`shared_mutex`) shared by workers
- **Fast-pattern matching**: at compile time each pattern is classified. Shapes
  that are common in scraping — pure literals, `prefix + [^x]* / [^x]+ /
  [^x]{m,n} + suffix`, `prefix + (.*?) / (.*) + suffix` — run through a
  dedicated scanner (`memchr`/`memcmp`, hand-rolled backtracking mirroring
  ECMAScript greediness) and never touch `std::regex`. A cached analysis means
  the check runs once per pattern per process.
- Fallback anchoring: patterns outside those shapes keep `std::regex`, but
  when they start with ≥2 literal bytes, candidate positions come from
  `memchr`/`memcmp` and the engine only runs *anchored* at each candidate
  (`match_continuous`), instead of scanning byte-by-byte.
- Correctness: `tests/test_regex_engine.cpp` runs a deterministic fuzz plus a
  fixed corpus through both engines and demands byte-identical match results.

Selector queries add a **tier 1.5 tag index**: one single-pass, quote-aware scan
of the raw HTML (no DOM) producing POD element spans. It handles comments,
CDATA, `<script>`/`<style>` raw-text, void/self-closing tags and HTML5 implied
end tags (`<li>`, `<p>`, `<td>`, …), silently recovers from missing close tags,
and lowercases only tag names — never the document. Extraction (text,
attributes, regex) then works directly on `std::string_view`s into the page.
Text extraction decodes HTML entities and collapses whitespace.

### Data cleaning sieve

After extraction and before transforms, every value passes through a
configurable cleaning pipeline (see `cleaner.hpp`). Stages:

1. `entities` — HTML entity decode (~250 names + decimal/hex numerics with the
   Windows-1252 correction)
2. `invisibles` — zero-width / directional / control Unicode cleanup
3. `whitespace` — Unicode-space normalisation, run collapse, trim
4. `fold` — smart quotes/dashes/ellipsis → ASCII
5. `numeric` — canonicalise numbers embedded in noisy text (`- € 1,299.50` →
   `-1299.50`), leaving non-numeric strings untouched
6. `null_tokens` — `n/a`, `-`, `none`, … → JSON `null`

The default mask is `entities|invisibles|whitespace`, fused into a single
lookahead pass (`stage_fused_default`); other combinations run stage-by-stage
with output-doubling-safe in-place rewrite. Config: a global `"clean"` spec
(`true`/`false`/name/list) with per-`ExtractRule` overrides (`clean: -1`
inherits). `int`/`float` transforms independently retry after
`clean_numeric_inplace`, so messy prices convert even when the `numeric`
stage is off.

### Tier 2 — XPath (opt-in, heavy)

When `xpath` is specified, we parse with pugixml and run an XPath query. This is slower but necessary when you need structural queries like:

- `//div[@class="product"]/a[1]`
- Nested attribute access
- Text node selection with position predicates

---

## Query Configuration Language

Bullet Scrape config is a JSON file. Example:

```json
{
  "name": "product_scraper",
  "url": "https://example-shop.com/products",
  "method": "GET",
  "headers": {
    "Accept": "text/html",
    "Accept-Language": "en-US"
  },
  "queries": {
    "products": {
      "selector": "a.product-item",
      "extract": {
        "url":      { "attribute": "href" },
        "title":    { "text": true },
        "price":    { "regex": "[\\$]?([0-9]+\\.[0-9]{2})", "transform": ["float"] },
        "in_stock": { "regex": "in-stock", "aggregate": "exists" }
      }
    },
    "total_pages": {
      "regex": "Page \\d+ of (\\d+)",
      "transform": ["int", "first"]
    }
  },
  "pagination": {
    "type": "url_param",
    "param": "page",
    "max_pages": 50,
    "next_selector": "a.next"
  },
  "output": {
    "format": "json",
    "path": "products.json",
    "array": true
  },
  "limits": {
    "max_concurrent": 8,
    "max_retries": 2,
    "retry_delay_ms": 1000,
    "timeout_ms": 30000
  }
}
```

### Query composition

A top-level query (like `products`) is a **collection query**: it finds multiple elements and runs sub-queries on each. A leaf query returns a scalar.

```
collection(query) → [ { leaf1, leaf2, ... }, { ... } ]
scalar(query)     → "value" | 42 | true | null
```

### Aggregation operators

| Operator  | Behaviour                                                    |
|-----------|--------------------------------------------------------------|
| `join`    | Concatenate all matches with `joinSep`                      |
| `count`   | Number of matches                                            |
| `unique`  | Deduplicated matches                                         |
| `first`   | First match only                                             |
| `last`    | Last match only                                              |
| `exists`  | `true` if any match, `false` otherwise                       |

### Transform operators

| Transform       | Behaviour                              |
|-----------------|----------------------------------------|
| `trim`          | Strip whitespace                       |
| `lowercase`     | `tolower`                              |
| `uppercase`     | `toupper`                              |
| `int`           | `stoi` (null on failure)              |
| `float`         | `stod` (null on failure)              |
| `urljoin(base)` | Resolve relative URL against base      |
| `regex_sub(r, s)`| Apply `regex_replace` with pattern r  |

---

## Concurrency Model

```
Main Thread
  │
  ├─ load config
  ├─ build URL list (pagination / url list)
  │
  └─ spawn N worker threads (N = max_concurrent)
        │
        ├─ pop URL from queue
        ├─ fetch (libcurl, connection pool)
        ├─ parse + extract
        ├─ push result to output queue
        │
        └─ repeat until queue empty

Main Thread (collector)
  │
  ├─ drain output queue
  ├─ apply post-processing (dedupe, sort, filter)
  └─ write output
```

Thread-safety: only the output queue is shared. Each worker owns its curl handle and parser state. No locks in the hot path.

---

## Output Formats

| Format | Description                                                  |
|--------|--------------------------------------------------------------|
| `json` | JSON array of objects (one per collection item)             |
| `jsonl`| JSON Lines — one object per line (streaming-friendly)        |
| `csv`  | CSV with header row derived from query keys                 |
| `stdout`| Same as `json` but to stdout                               |

---

## Defaults

When config is sparse, Bullet Scrape fills in:

- `method`: GET
- `output.format`: json
- `output.path`: stdout
- `output.array`: true
- `limits.max_concurrent`: 4
- `limits.max_retries`: 0
- `limits.timeout_ms`: 30000
- `user_agent`: "BulletScrape/1.0"
- `clean`: `["entities", "invisibles", "whitespace"]` (the fused sieve default)

---

## File Layout

```
Bullet-Scrape/
├── CMakeLists.txt           # build system (static + shared)
├── Makefile                 # Make build (auto-detects libcurl)
├── DESIGN.md                # this file
├── README.md                # user-facing docs
│
├── include/
│   └── bullet_scrape/
│       ├── c_api.h          # stable C ABI (Python / other bindings)
│       ├── config.hpp       # JSON config types
│       ├── exceptions.hpp   # error categories
│       ├── http_client.hpp  # libcurl / POSIX HTTP engine
│       ├── posix_http.hpp   # HTTP-only fallback client
│       ├── extractor.hpp    # query engine
│       ├── mini_json.hpp    # embedded JSON (zero deps)
│       ├── output.hpp       # writers
│       └── scraper.hpp      # top-level orchestrator
│
├── src/
│   ├── core/
│   │   ├── config.cpp
│   │   ├── http_client.cpp  # bounded pool + curl / posix
│   │   ├── extractor.cpp
│   │   ├── output.cpp
│   │   ├── scraper.cpp
│   │   └── c_api.cpp        # C ABI implementation
│   └── cli/
│       └── main.cpp
│
├── python/
│   └── bullet_scrape/       # ctypes bindings + build_native
├── notebooks/
│   └── Bullet_Scrape_Colab.ipynb
├── data/                    # sample configs
├── tests/                   # unit tests
└── scripts/
    └── colab_setup.sh       # Colab / Linux one-shot installer
```

## Google Colab / Python bindings

The C API (`c_api.h`) exports a small stable surface:

- `bullet_scraper_create / destroy`
- `bullet_scraper_load_json / load_file`
- `bullet_scrape_run` → malloc'd JSON array + `bullet_stats_t`
- `bullet_scrape_extract` → offline extract (no network)
- `bullet_http_get` → raw fetch

Python loads `libbullet_scrape.so` via `ctypes` (no pybind11 compile step at
import). `scripts/colab_setup.sh` installs deps, builds the `.so` at `-O3`, and
`pip install -e`s the package. See `notebooks/Bullet_Scrape_Colab.ipynb`.


---

## Dependencies

| Dependency      | Purpose            | Install                         |
|----------------|--------------------|---------------------------------|
| C++17 compiler | Language           | `g++ >= 8`, `clang++ >= 7`    |
| libcurl >= 7.68| HTTP               | `apt install libcurl4-openssl-dev` |
| nlohmann/json  | JSON parsing       | fetch header (CMake downloads) |
| pugixml        | XPath (optional)   | `apt install libpugixml-dev`  |
| pthreads       | Threading          | Built-in on Linux              |

---

## Performance Notes

- Selector + regex extraction on a ~91 KB / 500-element page, default sieve
  enabled: **~0.8 ms/page** (`./bullet-scrape --bench`) — down from ~2.9 ms
  with the original byte-scan parser (~3.6×), and from ~1.4 ms before the
  fast-pattern engine landed.
- Bulk rows (selector + text + attribute + urljoin + sieve): ~2.0 µs/row
  (`make bench-full` prints the whole per-workload table).
- Regex fast-patterns are ~10–40 ns per candidate check vs ~200 ns+ for
  `std::regex_search`; doc-wide scans show the gap clearly in `bench-full`.
- DOM + XPath on the same page: ~5–15 ms (depends on tree size).
- Fetch latency dominates for most targets (network RTT). Bullet Scrape minimizes parse time but can't eliminate network latency.
- With `max_concurrent = 8`, 8 pages fetch in roughly the time of 1 page + queue overhead (assuming same host or warmed connections).

---

## Production Hardening

- All I/O errors are typed (`ScrapeError::Http`, `ScrapeError::ParseError`, `ScrapeError::Timeout`, ...).
- Config validation on load: missing required fields → fatal error with clear message.
- Output file is written to a temp path then renamed (atomic on POSIX).
- Signal handling: Ctrl+C drains in-flight requests and writes partial results.
- Rate limiting: configurable `requests_per_second` across the whole scraper.
