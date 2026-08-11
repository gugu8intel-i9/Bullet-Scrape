// ============================================================================
//  bullet-scrape extraction benchmark
//
//  Times several representative offline workloads on a synthetic ~90 KB
//  product-listing page (500 cards). Run:  make bench-full
//
//  For A/B comparisons, check out an older extractor and rebuild — the
//  harness prints one line per workload:  name  ms/iter  MB/s
// ============================================================================
#include "bullet_scrape/scraper.hpp"
#include "bullet_scrape/extractor.hpp"
#include <chrono>
#include <cstdio>
#include <string>

using namespace bullet_scrape;
using Clock = std::chrono::steady_clock;

static std::string make_html(int cards) {
    std::string h;
    h.reserve(static_cast<size_t>(cards) * 220);
    h += "<!DOCTYPE html><html><head><title>Shop</title>"
         "<script>var t=\"<div class='x'>js junk</div>\";</script></head><body>\n";
    for (int i = 0; i < cards; ++i) {
        char buf[512];
        std::snprintf(buf, sizeof buf,
            "<div class=\"product card\" data-id=\"%d\" data-sku=\"SKU-%04d\">\n"
            "  <a href=\"/product/%d?ref=hp&amp;cc=1\" class=\"link\">Product %d &amp; co</a>\n"
            "  <span class=\"price\">&euro;%d.%02d</span>\n"
            "  <p class=\"desc\">Great product %d &mdash; ships today</p>\n"
            "  <ul class=\"meta\"><li>stock: %d</li><li>sold: %d</li></ul>\n"
            "</div>\n",
            i, i, i, i, 10 + (i % 90), i % 100, i, i % 12, i * 7 % 300);
        h += buf;
    }
    h += "</body></html>";
    return h;
}

struct Workload {
    const char* name;
    const char* config;   // JSON fragment containing only "queries"
};

static double run(const ScraperConfig& cfg, const std::string& html, int iters) {
    // warm-up (regex cache etc.)
    for (int i = 0; i < 3; ++i) {
        volatile auto v = ExtractionEngine().execute(cfg, html, "https://shop.test");
        (void)v;
    }
    const auto t0 = Clock::now();
    for (int i = 0; i < iters; ++i) {
        volatile auto v = ExtractionEngine().execute(cfg, html, "https://shop.test");
        (void)v;
    }
    const auto t1 = Clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()
           / 1e6 / iters;
}

int main(int argc, char** argv) {
    const int iters = argc > 1 ? std::atoi(argv[1]) : 100;
    const std::string html = make_html(500);

    static const Workload workloads[] = {
        {"selector + fast-pattern regex",
         R"J("queries":{"q":{"selector":"div.product","extract":[
             {"name":"cls","rule":{"regex":"class=\"([^\"]+)\"","aggregate":"unique"}}
         ]}})J"},
        {"selector + text + attr + urljoin",
         R"J("queries":{"q":{"selector":"div.product","extract":[
             {"name":"t","rule":{"text":true}},
             {"name":"u","rule":{"attribute":"href","transform":["urljoin"]}}
         ]}})J"},
        {"text only (entities)",
         R"J("queries":{"q":{"selector":"p.desc","extract":[
             {"name":"t","rule":{"text":true}}
         ]}})J"},
        {"doc regex, fast-pattern shape",
         R"J("queries":{"q":{"regex":"data-sku=\"([A-Z]+-[0-9]+)\"","extract":[
             {"name":"sku","rule":{}}
         ]}})J"},
        {"doc regex, fallback (seeded)",
         R"J("queries":{"q":{"regex":"price\">\\x26euro;[0-9]+\\.[0-9]{2}","extract":[
             {"name":"m","rule":{}}
         ]}})J"},
        {"doc regex, fallback (no seed)",
         R"J("queries":{"q":{"regex":"euro;([0-9]+\\.[0-9]{2})","extract":[
             {"name":"m","rule":{}}
         ]}})J"},
        {"generic #id + .class selectors",
         R"J("queries":{"q":{"selector":".link","extract":[
             {"name":"t","rule":{"text":true}}
         ]}})J"},
        {"mixed kitchen sink",
         R"J("queries":{"q":{"selector":"div.card","extract":[
             {"name":"cls","rule":{"regex":"class=\"([^\"]+)\""}},
             {"name":"price","rule":{"regex":"([0-9]+\\.[0-9]{2})","transform":["float"],"aggregate":"first"}},
             {"name":"t","rule":{"text":true}},
             {"name":"u","rule":{"attribute":"href","transform":["urljoin"]}},
             {"name":"sku","rule":{"attribute":"data-sku"}}
         ]}})J"},
    };

    std::printf("%-34s %8s %8s\n", "workload", "ms/iter", "MB/s");
    std::printf("%-34s %8s %8s\n", "----------------------------------", "--------", "--------");
    for (const auto& w : workloads) {
        ScraperConfig cfg;
        std::string j = std::string("{\"url\":\"https://shop.test\",") + w.config + "}";
        cfg.load(json::parse(j), "<bench>");
        const double ms = run(cfg, html, iters);
        std::printf("%-34s %8.3f %8.1f\n", w.name, ms, html.size() / (ms * 1000.0));
    }
    return 0;
}
