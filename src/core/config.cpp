#include "bullet_scrape/config.hpp"
#include "bullet_scrape/cleaner.hpp"
#include "bullet_scrape/exceptions.hpp"
#include "bullet_scrape/mini_json.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <iostream>
#include <unordered_set>

namespace bullet_scrape {

using json = mini_json::json;

// ── Cleaning-sieve spec ─────────────────────────────────────────────────────
// Accepts:  "clean": true|false
//           "clean": "fold"
//           "clean": ["entities", "whitespace", "fold", "numeric", "null_tokens"]
// `fallback` is returned when the key is absent.
static uint32_t parse_clean_spec(const json& j, uint32_t fallback) {
    if (!j.contains("clean")) return fallback;
    const json& c = j["clean"];
    if (c.is_bool())   return c.get_bool() ? CLEAN_DEFAULT : 0u;
    if (c.is_int())    return static_cast<uint32_t>(c.get_int());
    if (c.is_string()) {
        auto bit = clean_stage_by_name(c.get_string());
        if (!bit) throw config_error("unknown cleaner stage: " + c.get_string());
        return *bit;
    }
    if (c.is_array()) {
        uint32_t mask = 0;
        for (size_t i = 0; i < c.size(); ++i) {
            const std::string name = c[i].get_string();
            auto bit = clean_stage_by_name(name);
            if (!bit) throw config_error("unknown cleaner stage: " + name);
            mask |= *bit;
        }
        return mask;
    }
    throw config_error("'clean' must be true, false, a stage name, or an "
                       "array of stage names");
}

// ── ExtractRule ─────────────────────────────────────────────────────────────

void ExtractRule::load(const json& j) {
    if (j.contains("text"))
        text = j["text"].get_bool();
    if (j.contains("attribute"))
        attribute = j["attribute"].get_string();
    if (j.contains("regex"))
        regex = j["regex"].get_string();
    if (j.contains("regex_sub"))
        regex_sub = j["regex_sub"].get_string();
    if (j.contains("transform") && j["transform"].is_array()) {
        for (size_t i = 0; i < j["transform"].size(); ++i)
            transform.push_back(j["transform"][i].get_string());
    }
    if (j.contains("aggregate"))
        aggregate = j["aggregate"].get_string();
    if (j.contains("join_sep"))
        join_sep = j["join_sep"].get_string();
    if (j.contains("optional"))
        optional = j["optional"].get_bool();
    if (j.contains("clean"))
        clean = static_cast<int>(parse_clean_spec(j, 0));
}

// ── CollectionQuery ─────────────────────────────────────────────────────────

void CollectionQuery::load(const json& j) {
    if (j.contains("name"))
        name = j["name"].get_string();
    if (j.contains("selector"))
        selector = j["selector"].get_string();
    if (j.contains("regex"))
        regex = j["regex"].get_string();
    if (j.contains("xpath"))
        xpath = j["xpath"].get_string();
    if (j.contains("multiple"))
        multiple = j["multiple"].get_bool();
    if (j.contains("extract") && j["extract"].is_array()) {
        for (size_t i = 0; i < j["extract"].size(); ++i) {
            const auto& eq = j["extract"][i];
            SubQuery sq;
            if (eq.contains("name"))
                sq.name = eq["name"].get_string();
            if (eq.contains("rule"))
                sq.rule.load(eq["rule"]);
            else
                sq.rule.load(eq);
            extract.push_back(std::move(sq));
        }
    }
}

// ── Pagination ──────────────────────────────────────────────────────────────

Pagination Pagination::from_json(const json& j) {
    Pagination p;
    if (!j.is_object()) return p;

    std::string t = j.contains("type") && j["type"].is_string()
        ? j["type"].get_string() : "none";
    if      (t == "url_param")  p.type = Type::UrlParam;
    else if (t == "next_link")  p.type = Type::NextLink;
    else if (t == "offset")     p.type = Type::Offset;
    else                        p.type = Type::None;

    if (j.contains("param"))
        p.param = j["param"].get_string();
    if (j.contains("start_page"))
        p.start_page = static_cast<int>(j["start_page"].get_int());
    if (j.contains("max_pages"))
        p.max_pages = static_cast<int>(j["max_pages"].get_int());
    if (j.contains("next_selector"))
        p.next_selector = j["next_selector"].get_string();
    if (j.contains("offset_step"))
        p.offset_step = static_cast<int>(j["offset_step"].get_int());
    if (j.contains("max_offset"))
        p.max_offset = static_cast<int>(j["max_offset"].get_int());
    if (j.contains("base_url"))
        p.base_url = j["base_url"].get_string();

    return p;
}

std::vector<std::string> Pagination::expand_urls(const std::string& base) const {
    std::vector<std::string> urls;
    if (type == Type::None) {
        if (!base.empty()) urls.push_back(base);
        return urls;
    }

    auto make_url = [&](int page) -> std::string {
        if (type == Type::UrlParam && param) {
            std::string sep = base.find('?') == std::string::npos ? "?" : "&";
            return base + sep + *param + "=" + std::to_string(page);
        }
        if (type == Type::Offset && offset_step) {
            return base + "?offset=" + std::to_string((page - 1) * *offset_step);
        }
        return base;
    };

    int start = start_page.value_or(1);
    int end   = max_pages ? *max_pages : 1000;

    for (int i = start; i <= end; ++i)
        urls.push_back(make_url(i));

    return urls;
}

// ── ScraperConfig ───────────────────────────────────────────────────────────

static std::string json_opt_str(const json& j, const std::string& key,
                                 const std::string& def) {
    if (j.contains(key) && j[key].is_string())
        return j[key].get_string();
    return def;
}

static int json_opt_int(const json& j, const std::string& key, int def) {
    if (j.contains(key) && j[key].is_number())
        return static_cast<int>(j[key].get_int());
    return def;
}

static bool json_opt_bool(const json& j, const std::string& key, bool def) {
    if (j.contains(key) && j[key].is_bool())
        return j[key].get_bool();
    return def;
}

static void load_headers(const json& j,
                          std::unordered_map<std::string, std::string>& h) {
    if (j.contains("headers") && j["headers"].is_object()) {
        for (auto& [k, v] : j["headers"].get_object())
            h[k] = v.get_string();
    }
}

void ScraperConfig::load(const json& j, const std::string& /*src*/) {
    name        = json_opt_str(j, "name", "unnamed");
    url         = json_opt_str(j, "url", "");
    method      = json_opt_str(j, "method", "GET");
    body        = json_opt_str(j, "body", "");
    user_agent  = json_opt_str(j, "user_agent", "BulletScrape/1.0");
    clean       = parse_clean_spec(j, CLEAN_DEFAULT);

    load_headers(j, headers);

    if (j.contains("limits")) {
        const auto& l = j["limits"];
        limits.max_concurrent      = json_opt_int(l, "max_concurrent", 4);
        limits.max_retries         = json_opt_int(l, "max_retries", 0);
        limits.retry_delay_ms      = std::chrono::milliseconds(
            json_opt_int(l, "retry_delay_ms", 1000));
        limits.timeout_ms          = std::chrono::milliseconds(
            json_opt_int(l, "timeout_ms", 30000));
        if (l.contains("requests_per_second") && l["requests_per_second"].is_number())
            limits.requests_per_second =
                std::optional<double>(l["requests_per_second"].get_float());
        if (l.contains("proxy"))
            limits.proxy = l["proxy"].get_string();
        if (l.contains("follow_redirects"))
            limits.follow_redirects = std::optional<bool>(l["follow_redirects"].get_bool());
        if (l.contains("max_redirects"))
            limits.max_redirects = std::optional<int>(
                static_cast<int>(l["max_redirects"].get_int()));
    }

    if (j.contains("output")) {
        const auto& o = j["output"];
        output.format     = json_opt_str(o, "format", "json");
        output.path       = json_opt_str(o, "path", "");
        output.array      = json_opt_bool(o, "array", true);
        if (o.contains("csv_fields") && o["csv_fields"].is_array()) {
            for (size_t i = 0; i < o["csv_fields"].size(); ++i)
                output.csv_fields.push_back(o["csv_fields"][i].get_string());
        }
    }

    if (j.contains("queries") && j["queries"].is_object()) {
        for (auto& [qname, qj] : j["queries"].get_object()) {
            CollectionQuery q;
            q.name = qname;
            q.load(qj);
            queries[qname] = std::move(q);
        }
    }

    if (j.contains("pagination"))
        pagination = std::optional<Pagination>(Pagination::from_json(j["pagination"]));

    if (j.contains("url_list") && j["url_list"].is_array()) {
        for (size_t i = 0; i < j["url_list"].size(); ++i)
            url_list.push_back(j["url_list"][i].get_string());
    }

    expand_urls();
}

void ScraperConfig::load(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open())
        throw io_error("cannot open config file: " + path);
    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    try {
        load(json::parse(content), path);
    } catch (const std::exception& e) {
        throw config_error("JSON parse error in " + path + ": " + e.what());
    }
}

void ScraperConfig::validate() const {
    if (url.empty() && url_list.empty() && !pagination)
        throw config_error("config must specify 'url', 'url_list', or 'pagination'");

    if (method != "GET" && method != "POST" && method != "PUT" &&
        method != "DELETE" && method != "PATCH")
        throw config_error("unsupported HTTP method: " + method);

    if (queries.empty())
        throw config_error("no queries defined — add at least one query");

    // A query with no match type can never produce records — catch the typo
    // here instead of silently returning nothing.
    for (const auto& [qn, q] : queries) {
        if (q.selector.empty() && !q.regex && !q.xpath)
            throw config_error("query '" + qn +
                "' needs one of: \"selector\", \"regex\", \"xpath\"");
    }

    if (output.format != "json" && output.format != "jsonl" &&
        output.format != "csv" && output.format != "txt" &&
        output.format != "text" &&
        output.format != "stdout" &&
        output.format != "none" && output.format != "null" &&
        output.format != "memory")
        throw config_error("unsupported output format: " + output.format
            + " (want json|jsonl|csv|txt|stdout|memory)");
}

void ScraperConfig::expand_urls() {
    all_urls.clear();

    if (pagination) {
        // When pagination is used, the paginated URLs are the ones to scrape
        if (!url.empty()) {
            auto more = pagination->expand_urls(url);
            all_urls.insert(all_urls.end(), more.begin(), more.end());
        } else if (pagination->base_url) {
            auto more = pagination->expand_urls(*pagination->base_url);
            all_urls.insert(all_urls.end(), more.begin(), more.end());
        }
    } else {
        if (!url.empty())   all_urls.push_back(url);
        for (auto& u : url_list) all_urls.push_back(u);
    }

    // Deduplicate while preserving declaration order (page 2 must not end up
    // behind page 10 — a lexicographic sort would scramble pagination).
    std::unordered_set<std::string> seen;
    seen.reserve(all_urls.size());
    std::vector<std::string> ordered;
    ordered.reserve(all_urls.size());
    for (auto& u : all_urls)
        if (seen.insert(u).second)
            ordered.push_back(std::move(u));
    all_urls = std::move(ordered);
}

} // namespace bullet_scrape
