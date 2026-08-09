#pragma once
#include "bullet_scrape/config.hpp"
#include "bullet_scrape/http_client.hpp"
#include "bullet_scrape/extractor.hpp"
#include "bullet_scrape/output.hpp"
#include <memory>
#include <vector>

namespace bullet_scrape {

// ── Scraper ─────────────────────────────────────────────────────────────────
//
// The top-level orchestrator. Configure it with a ScraperConfig, then call
// run(). Results are written to the configured output automatically.
//
// Usage:
//   Scraper scraper;
//   scraper.load_config("config.json");
//   auto result = scraper.run();
//   std::cout << format_summary(result) << "\n";
//

class Scraper {
public:
    Scraper();
    ~Scraper();

    // Load and validate a config file
    void load_config(const std::string& path);

    // Load from a JSON string (for programmatic use)
    void load_json(const std::string& json_str);

    // Get the active config
    ScraperConfig&       config();
    const ScraperConfig& config() const;

    // Execute the scrape. Blocks until all URLs are processed.
    ScrapeResult run();

    // Cancel a running scrape (best-effort)
    void cancel();

private:
    struct Impl;
    std::unique_ptr<Impl> pimpl_;
};

} // namespace bullet_scrape
