#pragma once
// ============================================================================
//  cleaner — the "sieve": a configurable chain of cleanup stages that every
//  scraped value passes through before transforms/aggregation. Each stage is
//  a single O(n) pass with no regex and at most one output allocation; the
//  three default stages are fused into one pass.
// ============================================================================
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace bullet_scrape {

// ── Cleaning stages ─────────────────────────────────────────────────────────
// Bit flags — combine freely; application order is always the sieve order:
//   entities → invisibles → fold → numeric → whitespace
enum CleanStage : uint32_t {
    CLEAN_ENTITIES   = 1u << 0,  // decode HTML entities (&amp;, &#233;, &euro;)
    CLEAN_INVISIBLES = 1u << 1,  // strip zero-width/format + C0/C1 control chars
    CLEAN_WHITESPACE = 1u << 2,  // collapse all unicode whitespace, trim ends
    CLEAN_FOLD       = 1u << 3,  // fold smart quotes/dashes/ellipsis to ASCII
    CLEAN_NUMERIC    = 1u << 4,  // "$1,299.50" → "1299.50" (numeric strings only)
    CLEAN_NULL_TOKEN = 1u << 5,  // "n/a", "none", "-" … → JSON null
};

// The default sieve, enabled unless the config says otherwise.
constexpr uint32_t CLEAN_DEFAULT =
    CLEAN_ENTITIES | CLEAN_INVISIBLES | CLEAN_WHITESPACE;

// Look up a stage by config name ("entities", "whitespace", "invisibles",
// "fold", "numeric", "null_tokens"); nullopt for unknown names.
std::optional<uint32_t> clean_stage_by_name(std::string_view name);

// Run all stages present in `mask` over `value`, in sieve order.
void apply_cleaners(std::string& value, uint32_t mask);

// True if (the already-trimmed) value is a "no data" token: "", "n/a", "na",
// "none", "null", "nil", "unknown", "-", "--", "—" (case-insensitive).
bool is_null_token(std::string_view v);

// Canonicalise a numeric-looking string in place: strips leading currency
// symbols ($ € £ ¥ ₹ ₽ ₩ ฿), grouped thousands separators, and a trailing %;
// validates the rest so "room 1,200 sq ft" is left untouched.
// Returns false (leaving `v` unchanged) when the string is not numeric.
bool clean_numeric_inplace(std::string& v);

// Decode one HTML entity starting at s[pos] == '&': appends the decoded bytes
// to `out`, advances `pos` past the ';', returns true. Leaves both untouched
// (returns false) when no valid entity is present.
bool html_entity_decode(std::string_view s, size_t& pos, std::string& out);

} // namespace bullet_scrape
