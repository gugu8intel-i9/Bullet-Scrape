#include "bullet_scrape/config.hpp"
#include "bullet_scrape/extractor.hpp"
#include "bullet_scrape/mini_json.hpp"
#include "bullet_scrape/exceptions.hpp"
#include "test_harness.hpp"
#include <string>
#include <iostream>

using namespace bullet_scrape;
using json = mini_json::json;

static ScraperConfig make_config(const std::string& s) {
    ScraperConfig cfg;
    cfg.load(json::parse(s), "<test>");
    return cfg;
}

static const char* CFG_BASIC = R"CFG({
  "url": "https://example.com",
  "queries": {
    "products": {
      "selector": "div.product",
      "extract": [
        { "name": "url",   "rule": { "attribute": "href" } },
        { "name": "title", "rule": { "text": true, "transform": ["trim"] } },
        { "name": "price", "rule": { "regex": "[$]?([0-9]+\\.[0-9]{2})", "transform": ["float"] } }
      ]
    }
  }
})CFG";
static const char* CFG_MEM = R"CFG({
  "url": "https://example.com",
  "queries": {
    "memory": {
      "regex": "Allocated memory: (\\d+) MB",
      "extract": [
        { "name": "mb", "rule": { "aggregate": "first", "transform": ["int"] } }
      ]
    }
  }
})CFG";
static const char* CFG_COUNT = R"CFG({
  "url": "https://example.com",
  "queries": {
    "count": {
      "selector": "div",
      "extract": [ { "name": "n", "rule": { "aggregate": "count" } } ]
    }
  }
})CFG";
static const char* CFG_UNIQUE = R"CFG({
  "url": "https://example.com",
  "queries": {
    "tags": {
      "selector": "span.tag",
      "extract": [
        { "name": "tags", "rule": { "text": true, "aggregate": "unique", "transform": ["trim","lowercase"] } }
      ]
    }
  }
})CFG";
static const char* CFG_URLJOIN = R"CFG({
  "url": "https://shop.example.com/cat/1",
  "queries": {
    "link": {
      "selector": "a",
      "extract": [
        { "name": "url", "rule": { "attribute": "href", "transform": ["urljoin"] } }
      ]
    }
  }
})CFG";
static const char* CFG_LOWER = R"CFG({
  "url": "https://example.com",
  "queries": {
    "x": {
      "selector": "span.x",
      "extract": [ { "name": "v", "rule": { "text": true, "transform": ["lowercase"] } } ]
    }
  }
})CFG";
static const char* CFG_UPPER = R"CFG({
  "url": "https://example.com",
  "queries": {
    "x": {
      "selector": "span.x",
      "extract": [ { "name": "v", "rule": { "text": true, "transform": ["uppercase"] } } ]
    }
  }
})CFG";
static const char* CFG_OPTIONAL = R"CFG({
  "url": "https://example.com",
  "queries": {
    "x": {
      "selector": "div",
      "extract": [
        { "name": "text", "rule": { "text": true, "transform": ["trim"] } },
        { "name": "href", "rule": { "attribute": "href", "optional": true } }
      ]
    }
  }
})CFG";
static const char* CFG_EMPTY = R"CFG({
  "url": "https://example.com",
  "queries": {
    "items": {
      "selector": "li.item",
      "extract": [ { "name": "text", "rule": { "text": true } } ]
    }
  }
})CFG";
static const char* CFG_FEATURED = R"CFG({
  "url": "https://example.com",
  "queries": {
    "featured": {
      "selector": "div.featured",
      "extract": [ { "name": "name", "rule": { "text": true, "transform": ["trim"] } } ]
    }
  }
})CFG";


REGISTER_TEST(extractor_basic_selector) {
    std::string html = R"XXX(<div class="product" data-id="101"><a href="/p/101">Widget A</a><span class="price">$19.99</span></div><div class="product" data-id="102"><a href="/p/102">Widget B</a><span class="price">$29.99</span></div><div class="product" data-id="103"><a href="/p/103">Widget C</a><span class="price">$39.99</span></div>)XXX";
    auto results = ExtractionEngine().execute(make_config(CFG_BASIC), html, "https://example.com");
    if (results.size() != 3) { std::cerr << "Expected 3, got " << results.size() << "\n"; return false; }
    auto& r0 = results[0];
    auto& u0 = r0["url"];   if (!u0.is_array()||u0.size()!=1) return false;
    if (u0[0].get_string() != "/p/101") return false;
    auto& t0 = r0["title"]; if (!t0.is_array()||t0.size()!=1) return false;
    if (t0[0].get_string() != "Widget A$19.99") return false;
    auto& p0 = r0["price"]; if (!p0.is_array()||p0.size()!=1) return false;
    if (!p0[0].is_float() || p0[0].get_float() != 19.99) return false;
    auto& r2 = results[2];
    auto& u2 = r2["url"];   if (!u2.is_array()||u2.size()!=1) return false;
    if (u2[0].get_string() != "/p/103") return false;
    auto& t2 = r2["title"]; if (!t2.is_array()||t2.size()!=1) return false;
    if (t2[0].get_string() != "Widget C$39.99") return false;
    return true;
}

REGISTER_TEST(extractor_regex) {
    std::string html = "<html><body><p>Allocated memory: 512 MB</p></body></html>";
    auto results = ExtractionEngine().execute(make_config(CFG_MEM), html);
    if (results.size() != 1) return false;
    auto mb = results[0]["mb"];
    if (!mb.is_int() || mb.get_int() != 512) { std::cerr << "Expected 512, got " << mb.dump() << "\n"; return false; }
    return true;
}

REGISTER_TEST(extractor_aggregate_count) {
    std::string html = "<html><body><div>A</div><div>B</div><div>C</div></body></html>";
    auto results = ExtractionEngine().execute(make_config(CFG_COUNT), html);
    if (results.size() != 1) return false;
    auto n = results[0]["n"];
    if (!n.is_int() || n.get_int() != 3) { std::cerr << "Expected 3, got " << n.dump() << " (type=" << n.is_int() << ")\n"; return false; }
    return true;
}

REGISTER_TEST(extractor_aggregate_unique) {
    std::string html = R"XXX(<html><body><span class="tag">python</span><span class="tag">rust</span><span class="tag">python</span><span class="tag">go</span></body></html>)XXX";
    auto results = ExtractionEngine().execute(make_config(CFG_UNIQUE), html);
    if (results.size() != 1) return false;
    auto& tags = results[0]["tags"];
    if (!tags.is_array() || tags.size() != 3) { std::cerr << "Expected 3 unique tags, got " << tags.dump() << "\n"; return false; }
    return true;
}

REGISTER_TEST(extractor_transform_urljoin) {
    std::string html = R"XXX(<a href="/products/widget">Widget</a>)XXX";
    auto results = ExtractionEngine().execute(make_config(CFG_URLJOIN), html, "https://shop.example.com/cat/1");
    if (results.size() != 1) return false;
    auto& u = results[0]["url"];
    if (!u.is_array()||u.size()!=1) return false;
    std::string url = u[0].get_string();
    if (url != "https://shop.example.com/products/widget") { std::cerr << "Expected full URL, got: " << url << "\n"; return false; }
    return true;
}

REGISTER_TEST(extractor_transform_case) {
    std::string html = R"XXX(<span class="x">HeLLo WoRLD</span>)XXX";
    auto r = ExtractionEngine().execute(make_config(CFG_LOWER), html);
    if (r.size()!=1) return false;
    auto& v = r[0]["v"];
    if (!v.is_array()||v.size()!=1||v[0].get_string()!="hello world") return false;
    r = ExtractionEngine().execute(make_config(CFG_UPPER), html);
    auto& v2 = r[0]["v"];
    if (!v2.is_array()||v2.size()!=1||v2[0].get_string()!="HELLO WORLD") return false;
    return true;
}

REGISTER_TEST(extractor_optional) {
    std::string html = R"XXX(<div><span class="always">present</span></div>)XXX";
    auto results = ExtractionEngine().execute(make_config(CFG_OPTIONAL), html);
    if (results.size()!=1) return false;
    // "text" should be the text content of the div
    auto& text = results[0]["text"];
    if (!text.is_array()||text.size()!=1||text[0].get_string()!="present") return false;
    // "href" should be null (div has no href, but optional=true)
    if (!results[0]["href"].is_null()) return false;
    return true;
}

REGISTER_TEST(extractor_empty_page) {
    std::string html = "<html><body><p>No items here.</p></body></html>";
    auto results = ExtractionEngine().execute(make_config(CFG_EMPTY), html);
    if (!results.empty()) { std::cerr << "Expected 0 records\n"; return false; }
    return true;
}

REGISTER_TEST(extractor_class_selector) {
    std::string html = R"XXX(<div class="card featured">Featured</div><div class="card">Regular</div><div class="card featured">Another</div>)XXX";
    auto results = ExtractionEngine().execute(make_config(CFG_FEATURED), html);
    if (results.size() != 2) { std::cerr << "Expected 2, got " << results.size() << "\n"; return false; }
    auto& n0 = results[0]["name"];
    auto& n1 = results[1]["name"];
    if (!n0.is_array()||n0.size()!=1||n0[0].get_string()!="Featured") return false;
    if (!n1.is_array()||n1.size()!=1||n1[0].get_string()!="Another") return false;
    return true;
}

REGISTER_TEST(config_loading) {
    ScraperConfig cfg;
    cfg.load(json::parse(R"XXX({"name":"test","url":"https://example.com/items","pagination":{"type":"url_param","param":"page","start_page":1,"max_pages":5},"queries":{"items":{"selector":"li","extract":[{"name":"text","rule":{"text":true}}]}},"output":{"format":"json"},"limits":{"max_concurrent":4}})XXX"),"<test>");
    if (cfg.all_urls.size()!=5) { std::cerr << "Expected 5 URLs, got " << cfg.all_urls.size() << "\n"; return false; }
    if (cfg.all_urls[0] != "https://example.com/items?page=1") return false;
    if (cfg.all_urls[4] != "https://example.com/items?page=5") return false;
    return true;
}

REGISTER_TEST(config_validation_errors) {
    bool caught = false;
    try {
        ScraperConfig cfg;
        cfg.load(json::parse(R"XXX({"name":"test","method":"INVALID"})XXX"),"<test>");
        cfg.validate();
    } catch (const ScrapeError& e) {
        if (e.code == ErrorCode::Config) caught = true;
    }
    return caught;
}

