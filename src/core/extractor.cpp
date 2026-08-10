#include "bullet_scrape/extractor.hpp"
#include "bullet_scrape/exceptions.hpp"
#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>
#include <iomanip>
#include <mutex>
#include <shared_mutex>

namespace bullet_scrape {

// ═══════════════════════════════════════════════════════════════════════════
//  Regex helpers (private static members of ExtractionEngine)
// ═══════════════════════════════════════════════════════════════════════════

static std::regex make_regex(const std::string& pattern) {
    try {
        return std::regex(pattern, std::regex::ECMAScript | std::regex::optimize);
    } catch (const std::regex_error&) {
        return std::regex("^$");
    }
}

// Thread-safe regex cache for concurrent workers (shared_mutex = many readers).
// Patterns are stable for a job, so the hot path is almost always a shared lock hit.
static std::unordered_map<std::string, std::regex> regex_cache;
static std::shared_mutex regex_cache_mu;

static const std::regex& get_cached_regex(const std::string& pattern) {
    {
        std::shared_lock lk(regex_cache_mu);
        auto it = regex_cache.find(pattern);
        if (it != regex_cache.end()) return it->second;
    }
    std::unique_lock lk(regex_cache_mu);
    auto it = regex_cache.find(pattern);
    if (it != regex_cache.end()) return it->second;
    auto re = make_regex(pattern);
    return regex_cache.emplace(pattern, std::move(re)).first->second;
}

std::vector<std::string> ExtractionEngine::regex_find_all(
        const std::string& text, const std::string& pattern) {
    std::vector<std::string> out;
    const auto& re = get_cached_regex(pattern);
    auto begin = std::sregex_iterator(text.begin(), text.end(), re);
    auto end   = std::sregex_iterator();
    for (auto it = begin; it != end; ++it)
        out.push_back(it->str());
    return out;
}

std::vector<std::vector<std::string>> ExtractionEngine::regex_find_captures(
        const std::string& text, const std::string& pattern, int groups) {
    std::vector<std::vector<std::string>> out;
    const auto& re = get_cached_regex(pattern);
    auto begin = std::sregex_iterator(text.begin(), text.end(), re);
    auto end   = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        std::vector<std::string> caps;
        for (int i = 0; i <= groups && i < (int)it->size(); ++i)
            caps.push_back(it->str(i));
        out.push_back(std::move(caps));
    }
    return out;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Attribute / text extraction
// ═══════════════════════════════════════════════════════════════════════════

static std::string trim_str(const std::string& s) {
    size_t start = 0, end = s.size();
    while (start < end && std::isspace((unsigned char)s[start])) ++start;
    while (end > start && std::isspace((unsigned char)s[end - 1])) --end;
    return s.substr(start, end - start);
}

static std::string strip_tags(const std::string& html) {
    std::string out;
    out.reserve(html.size());
    bool in_tag = false;
    for (char c : html) {
        if (c == '<') { in_tag = true; continue; }
        if (c == '>') { in_tag = false; continue; }
        if (!in_tag) out += c;
    }
    std::string collapsed;
    bool last_space = false;
    for (char c : out) {
        if (std::isspace((unsigned char)c)) {
            if (!last_space) collapsed += ' ';
            last_space = true;
        } else {
            collapsed += c;
            last_space = false;
        }
    }
    return trim_str(collapsed);
}

std::optional<std::string> ExtractionEngine::extract_attribute(
        const std::string& element_html, const std::string& attr) {
    std::string lower_html = element_html;
    for (auto& c : lower_html) c = (char)std::tolower((unsigned char)c);

    auto try_quoted = [&](char quote) -> std::optional<std::string> {
        std::string needle = " " + attr + "=" + quote;
        auto pos = lower_html.find(needle);
        if (pos != std::string::npos) {
            pos += needle.size();
            auto end = lower_html.find(quote, pos);
            if (end != std::string::npos)
                return element_html.substr(pos, end - pos);
        }
        return std::nullopt;
    };

    auto r = try_quoted('"');
    if (r) return r;
    r = try_quoted('\'');
    if (r) return r;

    // Unquoted: attr=value (ends at whitespace, >, or /)
    std::string search = " " + attr + "=";
    auto pos = lower_html.find(search);
    if (pos != std::string::npos) {
        pos += search.size();
        auto end = element_html.find_first_of(" \t\n\r>/", pos);
        if (end == std::string::npos) end = element_html.size();
        return element_html.substr(pos, end - pos);
    }

    return std::nullopt;
}

std::string ExtractionEngine::extract_text(const std::string& element_html) {
    return strip_tags(element_html);
}

// ═══════════════════════════════════════════════════════════════════════════
//  Tag finding (CSS-selector-lite, no full DOM)
// ═══════════════════════════════════════════════════════════════════════════

// Parsed selector components
struct parsed_sel {
    std::string tag;
    std::string id;
    std::vector<std::string> classes;
    std::unordered_map<std::string, std::string> attrs;
};

static parsed_sel parse_selector(const std::string& sel) {
    parsed_sel ps;
    std::string s = sel;

    auto hash_pos = s.find('#');
    if (hash_pos != std::string::npos) {
        auto end = s.find_first_of(".#[:space>+~", hash_pos + 1);
        ps.id = s.substr(hash_pos + 1, end - hash_pos - 1);
        s.erase(hash_pos, ps.id.size() + 1);
    }

    size_t pos = 0;
    while ((pos = s.find('.', pos)) != std::string::npos) {
        auto end = s.find_first_of(".#[]>+~ \t\n\r", pos + 1);
        if (end == std::string::npos) {
            ps.classes.push_back(s.substr(pos + 1));
            s.erase(pos);  // erase from '.' to end
            break;
        } else {
            ps.classes.push_back(s.substr(pos + 1, end - pos - 1));
            s.erase(pos, end - pos + 1);
        }
        // pos stays at same index; next iteration finds next '.' from here
    }

    auto bracket = s.find('[');
    std::string tag_part = (bracket != std::string::npos)
        ? s.substr(0, bracket) : s;
    tag_part.erase(0, tag_part.find_first_not_of(" \t\n\r>+~"));
    tag_part.erase(tag_part.find_last_not_of(" \t\n\r>+~") + 1);
    if (!tag_part.empty() && tag_part != ">" && tag_part != "+" && tag_part != "~")
        ps.tag = tag_part;

    pos = 0;
    while ((pos = s.find('[', pos)) != std::string::npos) {
        auto close = s.find(']', pos);
        if (close == std::string::npos) break;
        std::string ae = s.substr(pos + 1, close - pos - 1);
        auto eq = ae.find('=');
        if (eq != std::string::npos) {
            std::string name  = ae.substr(0, eq);
            std::string value = ae.substr(eq + 1);
            if (value.size() >= 2 &&
                ((value[0] == '"' && value.back() == '"') ||
                 (value[0] == '\'' && value.back() == '\'')))
                value = value.substr(1, value.size() - 2);
            ps.attrs[name] = value;
        } else {
            ps.attrs[ae] = "";
        }
        pos = close + 1;
    }
    return ps;
}

static bool tag_matches(const std::string& open_tag, const parsed_sel& ps) {
    if (!ps.tag.empty()) {
        // Extract tag name from open_tag (e.g. "<div class=..." → "div")
        auto gt = open_tag.find('>');
        std::string tn = open_tag.substr(1, gt - 1);
        tn.erase(0, tn.find_first_not_of(" \t\n\r"));
        auto sp = tn.find(' ');
        if (sp != std::string::npos) tn = tn.substr(0, sp);
        if (tn != ps.tag) return false;
    }
    if (!ps.id.empty()) {
        std::string search = "id=\"";
        auto ip = open_tag.find(search);
        if (ip == std::string::npos) search = "id='";
        if (ip != std::string::npos) {
            ip += 4;
            auto ie = open_tag.find('"', ip);
            if (ie != std::string::npos) {
                std::string found_id = open_tag.substr(ip, ie - ip);
                if (found_id != ps.id) return false;
            }
        } else {
            return false;
        }
    }
    for (auto& cls : ps.classes) {
        auto cp = open_tag.find("class=\"");
        if (cp == std::string::npos) return false;
        cp += 7;  // "class=\" is 7 chars: c-l-a-s-s-=-"
        auto ce = open_tag.find('"', cp);
        std::string found;
        if (ce != std::string::npos)
            found = open_tag.substr(cp, ce - cp);
        std::istringstream iss(found);
        std::string token;
        bool found_cls = false;
        while (iss >> token)
            if (token == cls) { found_cls = true; break; }
        if (!found_cls) return false;
    }
    return true;
}

static std::vector<std::pair<size_t, size_t>> find_tag_ranges(
        const std::string& html, const std::string& tag_name) {
    std::vector<std::pair<size_t, size_t>> ranges;
    std::string lh = html;
    for (auto& c : lh) c = (char)std::tolower((unsigned char)c);

    std::string open_pat  = "<"  + tag_name;
    std::string close_pat = "</" + tag_name;
    std::vector<size_t> opens;
    size_t search_from = 0;

    while (true) {
        auto op = lh.find(open_pat, search_from);
        auto cp = lh.find(close_pat, search_from);
        if (op == std::string::npos && cp == std::string::npos) break;

        if (cp != std::string::npos &&
            (op == std::string::npos || cp < op)) {
            if (!opens.empty()) {
                ranges.emplace_back(opens.back(), cp);
                opens.pop_back();
            }
            search_from = cp + close_pat.size();
        } else {
            auto after = html.find('>', op);
            if (after != std::string::npos && after > (int)op) {
                std::string between = html.substr(op, after - op + 1);
                if (between.find('/') == std::string::npos || between.back() != '/')
                    opens.push_back(op);
            }
            search_from = op + 1;
        }
    }
    while (!opens.empty()) {
        ranges.emplace_back(opens.back(), html.size());
        opens.pop_back();
    }
    return ranges;
}

std::vector<ExtractionEngine::TagMatch> ExtractionEngine::find_elements(
        const std::string& html, const std::string& selector) {

    auto ps = parse_selector(selector);
    std::vector<TagMatch> result;

    std::vector<std::string> tags_to_search;
    if (ps.tag.empty())
        tags_to_search = {"div","span","a","p","li","tr","td","th",
                          "section","article","main","table","ul","ol"};
    else
        tags_to_search = {ps.tag};

    for (auto& tag : tags_to_search) {
        auto ranges = find_tag_ranges(html, tag);
        for (auto [open_pos, close_pos] : ranges) {
            auto gt_pos = html.find('>', open_pos);
            if (gt_pos == std::string::npos) continue;
            std::string open_tag = html.substr(open_pos, gt_pos - open_pos + 1);

            if (!tag_matches(open_tag, ps)) continue;

            bool attr_ok = true;
            for (auto& [aname, avalue] : ps.attrs) {
                auto it = extract_attribute(open_tag, aname);
                if (!it) { attr_ok = false; break; }
                if (!avalue.empty() && *it != avalue) { attr_ok = false; break; }
            }
            if (!attr_ok) continue;

            std::string inner_html = html.substr(gt_pos + 1, close_pos - gt_pos - 1);
            auto close_end = html.find('>', close_pos);
            std::string close_tag = close_end != std::string::npos
                ? html.substr(close_pos, close_end - close_pos + 1)
                : html.substr(close_pos);

            TagMatch tm;
            tm.open_tag   = open_tag;
            tm.inner_html = inner_html;
            tm.close_tag  = close_tag;
            tm.full       = open_tag + inner_html + close_tag;
            tm.tag_name   = tag;
            result.push_back(std::move(tm));
        }
    }
    return result;
}

// ═══════════════════════════════════════════════════════════════════════════
//  URL join helper
// ═══════════════════════════════════════════════════════════════════════════

static std::string urljoin(const std::string& base, const std::string& rel) {
    if (rel.empty()) return base;
    if (rel.find("://") != std::string::npos) return rel;
    auto se = base.find("://");
    if (se == std::string::npos) return rel;
    auto ps = base.find('/', se + 3);
    std::string scheme  = base.substr(0, se);
    std::string host    = base.substr(se + 3, (ps != std::string::npos ? ps - se - 3 : std::string::npos));
    std::string base_path = ps != std::string::npos ? base.substr(ps) : "/";

    if (rel[0] == '/') return scheme + "://" + host + rel;

    auto ls = base_path.rfind('/');
    std::string parent = (ls != std::string::npos) ? base_path.substr(0, ls + 1) : "/";
    std::string encoded;
    for (char c : rel) encoded += (c == ' ') ? "%20" : std::string(1, c);
    return scheme + "://" + host + parent + encoded;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Transform
// ═══════════════════════════════════════════════════════════════════════════

json ExtractionEngine::apply_transforms(
        const std::vector<std::string>& values,
        const ExtractRule& rule,
        const std::string& base_url) {
    json out = json::array();

    // Determine the target type from the last transform
    std::string last_transform;
    if (!rule.transform.empty())
        last_transform = rule.transform.back();

    for (auto v : values) {
        for (auto& t : rule.transform) {
            if      (t == "trim")        v = trim_str(v);
            else if (t == "lowercase")   for (auto& c : v) c = (char)std::tolower((unsigned char)c);
            else if (t == "uppercase")   for (auto& c : v) c = (char)std::toupper((unsigned char)c);
            else if (t == "urljoin")     v = urljoin(base_url, v);
            else if (t == "int") {
                try { v = std::to_string(std::stoi(v)); } catch (...) { v = "null"; }
            }
            else if (t == "float") {
                try { v = std::to_string(std::stod(v)); } catch (...) { v = "null"; }
            }
            else if (t == "regex_sub" && rule.regex_sub) {
                auto re = make_regex(rule.regex.value_or(""));
                v = std::regex_replace(v, re, *rule.regex_sub);
            }
        }
        if (v == "null") {
            out.push_back(nullptr);
        } else if (last_transform == "int") {
            try { out.push_back(std::stoll(v)); }
            catch (...) { out.push_back(nullptr); }
        } else if (last_transform == "float") {
            try { out.push_back(std::stod(v)); }
            catch (...) { out.push_back(nullptr); }
        } else {
            out.push_back(v);
        }
    }
    return out;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Aggregate (operates on the json array output of apply_transforms)
// ═══════════════════════════════════════════════════════════════════════════

json ExtractionEngine::apply_aggregate(const json& transformed,
                                        const ExtractRule& rule,
                                        size_t raw_count) {
    if (!rule.aggregate) return transformed;

    auto& agg = *rule.aggregate;

    if (agg == "count")   return static_cast<int>(raw_count);
    if (agg == "exists")  return raw_count > 0;

    if (agg == "first") {
        if (transformed.size() == 0) return json(nullptr);
        return transformed[0];  // returns typed json value
    }
    if (agg == "last") {
        if (transformed.size() == 0) return json(nullptr);
        return transformed[transformed.size() - 1];
    }

    if (agg == "unique") {
        std::set<std::string> seen;
        json result = json::array();
        for (size_t i = 0; i < transformed.size(); ++i) {
            std::string val = transformed[i].is_string()
                ? transformed[i].get_string()
                : transformed[i].dump();
            if (seen.insert(val).second)
                result.push_back(transformed[i]);
        }
        return result;
    }

    if (agg == "join") {
        std::string sep = rule.join_sep.value_or("");
        std::string joined;
        for (size_t i = 0; i < transformed.size(); ++i) {
            if (i) joined += sep;
            joined += transformed[i].is_string()
                ? transformed[i].get_string()
                : transformed[i].dump();
        }
        return joined;
    }

    return transformed;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Leaf extraction
// ═══════════════════════════════════════════════════════════════════════════

json ExtractionEngine::extract_leaf(const SubQuery& sub,
                                    const std::string& element_html,
                                    const std::string& base_url) {
    const auto& rule = sub.rule;
    std::vector<std::string> raw;

    if (rule.text) {
        raw = {extract_text(element_html)};
    } else if (rule.attribute) {
        auto val = extract_attribute(element_html, *rule.attribute);
        if (val) raw = {*val};
    } else if (rule.regex) {
        int groups = 0;
        bool escaped = false;
        for (char ch : *rule.regex) {
            if (escaped) { escaped = false; continue; }
            if (ch == '\\') { escaped = true; continue; }
            if (ch == '(') ++groups;
        }
        auto caps = regex_find_captures(element_html, *rule.regex, groups);
        if (groups >= 1) {
            for (auto& c : caps)
                if (c.size() > 1) raw.push_back(c[1]);
        } else {
            for (auto& c : caps) raw.push_back(c[0]);
        }
    }

    if (raw.empty()) {
        if (rule.optional) return json(nullptr);
        if (rule.aggregate) return apply_aggregate(json::array(), rule, 0);
        return json::array();
    }

    json transformed = apply_transforms(raw, rule, base_url);
    return apply_aggregate(transformed, rule, raw.size());
}

// ═══════════════════════════════════════════════════════════════════════════
//  Full query execution
// ═══════════════════════════════════════════════════════════════════════════

std::vector<Record> ExtractionEngine::execute(
        const ScraperConfig& cfg,
        const std::string& html,
        const std::string& base_url) {

    std::vector<Record> all_records;

    for (auto& [qname, query] : cfg.queries) {
        if (query.xpath) continue;

        if (query.regex) {
            // Count capturing groups: '(' not preceded by '\'
            int groups = 0;
            bool escaped = false;
            for (char ch : *query.regex) {
                if (escaped) {
                    escaped = false;
                    continue;
                }
                if (ch == '\\') {
                    escaped = true;
                    continue;
                }
                if (ch == '(') ++groups;
            }
            auto caps = regex_find_captures(html, *query.regex, groups);
            for (auto& c : caps) {
                std::string element_text;
                if (groups >= 1 && c.size() > 1)
                    element_text = c[1];
                else
                    element_text = c[0];

                Record rec;
                for (auto& sub : query.extract) {
                    if (sub.rule.regex) {
                        auto matches = regex_find_all(element_text, *sub.rule.regex);
                        if (matches.empty()) {
                            rec[sub.name] = sub.rule.optional ? json(nullptr) : json::array();
                        } else {
                            json t = apply_transforms(matches, sub.rule, base_url);
                            rec[sub.name] = apply_aggregate(t, sub.rule, matches.size());
                        }
                    } else {
                        json t = apply_transforms({element_text}, sub.rule, base_url);
                        rec[sub.name] = apply_aggregate(t, sub.rule, 1);
                    }
                }
                all_records.push_back(std::move(rec));
            }
            continue;
        }

        // ── Selector-based query ─────────────────────────────────────────────
        auto elements = find_elements(html, query.selector);

        // Check if any extract has an aggregate
        bool has_aggregate = false;
        for (auto& sub : query.extract)
            if (sub.rule.aggregate) { has_aggregate = true; break; }

        if (has_aggregate && query.multiple) {
            // Cross-element aggregate: collect all raw matches, then aggregate
            std::vector<Record> per_element;
            per_element.reserve(elements.size());

            for (auto& el : elements) {
                Record rec;
                for (auto& sub : query.extract)
                    rec[sub.name] = extract_leaf(sub, el.full, base_url);
                per_element.push_back(std::move(rec));
            }

            // Merge into one record
            Record merged;
            for (auto& sub : query.extract) {
                auto& rule = sub.rule;

                // Pure count with no extraction fields: count elements directly
                if (rule.aggregate.value_or("") == "count" &&
                    !rule.text && !rule.attribute &&
                    !rule.regex && rule.transform.empty())
                {
                    merged[sub.name] = static_cast<int>(elements.size());
                    continue;
                }

                // Collect all values for this field across all elements
                json all_values = json::array();
                for (auto& rec : per_element) {
                    auto it = rec.find(sub.name);
                    if (it != rec.end()) {
                        auto& val = it->second;
                        if (val.is_array()) {
                            for (size_t i = 0; i < val.size(); ++i)
                                all_values.push_back(val[i]);
                        } else {
                            all_values.push_back(val);
                        }
                    }
                }
                merged[sub.name] = apply_aggregate(all_values, rule, all_values.size());
            }
            all_records.push_back(std::move(merged));
        } else {
            if (!query.multiple)
                elements.resize(std::min(elements.size(), (size_t)1));

            for (auto& el : elements) {
                Record rec;
                for (auto& sub : query.extract)
                    rec[sub.name] = extract_leaf(sub, el.full, base_url);
                all_records.push_back(std::move(rec));
            }
        }
    }

    return all_records;
}

json ExtractionEngine::execute_scalar(const CollectionQuery& query,
                                      const std::string& html) {
    if (query.extract.empty()) return json(nullptr);
    json rec = json::object();
    for (auto& sub : query.extract)
        rec[sub.name] = extract_leaf(sub, html, "");
    return rec;
}

} // namespace bullet_scrape
