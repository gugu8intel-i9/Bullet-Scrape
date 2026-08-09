#include "bullet_scrape/scraper.hpp"
#include "bullet_scrape/exceptions.hpp"
#include "bullet_scrape/output.hpp"
#include "bullet_scrape/extractor.hpp"
#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iomanip>

static void print_usage() {
    std::cout <<
        "bullet-scrape — high-performance web scraping\n\n"
        "Usage:\n"
        "  bullet-scrape <config.json>            run a scrape job\n"
        "  bullet-scrape --bench                  run micro-benchmarks\n"
        "  bullet-scrape --version                print version\n"
        "  bullet-scrape --help                   show this help\n\n"
        "Config file:\n"
        "  A JSON file describing URLs, queries, and extraction rules.\n"
        "  See DESIGN.md for the full schema, or run:\n"
        "    bullet-scrape --example > example.json\n\n"
        "Output:\n"
        "  Results written to output.path in config, or stdout.\n"
        "  Format: json / jsonl / csv / stdout.\n";
}

static void print_version() {
    std::cout << "bullet-scrape 1.0.0\n"
              << "C++17 · POSIX sockets (libcurl for HTTPS)\n";
}

int main(int argc, char** argv) {
    using namespace bullet_scrape;

    if (argc < 2) { print_usage(); return 0; }

    std::string cmd = argv[1];

    if (cmd == "--help" || cmd == "-h") { print_usage(); return 0; }
    if (cmd == "--version" || cmd == "-v") {
        print_version();
        return 0;
    }
    if (cmd == "--example") {
        // Print an example config using a raw string with a unique delimiter
        // to avoid issues with embedded quote sequences.
        std::cout << R"JSONCONFIG({
  "name": "example_blog_scraper",
  "url": "https://example.com/blog",
  "method": "GET",
  "headers": {
    "Accept": "text/html,application/xhtml+xml",
    "Accept-Language": "en-US,en;q=0.9"
  },
  "queries": {
    "posts": {
      "name": "posts",
      "selector": "article.post",
      "extract": [
        { "name": "title",  "rule": { "text": true, "transform": ["trim"] } },
        { "name": "url",    "rule": { "attribute": "href", "transform": ["urljoin"] } },
        { "name": "date",   "rule": { "regex": "\\d{4}-\\d{2}-\\d{2}", "aggregate": "first" } },
        { "name": "excerpt", "rule": { "text": true, "transform": ["trim"], "optional": true } }
      ]
    },
    "total_pages": {
      "name": "total_pages",
      "regex": "Page \\d+ of (\\d+)",
      "extract": [
        { "name": "count", "rule": { "aggregate": "first", "transform": ["int"] } }
      ]
    }
  },
  "pagination": {
    "type": "url_param",
    "param": "page",
    "max_pages": 10
  },
  "output": {
    "format": "json",
    "path": "results.json"
  },
  "limits": {
    "max_concurrent": 4,
    "max_retries": 2,
    "retry_delay_ms": 1500,
    "timeout_ms": 30000
  }
})JSONCONFIG";
        return 0;
    }

    if (cmd == "--bench") {
        std::cerr << "[bench] running micro-benchmarks...\n";

        // Generate a ~50KB HTML page with 500 product elements
        std::string html;
        html += "<!DOCTYPE html><html><head><title>Test</title></head><body>\n";
        for (int i = 0; i < 500; ++i) {
            html += "<div class=\"product\" data-id=\"" + std::to_string(i) + "\">\n";
            html += "  <a href=\"/product/" + std::to_string(i) + "\" class=\"link\">Product " + std::to_string(i) + "</a>\n";
            html += "  <span class=\"price\">$";
            html += std::to_string(10 + (i % 90)) + ".";
            std::string cents = std::to_string(i % 100);
            if (cents.size() == 1) cents = "0" + cents;
            html += cents + "</span>\n";
            html += "  <p class=\"desc\">Description for product " + std::to_string(i) + "</p>\n";
            html += "</div>\n";
        }
        html += "</body></html>";

        // Benchmark
        std::string bench_json = R"({"url":"https://example.com","queries":{"items":{"selector":"div.product","extract":[{"name":"x","rule":{"regex":"class=\"([^\"]+)\""}}]}},"output":{"format":"json"}})";
        ScraperConfig cfg;
        cfg.load(json::parse(bench_json), "<bench>");

        auto t0 = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < 100; ++i) {
            volatile auto v = ExtractionEngine().execute(cfg, html);
            (void)v;
        }
        auto t1 = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count() / 1000.0;
        std::cout << "tag+regex extraction: " << std::fixed << std::setprecision(2)
                  << ms << " ms for 100 iterations ("
                  << html.size() << " bytes, 500 elements)\n";
        std::cout << "per iteration: " << std::fixed << std::setprecision(3)
                  << (ms / 100.0) << " ms\n";
        return 0;
    }

    // ── Normal: run a config file ──────────────────────────────────────────
    std::string config_path = cmd;
    std::string output_override;
    if (argc >= 3 && std::string(argv[2]) != "--")
        output_override = argv[2];

    try {
        auto scraper = std::make_unique<Scraper>();
        scraper->load_config(config_path);

        if (!output_override.empty())
            scraper->config().output.path = output_override;

        std::cerr << "[bullet-scrape] config: " << config_path << "\n";
        std::cerr << "[bullet-scrape] URLs: " << scraper->config().all_urls.size() << "\n";
        std::cerr << "[bullet-scrape] concurrent: " << scraper->config().limits.max_concurrent << "\n";

        auto result = scraper->run();
        std::cout << format_summary(result);
        return result.failed > 0 ? 1 : 0;

    } catch (const ScrapeError& e) {
        std::cerr << "ERROR: " << e.to_string() << "\n";
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << "\n";
        return 2;
    }
}
