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

// ── Regression tests for parsing bugs ───────────────────────────────────────

REGISTER_TEST(selector_id_with_common_letters) {
    // The id charset in the old parser contained the literal letters
    // s,p,a,c,e ("[:space" typo) — ids like "header" were truncated to "h".
    ScraperConfig cfg = make_config(R"CFG({
      "url": "https://example.com",
      "queries": { "q": { "selector": "div#header",
        "extract": [ { "name": "t", "rule": { "text": true } } ] } }
    })CFG");
    auto r = ExtractionEngine().execute(cfg, R"XXX(<div id="header">Hello</div>)XXX");
    if (r.size() != 1) { std::cerr << "id selector matched " << r.size() << "\n"; return false; }
    return r[0]["t"][0].get_string() == "Hello";
}

REGISTER_TEST(selector_multi_class) {
    // `div.a.b` was parsed as tag "divb" + class "a" by the old erasing parser.
    ScraperConfig cfg = make_config(R"CFG({
      "url": "https://example.com",
      "queries": { "q": { "selector": "div.a.b",
        "extract": [ { "name": "t", "rule": { "text": true } } ] } }
    })CFG");
    std::string html = R"XXX(<div class="a b c">Yes</div><div class="a">No</div><div class="b">No</div><span class="a b">No</span>)XXX";
    auto r = ExtractionEngine().execute(cfg, html);
    if (r.size() != 1) { std::cerr << "multi-class matched " << r.size() << "\n"; return false; }
    return r[0]["t"][0].get_string() == "Yes";
}

REGISTER_TEST(selector_single_quoted_attrs) {
    // id='...' / class='...' never matched (the re-search result was discarded).
    ScraperConfig cfg = make_config(R"CFG({
      "url": "https://example.com",
      "queries": { "a": { "selector": "div#main",
        "extract": [ { "name": "t", "rule": { "text": true } } ] } }
    })CFG");
    auto r = ExtractionEngine().execute(cfg, "<div id='main'>Hi</div>");
    if (r.size() != 1) return false;

    cfg = make_config(R"CFG({
      "url": "https://example.com",
      "queries": { "a": { "selector": "div.card",
        "extract": [ { "name": "t", "rule": { "text": true } } ] } }
    })CFG");
    r = ExtractionEngine().execute(cfg, "<div class='card'>Hi</div>");
    return r.size() == 1;
}

REGISTER_TEST(selector_attr_presence_and_value) {
    ScraperConfig cfg = make_config(R"CFG({
      "url": "https://example.com",
      "queries": { "a": { "selector": "a[href]",
        "extract": [ { "name": "t", "rule": { "text": true } } ] } }
    })CFG");
    auto r = ExtractionEngine().execute(cfg, R"XXX(<a href="/x">With</a><a>Without</a>)XXX");
    if (r.size() != 1) return false;

    cfg = make_config(R"CFG({
      "url": "https://example.com",
      "queries": { "a": { "selector": "div[data-id=\"101\"]",
        "extract": [ { "name": "t", "rule": { "text": true } } ] } }
    })CFG");
    r = ExtractionEngine().execute(cfg, R"XXX(<div data-id="100">A</div><div data-id="101">B</div>)XXX");
    return r.size() == 1 && r[0]["t"][0].get_string() == "B";
}

REGISTER_TEST(selector_compound_uses_rightmost) {
    // Compound selectors use the rightmost simple segment ("div.card > a" → "a").
    ScraperConfig cfg = make_config(R"CFG({
      "url": "https://example.com",
      "queries": { "a": { "selector": "div.card > a.link",
        "extract": [ { "name": "t", "rule": { "text": true } } ] } }
    })CFG");
    auto r = ExtractionEngine().execute(cfg, R"XXX(<div class="card"><a class="link">In</a></div><a>Out</a>)XXX");
    if (r.size() != 1) { std::cerr << "compound matched " << r.size() << "\n"; return false; }
    return r[0]["t"][0].get_string() == "In";
}

REGISTER_TEST(void_and_self_closing_tags) {
    // <img> has no close tag; it used to swallow the rest of the page and
    // reverse the record order.
    ScraperConfig cfg = make_config(R"CFG({
      "url": "https://example.com",
      "queries": { "q": { "selector": "img",
        "extract": [ { "name": "s", "rule": { "attribute": "src" } } ] } }
    })CFG");
    auto r = ExtractionEngine().execute(cfg, R"XXX(<img src="a.png"><img src="b.png">)XXX");
    if (r.size() != 2) { std::cerr << "expected 2 imgs, got " << r.size() << "\n"; return false; }
    if (r[0]["s"][0].get_string() != "a.png") return false;
    if (r[1]["s"][0].get_string() != "b.png") return false;

    // Explicit self-close: content after <div/> is not inside the div.
    cfg = make_config(R"CFG({
      "url": "https://example.com",
      "queries": { "q": { "selector": "div.self",
        "extract": [ { "name": "t", "rule": { "text": true } } ] } }
    })CFG");
    r = ExtractionEngine().execute(cfg, R"XXX(<div class="self"/>after)XXX");
    return r.size() == 1 && r[0]["t"][0].get_string().empty();
}

REGISTER_TEST(quoted_gt_in_attribute) {
    // A '>' inside a quoted attribute must not terminate the tag scan.
    ScraperConfig cfg = make_config(R"CFG({
      "url": "https://example.com",
      "queries": { "q": { "selector": "div.note",
        "extract": [ { "name": "t", "rule": { "text": true } } ] } }
    })CFG");
    std::string html = R"XXX(<div class="note" title="a > b">Keep</div><div class="note">Two</div>)XXX";
    auto r = ExtractionEngine().execute(cfg, html);
    if (r.size() != 2) { std::cerr << "expected 2, got " << r.size() << "\n"; return false; }
    if (r[0]["t"][0].get_string() != "Keep") return false;
    if (r[1]["t"][0].get_string() != "Two") return false;
    return true;
}

REGISTER_TEST(script_style_content_skipped) {
    // Script/style content is raw text, not markup: tags inside JS strings
    // must not be indexed, and JS must not leak into extracted text.
    ScraperConfig cfg = make_config(R"CFG({
      "url": "https://example.com",
      "queries": { "q": { "selector": "div",
        "extract": [ { "name": "t", "rule": { "text": true } } ] } }
    })CFG");
    std::string html = R"XXX(<div>Before</div><script>var s = "<div>Fake</div>";</script><div>After</div>)XXX";
    auto r = ExtractionEngine().execute(cfg, html);
    if (r.size() != 2) { std::cerr << "expected 2 real divs, got " << r.size() << "\n"; return false; }
    if (r[0]["t"][0].get_string() != "Before") return false;
    if (r[1]["t"][0].get_string() != "After") return false;
    return true;
}

REGISTER_TEST(unclosed_elements_recover) {
    // Missing close tags: elements must not swallow the rest of the page.
    ScraperConfig cfg = make_config(R"CFG({
      "url": "https://example.com",
      "queries": { "q": { "selector": "span.i",
        "extract": [ { "name": "t", "rule": { "text": true } } ] } }
    })CFG");
    std::string html = R"XXX(<div><span class="i">One</div><div><span class="i">Two</span></div>)XXX";
    auto r = ExtractionEngine().execute(cfg, html);
    if (r.size() != 2) { std::cerr << "expected 2, got " << r.size() << "\n"; return false; }
    if (r[0]["t"][0].get_string() != "One") return false;
    if (r[1]["t"][0].get_string() != "Two") return false;
    return true;
}

REGISTER_TEST(text_decodes_entities) {
    ScraperConfig cfg = make_config(R"CFG({
      "url": "https://example.com",
      "queries": { "q": { "selector": "p",
        "extract": [ { "name": "t", "rule": { "text": true } } ] } }
    })CFG");
    auto r = ExtractionEngine().execute(cfg, R"XXX(<p>Coffee &amp; Tea &#8212; &lt;hot&gt;</p>)XXX");
    if (r.size() != 1) return false;
    std::string t = r[0]["t"][0].get_string();
    if (t != "Coffee & Tea \xE2\x80\x94 <hot>") { std::cerr << "got: " << t << "\n"; return false; }
    // &amp; in attribute values decodes as well
    cfg = make_config(R"CFG({
      "url": "https://example.com",
      "queries": { "q": { "selector": "a",
        "extract": [ { "name": "u", "rule": { "attribute": "href" } } ] } }
    })CFG");
    r = ExtractionEngine().execute(cfg, R"XXX(<a href="/x?a=1&amp;b=2">l</a>)XXX");
    return r.size() == 1 && r[0]["u"][0].get_string() == "/x?a=1&b=2";
}

REGISTER_TEST(text_skips_comments) {
    ScraperConfig cfg = make_config(R"CFG({
      "url": "https://example.com",
      "queries": { "q": { "selector": "div",
        "extract": [ { "name": "t", "rule": { "text": true } } ] } }
    })CFG");
    auto r = ExtractionEngine().execute(cfg, R"XXX(<div>real <!-- a > b comment --> text</div>)XXX");
    if (r.size() != 1) return false;
    return r[0]["t"][0].get_string() == "real text";
}

REGISTER_TEST(literal_null_string_survives) {
    // The old transform pipeline used the string "null" as a null sentinel,
    // turning real text into JSON null.
    ScraperConfig cfg = make_config(R"CFG({
      "url": "https://example.com",
      "queries": { "q": { "selector": "td",
        "extract": [ { "name": "v", "rule": { "text": true, "transform": ["trim"] } } ] } }
    })CFG");
    auto r = ExtractionEngine().execute(cfg, "<td>null</td>");
    if (r.size() != 1) return false;
    auto& v = r[0]["v"];
    return v.is_array() && v.size() == 1 && v[0].is_string() && v[0].get_string() == "null";
}

REGISTER_TEST(regex_group_counting) {
    // Non-capturing groups and parens inside character classes must not be
    // counted as capture groups.
    ScraperConfig cfg = make_config(R"CFG({
      "url": "https://example.com",
      "queries": { "q": { "regex": "[(]price[)]: ([0-9]+)",
        "extract": [ { "name": "p", "rule": { "aggregate": "first", "transform": ["int"] } } ] } }
    })CFG");
    auto r = ExtractionEngine().execute(cfg, "(price): 42");
    if (r.size() != 1) return false;
    if (!r[0]["p"].is_int() || r[0]["p"].get_int() != 42) return false;

    cfg = make_config(R"CFG({
      "url": "https://example.com",
      "queries": { "q": { "regex": "(?:item)-([a-z]+)",
        "extract": [ { "name": "p", "rule": { "aggregate": "first" } } ] } }
    })CFG");
    r = ExtractionEngine().execute(cfg, "item-abc");
    return r.size() == 1 && r[0]["p"].get_string() == "abc";
}

REGISTER_TEST(invalid_regex_is_a_config_error) {
    // An uncompilable pattern must surface an error, not silently match nothing.
    ScraperConfig cfg = make_config(R"CFG({
      "url": "https://example.com",
      "queries": { "q": { "regex": "([unclosed",
        "extract": [ { "name": "p", "rule": {} } ] } }
    })CFG");
    try {
        ExtractionEngine().execute(cfg, "text");
    } catch (const ScrapeError& e) {
        return e.code == ErrorCode::Config;
    }
    return false;
}

REGISTER_TEST(nested_same_tag_and_document_order) {
    ScraperConfig cfg = make_config(R"CFG({
      "url": "https://example.com",
      "queries": { "q": { "selector": "div.i",
        "extract": [ { "name": "t", "rule": { "text": true } } ] } }
    })CFG");
    std::string html = R"XXX(<div class="i">first</div><div class="i">second</div><div class="i">third</div>)XXX";
    auto r = ExtractionEngine().execute(cfg, html);
    if (r.size() != 3) return false;
    if (r[0]["t"][0].get_string() != "first")  return false;
    if (r[1]["t"][0].get_string() != "second") return false;
    if (r[2]["t"][0].get_string() != "third")  return false;
    return true;
}

REGISTER_TEST(json_object_keys_escaped) {
    // Keys with quotes/control chars used to break the emitted JSON.
    json j = json::object();
    j["a\"b\tc"] = 1;
    std::string s = j.dump();
    // round-trip
    auto back = json::parse(s);
    if (!back.contains("a\"b\tc") || back["a\"b\tc"].get_int() != 1) return false;
    return true;
}

REGISTER_TEST(json_rejects_trailing_garbage) {
    bool threw = false;
    try { json::parse("[1, 2] oops"); } catch (const std::exception&) { threw = true; }
    if (!threw) return false;
    // trailing whitespace is fine
    try { json::parse("[1, 2] \n\t "); } catch (...) { return false; }
    return true;
}

REGISTER_TEST(config_pagination_order_preserved) {
    // all_urls used to be sorted lexicographically: page 10 landed before 2.
    ScraperConfig cfg = make_config(R"CFG({
      "url": "https://example.com/i",
      "pagination": { "type": "url_param", "param": "p", "start_page": 1, "max_pages": 11 },
      "queries": { "q": { "selector": "li",
        "extract": [ { "name": "t", "rule": { "text": true } } ] } }
    })CFG");
    if (cfg.all_urls.size() != 11) return false;
    for (int k = 0; k < 11; ++k) {
        std::string want = "https://example.com/i?p=" + std::to_string(k + 1);
        if (cfg.all_urls[size_t(k)] != want) {
            std::cerr << "url " << k << ": " << cfg.all_urls[size_t(k)] << " != " << want << "\n";
            return false;
        }
    }
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

