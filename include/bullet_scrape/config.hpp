#pragma once
#include "bullet_scrape/mini_json.hpp"
#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include <chrono>

namespace bullet_scrape {
using json = mini_json::json;
} // namespace bullet_scrape

namespace bullet_scrape {

// ── Transform operator names ───────────────────────────────────────────────
using TransformOp = std::string;  // "trim", "lowercase", "uppercase",
                                   // "int", "float", "urljoin",
                                   // "regex_sub"

// ── Aggregate operator names ───────────────────────────────────────────────
using AggregateOp = std::string;  // "join", "count", "unique", "first", "last", "exists"

// ── A single leaf extraction rule ──────────────────────────────────────────
struct ExtractRule {
    std::optional<bool> text;          // true → extract inner text
    std::optional<std::string> attribute;   // e.g. "href", "src", "data-id"
    std::optional<std::string> regex;       // regex pattern to run on text/attr
    std::optional<std::string> regex_sub;   // replacement pattern
    std::vector<TransformOp> transform;     // applied left to right
    std::optional<AggregateOp> aggregate;   // "join", "count", "unique", ...
    std::optional<std::string> join_sep;    // separator for "join"
    bool  optional = false;                 // don't error if missing

    void load(const json& j);
};

// ── A sub-query inside a collection ────────────────────────────────────────
struct SubQuery {
    std::string name;
    ExtractRule rule;
};

// ── A collection-level query (finds N elements, runs sub-queries on each) ─
struct CollectionQuery {
    std::string name;
    std::string selector;        // CSS-selector-like string or regex
    std::optional<std::string> regex;   // raw regex instead of selector
    std::optional<std::string> xpath;   // XPath (requires DOM parse)
    std::vector<SubQuery> extract; // leaves to extract per element
    bool multiple = true;         // false → treat as scalar

    void load(const json& j);
};

// ── Pagination strategy ─────────────────────────────────────────────────────
struct Pagination {
    enum class Type { None, UrlParam, NextLink, Offset };

    Type type = Type::None;
    std::optional<std::string> param;     // URL param name for UrlParam
    std::optional<int> start_page;         // 1-based
    std::optional<int> max_pages;
    std::optional<std::string> next_selector; // CSS selector for NextLink
    std::optional<int> offset_step;
    std::optional<int> max_offset;
    std::optional<std::string> base_url;  // for NextLink resolution

    static Pagination from_json(const json& j);
    std::vector<std::string> expand_urls(const std::string& base) const;
};

// ── Global limits ───────────────────────────────────────────────────────────
struct Limits {
    int max_concurrent = 4;
    int max_retries    = 0;
    std::chrono::milliseconds retry_delay_ms{1000};
    std::chrono::milliseconds timeout_ms{30000};
    std::optional<double> requests_per_second; // rate limit
    std::optional<std::string> proxy;
    std::optional<bool>    follow_redirects;  // default true
    std::optional<int>     max_redirects;     // default 5
};

// ── Output target ───────────────────────────────────────────────────────────
struct OutputConfig {
    // format: "json" | "jsonl" | "csv" | "txt" | "stdout" | "memory"
    // (parquet is exported from the Python bindings — see ScrapeResult.export)
    std::string format = "json";
    std::string path;               // file path or empty → stdout
    bool array = true;
    std::vector<std::string> csv_fields; // explicit header / field order (csv, txt)
};

// ── Full scraper configuration ─────────────────────────────────────────────
struct ScraperConfig {
    std::string name;
    std::string url;
    std::vector<std::string> url_list;  // explicit URL list (alternative to pagination)
    std::string method  = "GET";
    std::string body;
    std::unordered_map<std::string, std::string> headers;
    std::string user_agent = "BulletScrape/1.0";
    std::optional<std::string> cookie_file;
    Limits    limits;
    OutputConfig output;
    std::unordered_map<std::string, CollectionQuery> queries; // name → query
    std::optional<Pagination> pagination;

    // Derived: the full list of URLs to scrape
    std::vector<std::string> all_urls;

    void load(const std::string& path);
    void load(const json& j, const std::string& src = "<config>");
    void validate() const;
    void expand_urls();
};

// ── A single extracted record (one collection element) ────────────────────
using Record = std::unordered_map<std::string, json>;

// ── Scraped result for one URL ─────────────────────────────────────────────
struct ScrapedResult {
    std::string url;
    std::string raw_html;          // captured if capture_raw = true
    std::vector<Record> records;
    int http_status = 0;
    std::chrono::milliseconds latency{0};
    std::optional<std::string> error;
    size_t bytes_downloaded = 0;
};

// ── Full scrape run result ─────────────────────────────────────────────────
struct ScrapeResult {
    std::string config_name;
    std::vector<ScrapedResult> results;
    std::chrono::milliseconds total_ms{0};
    size_t total_bytes = 0;
    int succeeded = 0;
    int failed    = 0;
};

} // namespace bullet_scrape
