# Bullet Scrape

> **Parse only what you ask for. Fetch only what you need. Ship it fast.**

Bullet Scrape is a high-performance C++ web scraping engine built for speed and precision. Unlike general-purpose scraping frameworks, it does zero unnecessary work: no DOM is built unless you use XPath, HTTP connections are pooled and reused, and extraction runs directly against raw HTML bytes.

| Feature | Bullet Scrape |
|---|---|
| Language | C++17 |
| HTTP | libcurl (connection pooling, HTTP/2, retries) |
| Extraction | Regex (zero-copy) + CSS-selector-lite + XPath (opt-in) |
| Concurrency | Thread pool, configurable parallelism |
| Output | JSON · JSONL · CSV · stdout |
| Config | Declarative JSON |
| Build | CMake, single binary |

---

## Quick start

```bash
# 1. Install dependencies
sudo apt install -y libcurl4-openssl-dev cmake g++  # Ubuntu/Debian
brew install curl cmake                               # macOS

# 2. Build
mkdir build && cd build
cmake .. -DBULLET_BUILD_TESTS=ON
make -j$(nproc)

# 3. Generate an example config
../build/bullet-scrape --example > my_scrape.json

# 4. Edit the config, then run
./bullet-scrape my_scrape.json
```

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
| `a[href]` | Tag with attribute presence |
| `a[href="/p/1"]` | Tag with attribute value |

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

Formats: `json` (array) · `jsonl` (lines) · `csv` (needs `csv_fields`) · `stdout`.

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
mkdir build && cd build
cmake .. \
  -DCMAKE_BUILD_TYPE=Release \
  -DBULLET_BUILD_TESTS=ON \
  -DBULLET_BUILD_BENCH=ON
make -j$(nproc)
ctest --output-on-failure
```

**Windows:** Use vcpkg: `vcpkg install curl nlohmann-json pugixml`

**macOS:** `brew install curl cmake && export CMAKE_PREFIX_PATH=$(brew --prefix curl)`

## Performance characteristics

- Regex extraction on a 200 KB HTML page: < 1 ms per query (measured on the bench target).
- DOM-based XPath: 5–15 ms depending on tree size.
- Network latency is the dominant cost for most targets. Use `max_concurrent` to parallelise across pages.
- Connection pooling means second+ request to the same host is just the application-round-trip time.

## Design principles

1. **Minimal parsing** — Regex extraction is the default. Don't pay for a DOM you don't use.
2. **Connection reuse** — Persistent curl connections across requests.
3. **Thread-per-connection** — Each worker owns its curl handle; no locks in the hot path.
4. **Declarative config** — Describe what you want, not how to get it.
5. **Fail clearly** — Every error is typed (`Config`, `Http`, `Parse`, `Extract`, `Io`, `Timeout`).

## License

MIT
