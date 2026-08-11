#pragma once
#include "bullet_scrape/config.hpp"
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bullet_scrape {

// ── Raw HTML is the unit of work ────────────────────────────────────────────
// The parser only builds a DOM if XPath is requested. Otherwise a single-pass,
// quote-aware tag scan runs directly on the raw string — no DOM, no copies of
// the document beyond the matched elements.

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
        std::string_view text, const std::string& pattern);

    // ── Tier-2: tag-level extraction (no full DOM, just tag boundaries) ────

    // One indexed element: byte offsets into the source HTML + an id into
    // Document::tag_names (interned, lowercased).
    struct Element {
        uint32_t tag;      // id into Document::tag_names
        uint32_t open;     // position of '<' of the open tag
        uint32_t open_gt;  // position of '>' of the open tag
        uint32_t inner;    // first byte of the element's content
        uint32_t close;    // position of '<' of the close tag (== end if unclosed)
        uint32_t end;      // one past the '>' of the close tag
    };

    // A lightweight, single-pass index of every element in the document.
    // Built once per page and shared by all selector queries in `execute`.
    // (Offsets are 32-bit: documents up to 4 GiB.)
    struct Document {
        std::vector<Element> elements;   // emission (close) order
        std::deque<std::string> tag_names;  // interned tag-name storage
    };

    // Scan `html` once (quote-aware, handles comments, CDATA, raw-text
    // <script>/<style>, void and self-closing tags, implicitly-closed
    // elements) and index all elements.
    static Document parse_document(const std::string& html);

    // Collect the indexed elements matching a CSS-selector-lite pattern.
    // Elements are copied POD spans into the original `html` — no string
    // materialisation happens here; extraction reads views directly.
    // Supported: `tag`, `*`, `.class`, `#id`, `[attr]`, `[attr="v"]`, and
    // combinations; for compound selectors with combinators (`>`, `+`, `~`,
    // whitespace) the rightmost simple segment is used.
    static std::vector<Element> find_elements(
        const Document& doc, const std::string& html, const std::string& selector);

    // Extract attribute value from element HTML.
    // Scans tag attribute lists only (never text nodes); case-insensitive
    // attribute name; single/double/unquoted values. The first occurrence —
    // including descendant tags — wins, so `div.product` can yield the href
    // of a nested <a>.
    static std::optional<std::string> extract_attribute(
        std::string_view element_html, std::string_view attr);

    // Extract visible inner text: strips tags, comments and <script>/<style>
    // content, decodes HTML entities, collapses whitespace.
    static std::string extract_text(std::string_view element_html);

    // ── Transform & aggregate ───────────────────────────────────────────────

    static json apply_transforms(const std::vector<std::string>& values,
                                 const ExtractRule& rule,
                                 const std::string& base_url);

    static json apply_aggregate(const json& transformed,
                                const ExtractRule& rule,
                                size_t raw_count = 0);

    // ── Helper: resolve a leaf query against one element's HTML ─────────────

    json extract_leaf(const SubQuery& sub,
                      std::string_view element_html,
                      const std::string& base_url);
};

} // namespace bullet_scrape
