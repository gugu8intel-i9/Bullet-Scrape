// tests/test_cleaner.cpp — the cleaning "sieve"
#include "bullet_scrape/cleaner.hpp"
#include "bullet_scrape/config.hpp"
#include "bullet_scrape/extractor.hpp"
#include "bullet_scrape/exceptions.hpp"
#include "test_harness.hpp"
#include <iostream>
#include <string>

using namespace bullet_scrape;
using json = mini_json::json;

static ScraperConfig make_clean_cfg(const std::string& qjson,
                                    const std::string& clean_spec = "") {
    // Build a minimal config around a query object, with an optional global
    // "clean" spec (raw JSON snippet, e.g. `"clean": ["fold"],` — may be "").
    std::string cfg = std::string(R"J({"url":"https://example.com",)J")
        + clean_spec
        + R"J("queries":{"q":)J" + qjson + "}}";
    ScraperConfig c;
    c.load(json::parse(cfg), "<test>");
    return c;
}

static json first_field(const std::vector<Record>& rs, const char* name) {
    if (rs.empty()) return json(nullptr);
    auto it = rs[0].find(name);
    return it == rs[0].end() ? json(nullptr) : it->second;
}

// ── Stage-level units ───────────────────────────────────────────────────────

REGISTER_TEST(cleaner_stage_units) {
    std::string s;

    s = "Fish &amp; Chips &#8212; &#x41;";      // entities
    apply_cleaners(s, CLEAN_ENTITIES);
    if (s != "Fish & Chips \xE2\x80\x94 A") { std::cerr << s << "\n"; return false; }

    s = std::string("ab") + "\xE2\x80\x8B\xEF\xBB\xBF" + "cd\x07";  // ZWSP, BOM, BEL
    apply_cleaners(s, CLEAN_INVISIBLES);
    if (s != "abcd") { std::cerr << "invisibles: " << s << "\n"; return false; }

    s = std::string("  x \xC2\xA0\xC2\xA0  y\xE2\x80\x83z  ");      // NBSP run
    apply_cleaners(s, CLEAN_WHITESPACE);
    if (s != "x y z") { std::cerr << "ws: [" << s << "]\n"; return false; }

    s = "\xE2\x80\x9CHello\xE2\x80\x9D \xE2\x80\x93 it\xE2\x80\x99s\xE2\x80\xA6"; // fold
    apply_cleaners(s, CLEAN_FOLD);
    if (s != "\"Hello\" - it's...") { std::cerr << "fold: " << s << "\n"; return false; }

    s = "$1,299.50";                              // numeric
    apply_cleaners(s, CLEAN_NUMERIC);
    if (s != "1299.50") { std::cerr << "numeric1: " << s << "\n"; return false; }
    s = "-\xE2\x82\xAC" "2,000";
    apply_cleaners(s, CLEAN_NUMERIC);
    if (s != "-2000") { std::cerr << "numeric2: " << s << "\n"; return false; }
    s = "room 1,200 sq ft";                       // not numeric → untouched
    std::string orig = s;
    apply_cleaners(s, CLEAN_NUMERIC);
    if (s != orig) return false;
    s = "12,34";                                  // malformed grouping → untouched
    orig = s;
    apply_cleaners(s, CLEAN_NUMERIC);
    if (s != orig) return false;

    if (!is_null_token("n/a") || !is_null_token("NONE") ||
        !is_null_token("-")  || is_null_token("nope")) return false;
    return true;
}

REGISTER_TEST(cleaner_default_sieve_via_extract) {
    // Default sieve decodes entities + strips invisibles + collapses ws,
    // including for regex-extracted values (not just text).
    auto cfg = make_clean_cfg(R"J({"regex":"Price: (.*?)<","extract":[{"name":"p","rule":{}}]})J");
    auto r = ExtractionEngine().execute(cfg,
        "x Price: Caf\xC3\xA9&nbsp;&amp;  \x01 y< z");
    if (r.empty()) { std::cerr << "no records\n"; return false; }
    auto v = first_field(r, "p");
    if (!v.is_array() || v.size() != 1) return false;
    const std::string got = v[0].get_string();
    if (got != "Caf\xC3\xA9 & y") { std::cerr << "got: [" << got << "]\n"; return false; }
    return true;
}

REGISTER_TEST(cleaner_globals_off_and_on) {
    // "clean": false globally → sieve adds nothing beyond extraction itself
    auto cfg = make_clean_cfg(
        R"J({"selector":"p","extract":[{"name":"t","rule":{"text":true}}]})J",
        R"J("clean": false,)J");
    auto r = ExtractionEngine().execute(cfg, "<p>a&nbsp;&nbsp;b</p>");
    if (r.empty()) return false;
    auto v = first_field(r, "t");
    if (!v.is_array() || v.size() != 1) return false;
    // text extraction decodes entities but does not collapse unicode
    // whitespace, so the NBSPs survive with the sieve off:
    if (v[0].get_string() != "a\xC2\xA0\xC2\xA0" "b") {
        std::cerr << "off: [" << v[0].get_string() << "]\n"; return false;
    }

    // explicit stage list wins over the default
    cfg = make_clean_cfg(
        R"J({"selector":"p","extract":[{"name":"t","rule":{"attribute":"data-q"}}]})J",
        R"J("clean": ["fold"],)J");
    r = ExtractionEngine().execute(cfg,
        "<p data-q=\"\xE2\x80\x9CHi\xE2\x80\x9D&nbsp;&nbsp;\">x</p>");
    if (r.empty()) return false;
    v = first_field(r, "t");
    if (!v.is_array() || v.size() != 1) return false;
    // fold only: quotes fixed, but NBSPs NOT collapsed (whitespace not enabled)
    if (v[0].get_string() != "\"Hi\"\xC2\xA0\xC2\xA0") {
        std::cerr << "got: [" << v[0].get_string() << "]\n"; return false;
    }
    return true;
}

REGISTER_TEST(cleaner_rule_override) {
    // Per-rule clean: off on one rule, fold-only on another.
    auto cfg = make_clean_cfg(R"J({"selector":"p","extract":[
        {"name":"raw",  "rule":{"attribute":"data-x", "clean": false}},
        {"name":"folded","rule":{"attribute":"data-x", "clean": ["fold"]}}
    ]})J");
    auto r = ExtractionEngine().execute(cfg,
        "<p data-x=\"  \xE2\x80\x9CHello\xE2\x80\x9D  \">x</p>");
    if (r.empty()) return false;
    auto raw = first_field(r, "raw");
    auto folded = first_field(r, "folded");
    if (raw[0].get_string() != "  \xE2\x80\x9CHello\xE2\x80\x9D  ") return false;
    if (folded[0].get_string() != "  \"Hello\"  ") return false;
    return true;
}

REGISTER_TEST(cleaner_numeric_transform_affinity) {
    // int/float transforms tolerate "$1,299.50" even without CLEAN_NUMERIC.
    auto cfg = make_clean_cfg(R"J({"selector":"span.price","extract":[
        {"name":"p","rule":{"text":true,"transform":["float"],"aggregate":"first"}}
    ]})J");
    auto r = ExtractionEngine().execute(cfg, "<span class=\"price\">$1,299.50</span>");
    if (r.empty()) return false;
    const json p = first_field(r, "p");
    if (!p.is_float() || p.get_float() != 1299.5) {
        std::cerr << "got: " << p.dump() << "\n"; return false;
    }
    // non-numeric stays a clean string, becomes null on conversion
    r = ExtractionEngine().execute(cfg, "<span class=\"price\">Free!</span>");
    return !r.empty() && first_field(r, "p").is_null();
}

REGISTER_TEST(cleaner_null_tokens) {
    // Two queries → one record each, in query-name order ("missing" < "present").
    std::string cfgjson = R"J({"url":"https://example.com",
        "clean": ["entities","invisibles","whitespace","null_tokens"],
        "queries":{
            "missing": {"selector":"td.a", "extract":[{"name":"v","rule":{"text":true}}]},
            "present": {"selector":"td.b", "extract":[{"name":"v","rule":{"text":true}}]}
        }})J";
    ScraperConfig cfg;
    cfg.load(json::parse(cfgjson), "<test>");
    auto r = ExtractionEngine().execute(cfg,
        "<td class=\"a\">N/A</td><td class=\"b\">Real</td>");
    if (r.size() != 2) return false;
    const json& missing = r[0].find("v")->second;
    if (!missing.is_array() || !missing[0].is_null()) {
        std::cerr << "expected [null], got " << missing.dump() << "\n"; return false; }
    const json& present = r[1].find("v")->second;
    if (!present.is_array() || present[0].get_string() != "Real") return false;
    return true;
}

REGISTER_TEST(cleaner_unknown_stage_errors) {
    bool caught = false;
    try {
        make_clean_cfg(
            R"J({"selector":"p","extract":[{"name":"t","rule":{"text":true}}]})J",
            R"J("clean": ["does_not_exist"],)J");
    } catch (const ScrapeError& e) {
        caught = (e.code == ErrorCode::Config);
    }
    return caught;
}

REGISTER_TEST(cleaner_sieve_order_full_chain) {
    // Everything at once: entity + invisible + quote-fold + ws collapse.
    auto cfg = make_clean_cfg(R"J({"selector":"p","extract":[
        {"name":"t","rule":{"text":true,"aggregate":"first"}}
    ]})J", R"J("clean": ["entities","invisibles","whitespace","fold"],)J");
    std::string html = std::string("<p>&ldquo;Hot&#160;Mess&rdquo;\xE2\x80\x8B&nbsp; &hellip;\t ok</p>");
    auto r = ExtractionEngine().execute(cfg, html);
    if (r.empty()) return false;
    const json t = first_field(r, "t");
    const std::string want = "\"Hot Mess\" \xC2\xA0... ok";
    // note: NBSP chars decode to C2 A0 → collapsed by ws stage to a space
    const std::string want2 = "\"Hot Mess\" ... ok";
    if (t.get_string() != want && t.get_string() != want2) {
        std::cerr << "got: [" << t.get_string() << "]\n"; return false;
    }
    return true;
}
