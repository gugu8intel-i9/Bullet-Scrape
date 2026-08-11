// tests/test_regex_engine.cpp — the fast-pattern engine must produce
// byte-identical results to std::regex (ECMAScript) for every recognised
// shape, and the seeded std::regex fallback must match an unseeded scan.
//
// Verification strategy: run each pattern through the public execute() API
// (sieve disabled so values are compared raw) and against std::regex_iterator
// as the reference, over a fixed corpus plus a deterministic fuzz.
#include "bullet_scrape/config.hpp"
#include "bullet_scrape/extractor.hpp"
#include "test_harness.hpp"
#include <iostream>
#include <regex>
#include <string>
#include <vector>

using namespace bullet_scrape;
using json = mini_json::json;

namespace {

// Reference matcher: std::regex iteration. For patterns with a group, the
// group's text ("" if not participating); otherwise the whole match.
std::vector<std::string> ref_matches(const std::string& hay, const std::string& pat) {
    std::vector<std::string> out;
    const std::regex re(pat, std::regex::ECMAScript);
    for (std::sregex_iterator it(hay.begin(), hay.end(), re), end; it != end; ++it) {
        const auto& m = *it;
        if (m.size() > 1)
            out.push_back(m[1].matched ? m[1].str() : std::string());
        else
            out.push_back(m[0].str());
    }
    return out;
}

// Engine matcher: regex query, one record per match, field "g" = [value].
std::vector<std::string> engine_matches(const std::string& hay, const std::string& pat) {
    const std::string cfgjson =
        std::string("{\"url\":\"https://example.com\",\"clean\":false,"
                    "\"queries\":{\"q\":{\"regex\":") + json(pat).dump() +
        ",\"extract\":[{\"name\":\"g\",\"rule\":{}}]}}}";
    ScraperConfig cfg;
    cfg.load(json::parse(cfgjson), "<test>");
    auto records = ExtractionEngine().execute(cfg, hay);
    std::vector<std::string> out;
    for (const auto& r : records) {
        auto it = r.find("g");
        if (it == r.end() || !it->second.is_array()) continue;
        for (size_t i = 0; i < it->second.size(); ++i) {
            const json& v = it->second[i];
            out.push_back(v.is_string() ? v.get_string() : v.dump());
        }
    }
    return out;
}

bool equivalent(const std::string& hay, const std::string& pat) {
    const auto want = ref_matches(hay, pat);
    const auto got  = engine_matches(hay, pat);
    if (want == got) return true;
    std::cerr << "MISMATCH pattern: " << json(pat).dump() << "\n  haystack: "
              << json(hay).dump() << "\n  want " << want.size() << ":";
    for (const auto& w : want) std::cerr << " [" << w << "]";
    std::cerr << "\n  got " << got.size() << ":";
    for (const auto& g : got) std::cerr << " [" << g << "]";
    std::cerr << "\n";
    return false;
}

} // anonymous namespace

REGISTER_TEST(regex_engine_fast_shapes) {
    // One haystack per shape, built to stress backtracking edges.
    struct Case { const char* pat; const char* hay; };
    const Case cases[] = {
        // pure literal
        {"in-stock", "in-stock out-of-stock in-stock"},
        // literal + [^x]+ + literal (greedy negclass with suffix backtrack)
        {"class=\"([^\"]+)\"", "a class=\"one\" b class=\"two three\" c"},
        // [^x]* — may capture empty
        {"#([^\"]*)\"", "x#\" y#abc\" z##\"\""},
        // single [^x] (no quantifier)
        {"x([^,])y", "xay x,y xby"},
        // bounded quantifier {2,4}
        {"id=([^<]{2,4})<", "id=ab< id=abcde< id=abcd<"},
        // lazy any with literal suffix
        {"<p>(.*?)</p>", "<p>a</p><p>b</p> <p></p><p>x</p>"},
        // greedy any spans inner delimiters
        {"<td>(.*)</td>", "<td>a</td><td>b</td>"},
        // literal anchors with regex escapes (identity escapes folded)
        {"\\$([0-9]+\\.[0-9]{2}) <", "$1.99 < $2.50 <"},
    };
    for (const auto& c : cases)
        if (!equivalent(c.hay, c.pat)) return false;
    return true;
}

REGISTER_TEST(regex_engine_fallback_shapes) {
    // Shapes that must fall back to std::regex (seeded by literal prefix or
    // scanned whole): groups are compared against the reference engine.
    struct Case { const char* pat; const char* hay; };
    const Case cases[] = {
        {"price: [0-9]+", "price: 12 and price: 7"},          // seeded, no groups
        {"price: ([0-9]+)", "price: 12 and price: 7"},        // seeded + group
        {"([0-9]+)", "ab 12 cd 345 e"},                       // no anchor at all
        {"(?:item)-([a-z]+)", "item-abc nope item-zz"},       // non-capturing group
        {"a|bb", "a bb a"},                                   // alternation
    };
    for (const auto& c : cases)
        if (!equivalent(c.hay, c.pat)) return false;
    return true;
}

REGISTER_TEST(regex_engine_fuzz) {
    // Deterministic fuzz: LCG-generated haystacks over a delimiter-heavy
    // alphabet, compared against std::regex match-for-match.
    // (No '\r'/'\n' in the alphabet: libstdc++'s '.' also matches '\r' while
    // the ECMAScript dot does not — a known stdlib deviation, not ours.)
    const char* pats[] = {
        "x([^\"]*)\"",        // negclass *
        "<(.*?)>",            // lazy
        "<(.*)>",             // greedy
        "q([^\"]*)\"",        // negclass * (different anchor)
    };
    const char alpha[] = {'a', 'b', '\"', '<', '>', 'x', 'q', '/', ' '};
    unsigned long long s = 0x9E3779B97F4A7C15ull;
    auto next = [&](unsigned long long mod) {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return (s >> 33) % mod;
    };
    for (int iter = 0; iter < 600; ++iter) {
        std::string hay;
        const size_t len = 4 + next(25);
        for (size_t i = 0; i < len; ++i)
            hay += alpha[next(sizeof(alpha))];
        for (const char* p : pats)
            if (!equivalent(hay, p)) return false;
    }
    return true;
}
