# bullet-scrape (Python / Colab)

Python bindings for the Bullet Scrape C++ engine. Uses `ctypes` against
`libbullet_scrape.so` — no pybind11, no compile step at import if the shared
library is already built.

## Install

### Google Colab (recommended)

```python
# Cell 1 — clone + build (≈ 30–60 s)
!git clone --depth 1 https://github.com/gugu8intel-i9/Bullet-Scrape.git
%cd Bullet-Scrape
!bash scripts/colab_setup.sh
```

Or open [`notebooks/Bullet_Scrape_Colab.ipynb`](../notebooks/Bullet_Scrape_Colab.ipynb).

### Local

```bash
# deps
sudo apt install -y g++ make libcurl4-openssl-dev   # Debian/Ubuntu
# brew install curl                                   # macOS

make shared -j$(nproc)
pip install -e ./python
```

## Quick start

```python
import bullet_scrape as bs

result = bs.scrape({
    "name": "example",
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
    "output": {"format": "stdout"},
    "limits": {"max_concurrent": 8, "timeout_ms": 15000},
})

print(result.stats)
for row in result.records[:5]:
    print(row)

# Optional: pandas
df = result.to_dataframe()
```

### Offline extract (no network)

```python
html = open("page.html").read()
result = bs.extract(config_dict, html, base_url="https://example.com")
```

### From a config file

```python
result = bs.scrape_file("data/example_products.json", concurrency=16)
```

## Performance tips (Colab)

| Knob | Guidance |
|------|----------|
| `limits.max_concurrent` | Start at `2 * cpu_count`, cap ~32 on Colab |
| `limits.requests_per_second` | Set when scraping a single host politely |
| `limits.timeout_ms` | 10–15 s is usually enough; fail fast |
| Reuse `Scraper` | `with bs.Scraper() as s:` then `s.load` / `s.run` for multiple jobs |
| Offline benches | Use `bs.extract` — isolates pure parse speed |

```python
import os, bullet_scrape as bs
n = min(32, (os.cpu_count() or 2) * 2)
result = bs.scrape(cfg, concurrency=n)
print(result.stats.pages_per_sec, "pages/s", result.stats.mb_per_sec, "MB/s")
```

## API

| Function | Purpose |
|----------|---------|
| `bs.scrape(config, concurrency=None, output=None)` | One-shot scrape |
| `bs.scrape_file(path, …)` | Scrape from JSON config file |
| `bs.extract(config, html, base_url="")` | Offline extraction |
| `bs.http_get(url)` | Raw HTTP GET |
| `bs.version()` / `bs.backend_info()` / `bs.capabilities()` | Diagnostics |
| `bs.Scraper` | Stateful handle (load / run / extract) |

`ScrapeResult` fields: `.records` (list[dict]), `.stats`, `.raw_json`, `.to_dataframe()`.

## Troubleshooting

**`Could not load libbullet_scrape`**  
Run `bash scripts/colab_setup.sh` or `python -m bullet_scrape.build_native --force`.

**HTTPS fails / empty status**  
You built without libcurl. Install `libcurl4-openssl-dev` and rebuild.

**Set a custom library path**
```bash
export BULLET_SCRAPE_LIB=/path/to/libbullet_scrape.so
```
