#pragma once
#include "bullet_scrape/config.hpp"
#include <string>
#include <vector>
#include <regex>
#include <unordered_map>
#include <optional>

namespace bullet_scrape {

// ── Raw HTML is the unit of work ────────────────────────────────────────────
// The parser only builds a DOM if XPath is requested. Otherwise regex runs
// directly on the raw string.

class ExtractionEngine {
public:
    // Run all configured queries against `html`, returning one Record per
    // matched element (for collection queries) or a single Record (scalar).
    //
    // `base_url` is used to resolve relative URLs in `urljoin` transforms.
    std::vector<Record> execute(
        const ScraperConfig&        cfg,
        const std::string&           html,
        const std::string&           base_url = "");

    // Convenience: scalar query → json value
    json execute_scalar(
        const CollectionQuery&      query,
        const std::string&          html);

private:
    // ── Tier-1: regex-based extraction ─────────────────────────────────────

    // Find all regex matches in text
    static std::vector<std::string> regex_find_all(
        const std::string& text, const std::string& pattern);

    // Find all captures from a regex with N capture groups
    static std::vector<std::vector<std::string>> regex_find_captures(
        const std::string& text, const std::string& pattern, int groups);

    // Extract attribute value from matched element
    // `match` is the full element HTML (e.g. <a href="/p/1">...</a>)
    static std::optional<std::string> extract_attribute(
        const std::string& element_html, const std::string& attr);

    // Extract inner text from element (strip tags)
    static std::string extract_text(const std::string& element_html);

    // ── Tier-2: tag-level extraction (no full DOM, just tag boundaries) ───

    struct TagMatch {
        std::string open_tag;   // e.g. <a href="/p/1" class="x">
        std::string inner_html; // content between tags
        std::string close_tag;  // e.g. </a>
        std::string full;       // open + inner + close
        std::string tag_name;
    };

    // Split HTML into tag matches for a given CSS-selector-like pattern.
    // This is a simple implementation: selectors like `tag`, `.class`,
    // `#id`, `tag.class`, `tag#id` are handled.
    // `a[href]` and attribute presence checks are supported.
    // For anything complex, fall back to regex or XPath.
    static std::vector<TagMatch> find_elements(
        const std::string& html, const std::string& selector);

    // ── Transform & aggregate ───────────────────────────────────────────────

    static json apply_transforms(const std::vector<std::string>& values,
                                 const ExtractRule& rule,
                                 const std::string& base_url);

    static json apply_aggregate(const json& transformed,
                                const ExtractRule& rule,
                                size_t raw_count = 0);

    // ── Helper: resolve a leaf query against one element's HTML ─────────────

    json extract_leaf(const SubQuery& sub,
                      const std::string& element_html,
                      const std::string& base_url);
};

} // namespace bullet_scrape
