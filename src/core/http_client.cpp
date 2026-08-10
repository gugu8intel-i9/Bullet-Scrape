#include "bullet_scrape/http_client.hpp"
#include "bullet_scrape/posix_http.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <future>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

// ── Optional libcurl backend ─────────────────────────────────────────────────
// Define BULLET_HAVE_CURL (or HAVE_CURL) and link -lcurl for HTTPS/HTTP2/gzip.
#if defined(BULLET_HAVE_CURL) || defined(HAVE_CURL)
  #define BULLET_USE_CURL 1
  #include <curl/curl.h>
#else
  #define BULLET_USE_CURL 0
#endif

namespace bullet_scrape {

// ── Rate limiter (token bucket, shared across workers) ───────────────────────

class RateLimiter {
public:
    explicit RateLimiter(double rps)
        : interval_(rps > 0.0
              ? std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::duration<double>(1.0 / rps))
              : std::chrono::nanoseconds::zero()) {}

    void acquire() {
        if (interval_.count() <= 0) return;
        std::unique_lock<std::mutex> lk(mu_);
        auto now = std::chrono::steady_clock::now();
        if (next_ <= now) {
            next_ = now + interval_;
            return;
        }
        auto wait_for = next_ - now;
        next_ += interval_;
        lk.unlock();
        std::this_thread::sleep_for(wait_for);
    }

private:
    std::mutex mu_;
    std::chrono::nanoseconds interval_;
    std::chrono::steady_clock::time_point next_{std::chrono::steady_clock::now()};
};

// ── Bounded worker pool ──────────────────────────────────────────────────────
// Caps concurrency properly (std::async alone can spawn unbounded threads).

class WorkerPool {
public:
    explicit WorkerPool(int n)
        : n_(std::max(1, std::min(n, 256))) {
        workers_.reserve(static_cast<size_t>(n_));
        for (int i = 0; i < n_; ++i)
            workers_.emplace_back([this] { loop(); });
    }

    ~WorkerPool() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& t : workers_)
            if (t.joinable()) t.join();
    }

    template <typename F>
    void submit(F&& f) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            jobs_.emplace_back(std::forward<F>(f));
            ++pending_;
        }
        cv_.notify_one();
    }

    void wait() {
        std::unique_lock<std::mutex> lk(mu_);
        done_cv_.wait(lk, [this] { return pending_ == 0; });
    }

private:
    void loop() {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait(lk, [this] { return stop_ || !jobs_.empty(); });
                if (stop_ && jobs_.empty()) return;
                job = std::move(jobs_.front());
                jobs_.pop_front();
            }
            try { job(); } catch (...) { /* swallow — callers handle errors */ }
            {
                std::lock_guard<std::mutex> lk(mu_);
                --pending_;
                if (pending_ == 0) done_cv_.notify_all();
            }
        }
    }

    int n_;
    std::vector<std::thread> workers_;
    std::deque<std::function<void()>> jobs_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::condition_variable done_cv_;
    bool stop_ = false;
    int pending_ = 0;
};

// ── curl write callback ──────────────────────────────────────────────────────

#if BULLET_USE_CURL
static size_t curl_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    size_t total = size * nmemb;
    out->append(ptr, total);
    return total;
}

static size_t curl_header_cb(char* buffer, size_t size, size_t nitems, void* userdata) {
    auto* resp = static_cast<HttpResponse*>(userdata);
    size_t total = size * nitems;
    std::string line(buffer, total);
    // Strip CRLF
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
        line.pop_back();
    if (line.empty()) return total;

    auto colon = line.find(':');
    if (colon == std::string::npos) return total;

    std::string key = line.substr(0, colon);
    std::string val = line.substr(colon + 1);
    while (!val.empty() && val.front() == ' ') val.erase(val.begin());

    std::string lk = key;
    for (auto& c : lk) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if (lk == "content-type")
        resp->content_type = val;
    else if (lk == "set-cookie")
        resp->set_cookies.push_back(val);

    return total;
}

// One CURL* per worker thread — connection reuse via CURLOPT_TCP_KEEPALIVE
// and the process-wide multi DNS / connection cache when using the share.
struct CurlHandle {
    CURL* easy = nullptr;

    CurlHandle() {
        easy = curl_easy_init();
    }
    ~CurlHandle() {
        if (easy) curl_easy_cleanup(easy);
    }
    CurlHandle(const CurlHandle&) = delete;
    CurlHandle& operator=(const CurlHandle&) = delete;
};

static thread_local CurlHandle t_curl;

static void curl_global_once() {
    static std::once_flag once;
    std::call_once(once, [] {
        curl_global_init(CURL_GLOBAL_DEFAULT);
    });
}
#endif // BULLET_USE_CURL

// ── Impl ─────────────────────────────────────────────────────────────────────

struct HTTPClient::Impl {
    Options opts;
#if !BULLET_USE_CURL
    // Fallback: one PosixHTTPClient template; each call is independent.
    // (PosixHTTPClient is not thread-safe for shared state, but has none.)
#endif
    std::unique_ptr<RateLimiter> limiter;

    explicit Impl(const Options& o) : opts(o) {
#if BULLET_USE_CURL
        curl_global_once();
#endif
        if (o.rate_limit_rps && *o.rate_limit_rps > 0.0)
            limiter = std::make_unique<RateLimiter>(*o.rate_limit_rps);
    }

    HttpResponse perform_one(const std::string& url,
                             const std::string& method,
                             const std::string& body,
                             const std::unordered_map<std::string, std::string>* headers,
                             int max_retries,
                             std::chrono::milliseconds retry_delay) {
        HttpResponse last;
        for (int attempt = 0; ; ++attempt) {
            if (limiter) limiter->acquire();
            last = do_fetch(url, method, body, headers);
            bool retryable = (last.status == 429) ||
                             (last.status >= 500 && last.status < 600) ||
                             (last.status == 0 && last.curl_code != CURLE_OK);
            if (!retryable || attempt >= max_retries)
                return last;
            std::this_thread::sleep_for(retry_delay);
        }
    }

    HttpResponse do_fetch(const std::string& url,
                          const std::string& method,
                          const std::string& body,
                          const std::unordered_map<std::string, std::string>* headers) {
#if BULLET_USE_CURL
        return do_fetch_curl(url, method, body, headers);
#else
        return do_fetch_posix(url, method, body, headers);
#endif
    }

#if BULLET_USE_CURL
    HttpResponse do_fetch_curl(const std::string& url,
                               const std::string& method,
                               const std::string& body,
                               const std::unordered_map<std::string, std::string>* headers) {
        HttpResponse resp;
        CURL* easy = t_curl.easy;
        if (!easy) {
            resp.curl_code = CURLE_FAILED_INIT;
            return resp;
        }

        // Reset previous transfer state but keep connections alive in the handle
        curl_easy_reset(easy);

        std::string response_body;
        response_body.reserve(64 * 1024);

        curl_easy_setopt(easy, CURLOPT_URL, url.c_str());
        curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, curl_write_cb);
        curl_easy_setopt(easy, CURLOPT_WRITEDATA, &response_body);
        curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, curl_header_cb);
        curl_easy_setopt(easy, CURLOPT_HEADERDATA, &resp);
        curl_easy_setopt(easy, CURLOPT_USERAGENT, opts.user_agent.c_str());
        curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS,
                         static_cast<long>(opts.timeout_ms.count()));
        curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS,
                         static_cast<long>(std::min<int64_t>(opts.timeout_ms.count(), 15000)));
        curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION,
                         (opts.follow_redirects.value_or(true) ? 1L : 0L));
        curl_easy_setopt(easy, CURLOPT_MAXREDIRS, static_cast<long>(opts.max_redirects));
        curl_easy_setopt(easy, CURLOPT_ACCEPT_ENCODING, ""); // enable all supported
        curl_easy_setopt(easy, CURLOPT_TCP_KEEPALIVE, 1L);
        curl_easy_setopt(easy, CURLOPT_TCP_KEEPIDLE, 60L);
        curl_easy_setopt(easy, CURLOPT_TCP_KEEPINTVL, 30L);
        curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L); // thread-safe DNS
#if LIBCURL_VERSION_NUM >= 0x073E00
        // HTTP/2 multiplex when available (7.62.0+)
        curl_easy_setopt(easy, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
#endif
        // Prefer IPv4 first to avoid slow dual-stack stalls on some hosts
        curl_easy_setopt(easy, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_WHATEVER);

        if (opts.proxy && !opts.proxy->empty())
            curl_easy_setopt(easy, CURLOPT_PROXY, opts.proxy->c_str());

        // Method + body
        std::string m = method;
        for (auto& c : m) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (m == "POST") {
            curl_easy_setopt(easy, CURLOPT_POST, 1L);
            curl_easy_setopt(easy, CURLOPT_POSTFIELDS, body.data());
            curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
        } else if (m == "PUT" || m == "PATCH" || m == "DELETE") {
            curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, m.c_str());
            if (!body.empty()) {
                curl_easy_setopt(easy, CURLOPT_POSTFIELDS, body.data());
                curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
            }
        } else {
            curl_easy_setopt(easy, CURLOPT_HTTPGET, 1L);
        }

        struct curl_slist* hdr_list = nullptr;
        if (headers) {
            for (const auto& [k, v] : *headers) {
                std::string line = k + ": " + v;
                hdr_list = curl_slist_append(hdr_list, line.c_str());
            }
        }
        // Ask for keep-alive explicitly
        hdr_list = curl_slist_append(hdr_list, "Connection: keep-alive");
        if (hdr_list)
            curl_easy_setopt(easy, CURLOPT_HTTPHEADER, hdr_list);

        CURLcode rc = curl_easy_perform(easy);

        if (hdr_list) curl_slist_free_all(hdr_list);

        resp.curl_code = rc;
        resp.body = std::move(response_body);
        resp.bytes = resp.body.size();

        if (rc == CURLE_OK) {
            long code = 0;
            curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &code);
            resp.status = code;
            char* effective = nullptr;
            if (curl_easy_getinfo(easy, CURLINFO_EFFECTIVE_URL, &effective) == CURLE_OK && effective)
                resp.final_url = effective;
            else
                resp.final_url = url;
        } else {
            resp.status = 0;
            resp.final_url = url;
        }
        return resp;
    }
#else
    HttpResponse do_fetch_posix(const std::string& url,
                                const std::string& method,
                                const std::string& body,
                                const std::unordered_map<std::string, std::string>* headers) {
        PosixHTTPClient::Options po;
        po.timeout_ms    = opts.timeout_ms;
        po.user_agent    = opts.user_agent;
        po.max_redirects = opts.max_redirects;
        po.proxy         = opts.proxy.value_or("");
        PosixHTTPClient client(po);

        // Retries are handled by perform_one; pass 0 here.
        auto r = client.fetch(url, method, body, headers, /*max_retries=*/0);

        HttpResponse out;
        out.body         = std::move(r.body);
        out.status       = r.status;
        out.content_type = std::move(r.content_type);
        out.set_cookies  = std::move(r.set_cookies);
        out.final_url    = std::move(r.final_url);
        out.bytes        = r.bytes;
        out.curl_code    = (r.status == 0) ? CURLE_COULDNT_CONNECT : CURLE_OK;
        return out;
    }
#endif
};

// ── HTTPClient public API ────────────────────────────────────────────────────

HTTPClient::HTTPClient(const Options& opt) : pimpl_(new Impl(opt)) {}
HTTPClient::~HTTPClient() { delete pimpl_; }

HttpResponse HTTPClient::fetch(const std::string& url,
                               const std::string& method,
                               const std::string& body,
                               const std::unordered_map<std::string, std::string>* headers,
                               int max_retries,
                               std::chrono::milliseconds retry_delay) {
    return pimpl_->perform_one(url, method, body, headers, max_retries, retry_delay);
}

void HTTPClient::fetch_async(const std::vector<std::string>& urls,
                             FetchCallback                on_done,
                             const ScraperConfig&         cfg) {
    const int workers = std::max(1, pimpl_->opts.max_concurrent);
    WorkerPool pool(workers);

    for (const auto& u : urls) {
        pool.submit([this, u, on_done, &cfg]() {
            try {
                auto resp = fetch(
                    u,
                    cfg.method,
                    cfg.body,
                    cfg.headers.empty() ? nullptr : &cfg.headers,
                    cfg.limits.max_retries,
                    cfg.limits.retry_delay_ms
                );
                if (resp.status == 0 || resp.curl_code != CURLE_OK) {
                    std::string detail = "request failed";
#if BULLET_USE_CURL
                    if (resp.curl_code != CURLE_OK)
                        detail = curl_easy_strerror(resp.curl_code);
#endif
                    on_done(resp, u, http_error(
                        resp.status ? static_cast<int>(resp.status) : -1, u, detail));
                } else {
                    on_done(resp, u, ScrapeError{ErrorCode::Unknown, ""});
                }
            } catch (const ScrapeError& e) {
                HttpResponse empty;
                on_done(empty, u, e);
            } catch (const std::exception& e) {
                HttpResponse empty;
                on_done(empty, u, config_error(e.what()));
            }
        });
    }
    pool.wait();
}

void HTTPClient::shutdown() {
    // Handles are thread-local; nothing global to tear down per-client.
}

} // namespace bullet_scrape
