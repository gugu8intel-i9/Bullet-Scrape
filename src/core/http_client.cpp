#include "bullet_scrape/http_client.hpp"
#include "bullet_scrape/posix_http.hpp"
#include <algorithm>
#include <cctype>
#include <thread>
#include <future>
#include <sstream>

namespace bullet_scrape {

// ── HTTPClient uses PosixHTTPClient as backend ───────────────────────────────
// To use libcurl instead (for HTTPS, gzip, HTTP/2), install libcurl-dev and
// rebuild with -DBULLET_HAVE_CURL=ON. The API is identical.

struct HTTPClient::Impl {
    Options          opts;
    PosixHTTPClient  posix_client;

    Impl(const Options& o) : opts(o),
        posix_client(PosixHTTPClient::Options{
            o.timeout_ms, o.user_agent, o.max_redirects, o.proxy.value_or("")}) {}

    // Perform one request synchronously
    HTTPResponse perform_one(const std::string& url,
                             const std::string& method,
                             const std::string& body,
                             const std::unordered_map<std::string, std::string>* headers,
                             int max_retries,
                             std::chrono::milliseconds retry_delay) {
        return posix_client.fetch(url, method, body, headers, max_retries, retry_delay);
    }
};

// ── HTTPClient public API ───────────────────────────────────────────────────

HTTPClient::HTTPClient(const Options& opt) : pimpl_(new Impl(opt)) {}
HTTPClient::~HTTPClient() { delete pimpl_; }

HttpResponse HTTPClient::fetch(const std::string& url,
                               const std::string& method,
                               const std::string& body,
                               const std::unordered_map<std::string, std::string>* headers,
                               int max_retries,
                               std::chrono::milliseconds retry_delay) {
    auto resp = pimpl_->perform_one(url, method, body, headers, max_retries, retry_delay);
    HttpResponse out;
    out.body         = std::move(resp.body);
    out.status       = resp.status;
    out.content_type = std::move(resp.content_type);
    out.set_cookies  = std::move(resp.set_cookies);
    out.final_url    = std::move(resp.final_url);
    out.bytes        = resp.bytes;
    out.curl_code    = resp.status == 0 ? CURLE_COULDNT_CONNECT : CURLE_OK;
    return out;
}

void HTTPClient::fetch_async(const std::vector<std::string>& urls,
                             FetchCallback                on_done,
                             const ScraperConfig&         cfg) {
    std::vector<std::future<void>> futures;
    for (auto& u : urls) {
        futures.push_back(std::async(std::launch::async, [this, u, on_done, &cfg]() {
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
                    on_done(resp, u, http_error(
                        resp.status ? (int)resp.status : -1, u,
                        "request failed"));
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
        }));
    }
    for (auto& f : futures) f.get();
}

void HTTPClient::shutdown() {
    // No-op for PosixHTTPClient backend.
}

} // namespace bullet_scrape
