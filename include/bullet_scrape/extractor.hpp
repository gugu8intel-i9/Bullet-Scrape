#pragma once
#include "bullet_scrape/cleaner.hpp"
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
    // Queries are evaluated in name order, so record ordering is
    // deterministic even though the config container is unordered.
    //
    // `base_url` is used to resolve relative URLs in `urljoin` transforms.
    // `is_html = false` switches to bulk text mode for regex queries over
    // non-HTML payloads (JSON/CSV/logs): values are not tag-stripped or
    // entity-decoded during extraction, and '>' carries no special meaning.
    //
    // Every extracted value passes through the cleaning "sieve" (see
    // cleaner.hpp): ScraperConfig::clean is the default stage mask, a rule's
    // own "clean" overrides it. The sieve runs before transforms.
    std::vector<Record> execute(
        const ScraperConfig&         cfg,
        const std::string&           html,
        const std::string&           base_url = "",
        bool                          is_html = true);

    // Convenience: scalar query → json value
    json execute_scalar(
        const CollectionQuery&       query,
        const std::string&           html,
        bool                          is_html = true);

private:
    // ── Tier-1.5: tag-level extraction (no full DOM, just tag boundaries) ───

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
        std::vector<Element> elements;      // emission (close) order
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
    // The element's own open tag is consulted first; if it lacks the
    // attribute, descendant tags are scanned and the first occurrence wins,
    // so `div.product` can yield the href of a nested <a>. Case-insensitive
    // name; single/double/unquoted values; entities decoded only when present.
    static std::optional<std::string> extract_attribute(
        std::string_view element_html, std::string_view attr);

    // Extract visible inner text: strips tags, comments and <script>/<style>
    // content, decodes HTML entities, collapses whitespace. Has a zero-alloc
    // fast path for text that is already clean. (is_html=false → as-is.)
    static std::string extract_text(std::string_view element_html,
                                    bool is_html = true);

    // ── Transform & aggregate ───────────────────────────────────────────────

    // (values by value: they are sieved and transformed in place, then moved
    // into json). `clean_mask` is the resolved cleaning-stage bitmask for the
    // rule (0 → sieve off; see cleaner.hpp).
    static json apply_transforms(std::vector<std::string> values,
                                 const ExtractRule& rule,
                                 const std::string& base_url,
                                 uint32_t clean_mask);

    static json apply_aggregate(const json& transformed,
                                const ExtractRule& rule,
                                size_t raw_count = 0);

    // ── Helper: resolve a leaf query against one element's HTML ─────────────

    json extract_leaf(const SubQuery& sub,
                      std::string_view element_html,
                      const std::string& base_url,
                      bool is_html = true,
                      uint32_t clean_mask = CLEAN_DEFAULT);
};

} // namespace bullet_scrape
