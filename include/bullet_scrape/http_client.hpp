#pragma once
#include "bullet_scrape/exceptions.hpp"
#include "bullet_scrape/config.hpp"
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>
#include <functional>
#include <chrono>

// ── libcurl compatibility ────────────────────────────────────────────────────
// When built with -DBULLET_HAVE_CURL (or -DHAVE_CURL) and linked against
// libcurl, the real CURLcode enum is used. Otherwise a minimal shim lets the
// rest of the codebase compile against the POSIX HTTP fallback.
#if defined(BULLET_HAVE_CURL) || defined(HAVE_CURL)
  #include <curl/curl.h>
  #ifndef HAVE_CURL
    #define HAVE_CURL 1
  #endif
#else
  #ifndef HAVE_CURL
enum CURLcode {
    CURLE_OK = 0,
    CURLE_COULDNT_CONNECT = 7,
    CURLE_COULDNT_RESOLVE_HOST = 6,
    CURLE_OPERATION_TIMEDOUT = 28,
    CURLE_FAILED_INIT = 2,
};
  #endif
#endif

namespace bullet_scrape {

// ── Response received from a single HTTP request ────────────────────────────
struct HttpResponse {
    std::string body;
    long        status = 0;
    std::string final_url;
    std::string content_type;
    size_t      bytes = 0;
    CURLcode    curl_code = CURLE_OK;
    // Parsed cookies (as Set-Cookie header values)
    std::vector<std::string> set_cookies;
};

// ── HTTPClient ──────────────────────────────────────────────────────────────
//
// High-performance HTTP engine:
//   • libcurl backend (when BULLET_HAVE_CURL): HTTPS, HTTP/2, gzip, keep-alive,
//     per-thread easy handles for lock-free connection reuse.
//   • POSIX sockets fallback: plain HTTP only.
//   • Bounded worker pool (max_concurrent) — never unbounded thread spawn.
//   • Optional global rate limiter (token bucket).
//
// Thread-safe: fetch() / fetch_async() may be called concurrently.
//
class HTTPClient {
public:
    struct Options {
        int max_concurrent = 4;
        std::chrono::milliseconds timeout_ms{30000};
        std::optional<std::string> proxy;
        std::optional<bool>    follow_redirects = true;
        int max_redirects = 5;
        std::optional<double>  rate_limit_rps;  // global rate limit
        std::string            user_agent = "BulletScrape/1.0";
    };

    explicit HTTPClient(const Options& opt);
    ~HTTPClient();

    // Fetch a single URL synchronously (uses a pooled/thread-local handle)
    HttpResponse fetch(const std::string& url,
                      const std::string& method   = "GET",
                      const std::string& body      = "",
                      const std::unordered_map<std::string, std::string>* headers = nullptr,
                      int max_retries = 0,
                      std::chrono::milliseconds retry_delay = std::chrono::milliseconds(1000));

    // Async batch: dispatch N URLs across a bounded worker pool, collect results.
    // `on_done` is called per result (may be on any worker thread).
    // Blocks until all URLs have completed.
    using FetchCallback = std::function<void(const HttpResponse&, const std::string& url, const ScrapeError&)>;
    void fetch_async(const std::vector<std::string>& urls,
                     FetchCallback                on_done,
                     const ScraperConfig&         cfg);

    void shutdown();

private:
    struct Impl;
    Impl* pimpl_;
};

} // namespace bullet_scrape
