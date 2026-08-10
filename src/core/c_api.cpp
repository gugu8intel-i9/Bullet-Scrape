#include "bullet_scrape/c_api.h"
#include "bullet_scrape/scraper.hpp"
#include "bullet_scrape/extractor.hpp"
#include "bullet_scrape/http_client.hpp"
#include "bullet_scrape/exceptions.hpp"
#include "bullet_scrape/mini_json.hpp"
#include "bullet_scrape/output.hpp"

#include <cstring>
#include <cstdlib>
#include <string>
#include <new>
#include <mutex>
#include <sstream>

using namespace bullet_scrape;

// ── Thread-local error buffer ────────────────────────────────────────────────

static thread_local std::string g_last_error;

static void set_error(const std::string& msg) {
    g_last_error = msg;
}

static void clear_error() {
    g_last_error.clear();
}

static char* dup_cstr(const std::string& s) {
    char* p = static_cast<char*>(std::malloc(s.size() + 1));
    if (!p) return nullptr;
    std::memcpy(p, s.data(), s.size());
    p[s.size()] = '\0';
    return p;
}

// ── Opaque handle ────────────────────────────────────────────────────────────

struct bullet_scraper {
    Scraper scraper;
    bool    loaded = false;
};

// ── Helpers ──────────────────────────────────────────────────────────────────

static std::string records_to_json(const std::vector<Record>& records) {
    json arr = json::array();
    for (const auto& r : records) {
        json obj = json::object();
        for (const auto& [k, v] : r)
            obj[k] = v;
        arr.push_back(std::move(obj));
    }
    return arr.dump();
}

static std::string scrape_result_to_json(const ScrapeResult& result) {
    json arr = json::array();
    for (const auto& page : result.results) {
        for (const auto& r : page.records) {
            json obj = json::object();
            // Annotate with source URL for multi-page runs
            obj["_url"] = page.url;
            if (page.http_status)
                obj["_status"] = static_cast<long long>(page.http_status);
            for (const auto& [k, v] : r)
                obj[k] = v;
            arr.push_back(std::move(obj));
        }
    }
    return arr.dump();
}

static void fill_stats(bullet_stats_t* stats, const ScrapeResult& result, int record_count) {
    if (!stats) return;
    std::memset(stats, 0, sizeof(*stats));
    stats->total_ms      = result.total_ms.count();
    stats->total_bytes   = static_cast<int64_t>(result.total_bytes);
    stats->succeeded     = result.succeeded;
    stats->failed        = result.failed;
    stats->url_count     = static_cast<int>(result.results.size());
    stats->record_count  = record_count;
    if (result.total_ms.count() > 0) {
        double secs = result.total_ms.count() / 1000.0;
        stats->pages_per_sec = result.results.size() / secs;
        stats->mb_per_sec    = (result.total_bytes / (1024.0 * 1024.0)) / secs;
    }
}

// ── Public API ───────────────────────────────────────────────────────────────

extern "C" {

const char* bullet_version(void) {
    return "1.0.0";
}

unsigned bullet_capabilities(void) {
    unsigned caps = 0;
#if defined(BULLET_HAVE_CURL) || defined(HAVE_CURL)
    caps |= 1u;       // libcurl
    caps |= 4u;       // multi / connection reuse
#endif
#ifdef BULLET_HAVE_XPATH
    caps |= 2u;       // XPath
#endif
    return caps;
}

const char* bullet_backend_info(void) {
#if defined(BULLET_HAVE_CURL) || defined(HAVE_CURL)
    return "libcurl (HTTPS, HTTP/2, gzip, connection pooling)";
#else
    return "posix sockets (HTTP only — rebuild with libcurl for HTTPS)";
#endif
}

bullet_scraper_t* bullet_scraper_create(void) {
    clear_error();
    try {
        return new bullet_scraper();
    } catch (...) {
        set_error("out of memory");
        return nullptr;
    }
}

void bullet_scraper_destroy(bullet_scraper_t* s) {
    delete s;
}

int bullet_scraper_load_file(bullet_scraper_t* s, const char* path) {
    clear_error();
    if (!s || !path) { set_error("null argument"); return 1; }
    try {
        s->scraper.load_config(path);
        s->loaded = true;
        return 0;
    } catch (const ScrapeError& e) {
        set_error(e.to_string());
        return 2;
    } catch (const std::exception& e) {
        set_error(e.what());
        return 2;
    }
}

int bullet_scraper_load_json(bullet_scraper_t* s, const char* json_utf8) {
    clear_error();
    if (!s || !json_utf8) { set_error("null argument"); return 1; }
    try {
        s->scraper.load_json(json_utf8);
        s->loaded = true;
        return 0;
    } catch (const ScrapeError& e) {
        set_error(e.to_string());
        return 2;
    } catch (const std::exception& e) {
        set_error(e.what());
        return 2;
    }
}

int bullet_scraper_set_output_path(bullet_scraper_t* s, const char* path) {
    clear_error();
    if (!s) { set_error("null scraper"); return 1; }
    try {
        // Empty / stdout → in-memory capture (C API returns JSON string)
        if (!path || path[0] == '\0' || std::strcmp(path, "stdout") == 0
            || std::strcmp(path, "memory") == 0) {
            s->scraper.config().output.path.clear();
            s->scraper.config().output.format = "memory";
        } else {
            s->scraper.config().output.path = path;
            if (s->scraper.config().output.format == "stdout"
                || s->scraper.config().output.format == "memory")
                s->scraper.config().output.format = "json";
        }
        return 0;
    } catch (const std::exception& e) {
        set_error(e.what());
        return 2;
    }
}

int bullet_scraper_set_concurrency(bullet_scraper_t* s, int n) {
    clear_error();
    if (!s) { set_error("null scraper"); return 1; }
    if (n < 1) { set_error("concurrency must be >= 1"); return 1; }
    if (n > 256) n = 256;
    s->scraper.config().limits.max_concurrent = n;
    return 0;
}

int bullet_scrape_run(bullet_scraper_t* s, char** out_json, bullet_stats_t* stats) {
    clear_error();
    if (out_json) *out_json = nullptr;
    if (!s) { set_error("null scraper"); return 1; }
    if (!s->loaded) { set_error("no config loaded"); return 1; }
    if (!out_json) { set_error("out_json is required"); return 1; }

    try {
        // Prefer in-memory capture for the C API unless user set a file path.
        // "memory" uses NullWriter so nothing is printed to stdout.
        auto& out = s->scraper.config().output;
        if (out.path.empty() || out.format == "stdout") {
            out.format = "memory";
            out.path.clear();
        }

        auto result = s->scraper.run();
        std::string js = scrape_result_to_json(result);

        int rec_count = 0;
        for (const auto& page : result.results)
            rec_count += static_cast<int>(page.records.size());

        fill_stats(stats, result, rec_count);

        *out_json = dup_cstr(js);
        if (!*out_json) { set_error("out of memory"); return 3; }
        return 0;
    } catch (const ScrapeError& e) {
        set_error(e.to_string());
        return 2;
    } catch (const std::exception& e) {
        set_error(e.what());
        return 2;
    }
}

int bullet_scrape_extract(bullet_scraper_t* s,
                          const char* html,
                          size_t html_len,
                          const char* base_url,
                          char** out_json,
                          bullet_stats_t* stats) {
    clear_error();
    if (out_json) *out_json = nullptr;
    if (!s) { set_error("null scraper"); return 1; }
    if (!s->loaded) { set_error("no config loaded"); return 1; }
    if (!html) { set_error("null html"); return 1; }
    if (!out_json) { set_error("out_json is required"); return 1; }

    try {
        std::string html_str;
        if (html_len == static_cast<size_t>(-1))
            html_str.assign(html);
        else
            html_str.assign(html, html_len);

        std::string base = base_url ? base_url : "";

        auto t0 = std::chrono::steady_clock::now();
        auto records = ExtractionEngine().execute(s->scraper.config(), html_str, base);
        auto t1 = std::chrono::steady_clock::now();

        std::string js = records_to_json(records);
        *out_json = dup_cstr(js);
        if (!*out_json) { set_error("out of memory"); return 3; }

        if (stats) {
            std::memset(stats, 0, sizeof(*stats));
            stats->total_ms     = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
            // Prefer microsecond resolution for short offline extracts
            auto us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
            if (stats->total_ms == 0 && us > 0)
                stats->total_ms = 1; // at least 1 ms for rate math
            stats->total_bytes  = static_cast<int64_t>(html_str.size());
            stats->succeeded    = 1;
            stats->failed       = 0;
            stats->url_count    = 1;
            stats->record_count = static_cast<int>(records.size());
            if (us > 0) {
                double secs = us / 1e6;
                stats->pages_per_sec = 1.0 / secs;
                stats->mb_per_sec    = (html_str.size() / (1024.0 * 1024.0)) / secs;
            }
            // Stash microseconds in total_ms when sub-ms — expose via pages_per_sec
            (void)us;
        }
        return 0;
    } catch (const ScrapeError& e) {
        set_error(e.to_string());
        return 2;
    } catch (const std::exception& e) {
        set_error(e.what());
        return 2;
    }
}

int bullet_http_get(bullet_scraper_t* s,
                    const char* url,
                    char** out_body,
                    long* status,
                    size_t* bytes) {
    clear_error();
    if (out_body) *out_body = nullptr;
    if (!s || !url || !out_body) { set_error("null argument"); return 1; }

    try {
        HTTPClient::Options opts;
        if (s->loaded) {
            auto& cfg = s->scraper.config();
            opts.max_concurrent   = cfg.limits.max_concurrent;
            opts.timeout_ms       = cfg.limits.timeout_ms;
            opts.proxy            = cfg.limits.proxy;
            opts.follow_redirects = cfg.limits.follow_redirects;
            opts.max_redirects    = cfg.limits.max_redirects.value_or(5);
            opts.rate_limit_rps   = cfg.limits.requests_per_second;
            opts.user_agent       = cfg.user_agent;
        }
        HTTPClient client(opts);
        auto resp = client.fetch(url);

        if (status) *status = resp.status;
        if (bytes)  *bytes  = resp.bytes;

        if (resp.status == 0 || resp.curl_code != CURLE_OK) {
            set_error("HTTP request failed for " + std::string(url));
            return 2;
        }

        *out_body = dup_cstr(resp.body);
        if (!*out_body) { set_error("out of memory"); return 3; }
        return 0;
    } catch (const ScrapeError& e) {
        set_error(e.to_string());
        return 2;
    } catch (const std::exception& e) {
        set_error(e.what());
        return 2;
    }
}

const char* bullet_last_error(void) {
    return g_last_error.c_str();
}

void bullet_free(void* p) {
    std::free(p);
}

} // extern "C"
