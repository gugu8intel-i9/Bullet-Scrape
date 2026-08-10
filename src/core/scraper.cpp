#include "bullet_scrape/scraper.hpp"
#include "bullet_scrape/exceptions.hpp"
#include "bullet_scrape/mini_json.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <sstream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <future>
#include <csignal>

namespace bullet_scrape {

// ── Scraper::Impl ───────────────────────────────────────────────────────────

struct Scraper::Impl {
    ScraperConfig             cfg;
    std::unique_ptr<HTTPClient> client;
    std::atomic<bool>         cancelled{false};

    Impl() {
        struct sigaction sa{};
        sa.sa_handler = +[](int) { /* flag set via atomic in production */ };
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        sigaction(SIGINT, &sa, nullptr);
    }

    ScrapeResult run_impl() {
        ScrapeResult result;
        result.config_name = cfg.name;

        auto t0 = std::chrono::steady_clock::now();

        if (!client) {
            HTTPClient::Options opts;
            opts.max_concurrent    = cfg.limits.max_concurrent;
            opts.timeout_ms        = cfg.limits.timeout_ms;
            opts.proxy             = cfg.limits.proxy;
            opts.follow_redirects  = cfg.limits.follow_redirects;
            opts.max_redirects     = cfg.limits.max_redirects.value_or(5);
            opts.rate_limit_rps    = cfg.limits.requests_per_second;
            opts.user_agent        = cfg.user_agent;
            client = std::make_unique<HTTPClient>(opts);
        }

        auto urls = cfg.all_urls;
        if (urls.empty())
            throw config_error("no URLs to scrape (config may be misconfigured)");

        std::cerr << "[bullet-scrape] Starting: " << urls.size()
                  << " URL(s), " << cfg.limits.max_concurrent
                  << " concurrent\n";

        std::mutex                 result_mtx;
        std::vector<ScrapedResult> finished;
        finished.reserve(urls.size());
        std::atomic<int> done_count{0};
        int total = static_cast<int>(urls.size());

        // fetch_async blocks until every URL completes (bounded worker pool).
        auto on_done = [&](const HttpResponse& resp,
                           const std::string& url,
                           const ScrapeError& err) {
            ScrapedResult sr;
            sr.url = url;

            if (err.code != ErrorCode::Unknown || resp.status == 0) {
                sr.error       = err.to_string();
                sr.http_status = static_cast<int>(resp.status);
            } else {
                sr.http_status      = static_cast<int>(resp.status);
                sr.bytes_downloaded = resp.bytes;

                auto t_parse = std::chrono::steady_clock::now();
                try {
                    sr.records = ExtractionEngine().execute(cfg, resp.body, url);
                } catch (const std::exception& e) {
                    sr.error = "extract: " + std::string(e.what());
                }
                auto t1 = std::chrono::steady_clock::now();
                sr.latency = std::chrono::duration_cast<std::chrono::milliseconds>(
                    t1 - t_parse);
            }

            {
                std::lock_guard<std::mutex> lk(result_mtx);
                finished.push_back(std::move(sr));
            }

            int n = ++done_count;
            std::cerr << "\r[bullet-scrape] "
                      << n << "/" << total
                      << " complete   " << std::flush;
        };

        try {
            client->fetch_async(urls, on_done, cfg);
        } catch (const std::exception& e) {
            std::cerr << "\n[bullet-scrape] fatal: " << e.what() << "\n";
        }

        {
            std::lock_guard<std::mutex> lk(result_mtx);
            result.results = std::move(finished);
        }

        try {
            auto writer = make_writer(cfg.output);
            for (auto& r : result.results)
                for (auto& rec : r.records)
                    writer->write(rec);
            writer->flush();
        } catch (const std::exception& e) {
            std::cerr << "\n[bullet-scrape] output error: " << e.what() << "\n";
        }

        auto t1 = std::chrono::steady_clock::now();
        result.total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0);

        for (auto& r : result.results) {
            if (r.error)      result.failed++;
            else              result.succeeded++;
            result.total_bytes += r.bytes_downloaded;
        }

        std::cerr << "\n";
        return result;
    }
};

// ── Scraper public API ──────────────────────────────────────────────────────

Scraper::Scraper()  : pimpl_(new Impl()) {}
Scraper::~Scraper() = default;

void Scraper::load_config(const std::string& path) {
    pimpl_->cfg.load(path);
    pimpl_->cfg.validate();
}

void Scraper::load_json(const std::string& json_str) {
    try {
        json j = json::parse(json_str);
        pimpl_->cfg.load(j, "<string>");
        pimpl_->cfg.validate();
    } catch (const std::exception& e) {
        throw config_error("JSON parse error: " + std::string(e.what()));
    }
}

ScraperConfig& Scraper::config() {
    return pimpl_->cfg;
}

ScrapeResult Scraper::run() {
    return pimpl_->run_impl();
}

void Scraper::cancel() {
    pimpl_->cancelled = true;
}

} // namespace bullet_scrape
