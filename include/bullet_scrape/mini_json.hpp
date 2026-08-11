#pragma once
// ============================================================================
//  mini_json — self-contained JSON library, C++17, zero external dependencies
//  Supports: null, bool, int, unsigned, double, string, array, object.
//  Parser: recursive descent. Serializer: pretty + compact.
// ============================================================================
#include <string>
#include <vector>
#include <unordered_map>
#include <variant>
#include <memory>
#include <stdexcept>
#include <sstream>
#include <iomanip>
#include <cctype>
#include <cstdlib>
#include <cmath>
#include <fstream>

namespace mini_json {

// ── Null sentinel ────────────────────────────────────────────────────────────
struct json_null {};
inline constexpr json_null json_null_v{};

// ── JSON value class ─────────────────────────────────────────────────────────
class json {
public:
    // Type aliases (named with _t to avoid clashing with static factory methods)
    using object_t = std::unordered_map<std::string, json>;
    using array_t  = std::vector<json>;

private:
    using storage_t = std::variant<
        json_null,
        bool,
        long long,
        unsigned long long,
        double,
        std::string,
        std::shared_ptr<array_t>,
        std::shared_ptr<object_t>
    >;

    storage_t data_;

    // Private constructor from storage (used internally)
    explicit json(std::shared_ptr<array_t>  a) : data_(std::move(a)) {}
    explicit json(std::shared_ptr<object_t> o) : data_(std::move(o)) {}

    friend class json_parser;

public:
    // ── Public constructors ──────────────────────────────────────────────────
    json() : data_(json_null_v) {}
    json(json_null) : data_(json_null_v) {}
    json(std::nullptr_t) : data_(json_null_v) {}
    json(bool b) : data_(b) {}
    json(int v) : data_(static_cast<long long>(v)) {}
    json(long v) : data_(static_cast<long long>(v)) {}
    json(long long v) : data_(v) {}
    json(unsigned long v) : data_(static_cast<unsigned long long>(v)) {}
    json(unsigned long long v) : data_(v) {}
    json(float v) : data_(static_cast<double>(v)) {}
    json(double v) : data_(v) {}
    json(const char* s) : data_(std::string(s)) {}
    json(std::string s) : data_(std::move(s)) {}
    json(std::string_view s) : data_(std::string(s)) {}
    json(array_t a)  : data_(std::make_shared<array_t>(std::move(a))) {}
    json(object_t o) : data_(std::make_shared<object_t>(std::move(o))) {}

    json(const json&) = default;
    json(json&&) = default;
    json& operator=(const json&) = default;
    json& operator=(json&&) = default;

    // ── Static factories ─────────────────────────────────────────────────────
    static json array()  { return json(std::make_shared<array_t>()); }
    static json object() { return json(std::make_shared<object_t>()); }

    // Initializer-list — auto-detects array vs object
    json(std::initializer_list<json> init) {
        if (init.size() == 0) { data_ = json_null_v; return; }
        bool is_obj = true;
        for (auto& el : init) {
            if (!el.is_object() || el.size() != 1) { is_obj = false; break; }
        }
        if (is_obj) {
            object_t obj;
            for (auto& el : init)
                for (auto& [k, v] : el.get_object())
                    obj[k] = v;
            data_ = std::make_shared<object_t>(std::move(obj));
        } else {
            data_ = std::make_shared<array_t>(std::move(init));
        }
    }

    // ── Type queries ─────────────────────────────────────────────────────────
    bool is_null()   const { return std::holds_alternative<json_null>(data_); }
    bool is_bool()   const { return std::holds_alternative<bool>(data_); }
    bool is_int()    const {
        return std::holds_alternative<long long>(data_) ||
               std::holds_alternative<unsigned long long>(data_);
    }
    bool is_number() const { return is_int() || std::holds_alternative<double>(data_); }
    bool is_float()  const { return std::holds_alternative<double>(data_); }
    bool is_string() const { return std::holds_alternative<std::string>(data_); }
    bool is_array()  const { return std::holds_alternative<std::shared_ptr<array_t>>(data_); }
    bool is_object() const { return std::holds_alternative<std::shared_ptr<object_t>>(data_); }

    // ── Accessors ────────────────────────────────────────────────────────────
    bool      get_bool()      const { return std::get<bool>(data_); }
    long long get_int()       const {
        if (auto* i = std::get_if<long long>(&data_)) return *i;
        if (auto* u = std::get_if<unsigned long long>(&data_))
            return static_cast<long long>(*u);
        throw std::runtime_error("json: not an integer");
    }
    unsigned long long get_uint() const {
        if (auto* u = std::get_if<unsigned long long>(&data_)) return *u;
        if (auto* i = std::get_if<long long>(&data_))
            return static_cast<unsigned long long>(*i);
        throw std::runtime_error("json: not an integer");
    }
    double    get_float()     const {
        if (auto* d = std::get_if<double>(&data_)) return *d;
        if (auto* i = std::get_if<long long>(&data_))
            return static_cast<double>(*i);
        if (auto* u = std::get_if<unsigned long long>(&data_))
            return static_cast<double>(*u);
        throw std::runtime_error("json: not a number");
    }
    const std::string& get_string() const {
        return std::get<std::string>(data_);
    }
    const array_t&  get_array()  const {
        return *std::get<std::shared_ptr<array_t>>(data_);
    }
    const object_t& get_object() const {
        return *std::get<std::shared_ptr<object_t>>(data_);
    }

    bool      get_bool_def(bool d)            const { return is_bool() ? get_bool() : d; }
    long long get_int_def(long long d)        const { return is_int() ? get_int() : d; }
    double    get_float_def(double d)         const { return is_number() ? get_float() : d; }
    std::string get_string_def(const std::string& d) const {
        return is_string() ? get_string() : d;
    }

    // ── Dictionary access ────────────────────────────────────────────────────
    json& operator[](const std::string& key) {
        if (!is_object())
            data_ = std::make_shared<object_t>();
        return (*std::get<std::shared_ptr<object_t>>(data_))[key];
    }
    const json& operator[](const std::string& key) const {
        static json nil;
        if (!is_object()) return nil;
        auto& obj = *std::get<std::shared_ptr<object_t>>(data_);
        auto it = obj.find(key);
        return it != obj.end() ? it->second : nil;
    }
    bool contains(const std::string& key) const {
        if (!is_object()) return false;
        return std::get<std::shared_ptr<object_t>>(data_)->count(key) > 0;
    }

    // ── Array access ─────────────────────────────────────────────────────────
    json& operator[](size_t i) {
        auto& arr = *std::get<std::shared_ptr<array_t>>(data_);
        if (i >= arr.size()) arr.resize(i + 1);
        return arr[i];
    }
    const json& operator[](size_t i) const {
        static json nil;
        if (!is_array()) return nil;
        auto& arr = *std::get<std::shared_ptr<array_t>>(data_);
        return i < arr.size() ? arr[i] : nil;
    }
    size_t size() const {
        if (is_array())  return std::get<std::shared_ptr<array_t>>(data_)->size();
        if (is_object()) return std::get<std::shared_ptr<object_t>>(data_)->size();
        return 0;
    }
    void push_back(const json& v) {
        if (!is_array()) data_ = std::make_shared<array_t>();
        std::get<std::shared_ptr<array_t>>(data_)->push_back(v);
    }
    void push_back(json&& v) {
        if (!is_array()) data_ = std::make_shared<array_t>();
        std::get<std::shared_ptr<array_t>>(data_)->push_back(std::move(v));
    }

    // ── Iteration ────────────────────────────────────────────────────────────
    // NOTE: defined out-of-line (below). A function-local `static object_t`
    // inside the class instantiates unordered_map<std::string, json> while
    // the class is still being completed — rejected by libstdc++ (GCC ≤ 11).
    object_t::const_iterator begin() const;
    object_t::const_iterator end() const;

    // ── Serialization ────────────────────────────────────────────────────────
    std::string dump(int indent = -1) const {
        std::ostringstream ss;
        dump_to(ss, indent, 0);
        return ss.str();
    }

private:
    // Write a JSON string literal (keys and values alike) with all required
    // escapes — keys were previously emitted raw and could break the output.
    static void write_escaped(std::ostringstream& ss, const std::string& s) {
        static const char* hex_digits = "0123456789abcdef";
        ss << '"';
        for (char c : s) {
            switch (c) {
                case '"':  ss << "\\\""; break;
                case '\\': ss << "\\\\"; break;
                case '\n': ss << "\\n";  break;
                case '\r': ss << "\\r";  break;
                case '\t': ss << "\\t";  break;
                case '\b': ss << "\\b";  break;
                case '\f': ss << "\\f";  break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        ss << "\\u00"
                           << hex_digits[(static_cast<unsigned char>(c) >> 4) & 0xF]
                           << hex_digits[static_cast<unsigned char>(c) & 0xF];
                    } else {
                        ss << c;
                    }
            }
        }
        ss << '"';
    }

    void dump_to(std::ostringstream& ss, int indent, int level) const {
        if (is_null())   { ss << "null"; return; }
        if (is_bool())   { ss << (get_bool() ? "true" : "false"); return; }
        if (is_int())    { ss << get_int(); return; }
        if (is_float()) {
            double v = get_float();
            if (std::floor(v) == v && std::abs(v) < 1e15)
                ss << std::fixed << std::setprecision(1) << v;
            else
                ss << std::setprecision(15) << v;
            return;
        }
        if (is_string()) {
            write_escaped(ss, get_string());
            return;
        }
        if (is_array()) {
            auto& arr = *std::get<std::shared_ptr<array_t>>(data_);
            if (arr.empty()) { ss << "[]"; return; }
            if (indent <= 0) {
                ss << "[";
                for (size_t i = 0; i < arr.size(); ++i) {
                    if (i) ss << ",";
                    arr[i].dump_to(ss, -1, 0);
                }
                ss << "]";
            } else {
                ss << "[\n";
                std::string pad(level + 1, ' ');
                std::string pad0(level, ' ');
                for (size_t i = 0; i < arr.size(); ++i) {
                    ss << pad;
                    arr[i].dump_to(ss, indent, level + 1);
                    if (i + 1 < arr.size()) ss << ",";
                    ss << "\n";
                }
                ss << pad0 << "]";
            }
            return;
        }
        if (is_object()) {
            auto& obj = *std::get<std::shared_ptr<object_t>>(data_);
            if (obj.empty()) { ss << "{}"; return; }
            if (indent <= 0) {
                ss << "{";
                bool first = true;
                for (auto& [k, v] : obj) {
                    if (!first) ss << ",";
                    first = false;
                    write_escaped(ss, k);
                    ss << ":";
                    v.dump_to(ss, -1, 0);
                }
                ss << "}";
            } else {
                ss << "{\n";
                std::string pad(level + 1, ' ');
                std::string pad0(level, ' ');
                bool first = true;
                for (auto& [k, v] : obj) {
                    if (!first) ss << ",\n";
                    first = false;
                    ss << pad;
                    write_escaped(ss, k);
                    ss << ": ";
                    v.dump_to(ss, indent, level + 1);
                }
                ss << "\n" << pad0 << "}";
            }
            return;
        }
        ss << "null";
    }

    // ── Parsing ──────────────────────────────────────────────────────────────
public:
    static json parse(const std::string& s) {
        json_parser p{s};
        json v = p.parse_value();
        p.skip_ws();
        if (p.pos != s.size())
            throw std::runtime_error("json: trailing data at pos " +
                                     std::to_string(p.pos));
        return v;
    }

    static json parse_file(const std::string& path) {
        std::ifstream f(path);
        if (!f) throw std::runtime_error("json: cannot open file: " + path);
        std::string s((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
        return parse(s);
    }

private:
    struct json_parser {
        const std::string& s;
        size_t pos = 0;

        json_parser(const std::string& str) : s(str) {}

        void skip_ws() {
            while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos])))
                ++pos;
        }
        char peek() {
            skip_ws();
            if (pos >= s.size()) throw std::runtime_error("json: unexpected EOF");
            return s[pos];
        }
        char next() {
            skip_ws();
            if (pos >= s.size()) throw std::runtime_error("json: unexpected EOF");
            return s[pos++];
        }
        void expect(char c) {
            char got = next();
            if (got != c)
                throw std::runtime_error(std::string("json: expected '") + c +
                                         "' but got '" + got + "' at pos " +
                                         std::to_string(pos - 1));
        }

        json parse_value() {
            char c = peek();
            if      (c == 'n') return parse_null();
            else if (c == 't' || c == 'f') return parse_bool();
            else if (c == '"') return parse_string();
            else if (c == '{') return parse_object();
            else if (c == '[') return parse_array();
            else if (c == '-' || std::isdigit(static_cast<unsigned char>(c)))
                return parse_number();
            throw std::runtime_error(std::string("json: unexpected char '") +
                                     c + "' at pos " + std::to_string(pos));
        }

        json parse_null() {
            if (s.compare(pos, 4, "null") == 0) { pos += 4; return json(json_null_v); }
            throw std::runtime_error("json: invalid token at pos " + std::to_string(pos));
        }

        json parse_bool() {
            if (s.compare(pos, 4, "true") == 0)  { pos += 4; return json(true); }
            if (s.compare(pos, 5, "false") == 0) { pos += 5; return json(false); }
            throw std::runtime_error("json: invalid token at pos " + std::to_string(pos));
        }

        json parse_string() {
            expect('"');
            // Fast path: bulk-copy until the closing quote when there are no
            // escape sequences (the overwhelmingly common case).
            const size_t start = pos;
            while (pos < s.size()) {
                char c = s[pos];
                if (c == '"') {
                    std::string out = s.substr(start, pos - start);
                    ++pos;
                    return json(std::move(out));
                }
                if (c == '\\') break;
                ++pos;
            }
            // Slow path: escape handling.
            std::string out = s.substr(start, pos - start);
            while (pos < s.size()) {
                char c = s[pos++];
                if (c == '"') return json(std::move(out));
                if (c == '\\') {
                    if (pos >= s.size())
                        throw std::runtime_error("json: EOF in string escape");
                    char e = s[pos++];
                    switch (e) {
                        case '"':  out += '"';  break;
                        case '\\': out += '\\'; break;
                        case '/':  out += '/';  break;
                        case 'n':  out += '\n'; break;
                        case 'r':  out += '\r'; break;
                        case 't':  out += '\t'; break;
                        case 'b':  out += '\b'; break;
                        case 'f':  out += '\f'; break;
                        case 'u': {
                            if (pos + 4 > s.size())
                                throw std::runtime_error("json: incomplete \\u escape");
                            unsigned int cp = static_cast<unsigned int>(
                                std::stoul(s.substr(pos, 4), nullptr, 16));
                            pos += 4;
                            if (cp < 0x80) {
                                out += static_cast<char>(cp);
                            } else if (cp < 0x800) {
                                out += static_cast<char>(0xC0 | (cp >> 6));
                                out += static_cast<char>(0x80 | (cp & 0x3F));
                            } else {
                                out += static_cast<char>(0xE0 | (cp >> 12));
                                out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                                out += static_cast<char>(0x80 | (cp & 0x3F));
                            }
                            break;
                        }
                        default:
                            // Preserve unrecognized escapes (e.g. \d, \s, \w for regex)
                            out += '\\';
                            out += e;
                            break;
                    }
                } else {
                    out += c;
                }
            }
            throw std::runtime_error("json: unterminated string");
        }

        json parse_number() {
            size_t start = pos;
            if (pos < s.size() && s[pos] == '-') ++pos;
            if (pos < s.size() && s[pos] == '0') ++pos;
            else while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos])))
                ++pos;
            bool is_float = false;
            if (pos < s.size() && s[pos] == '.') {
                is_float = true; ++pos;
                while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos])))
                    ++pos;
            }
            if (pos < s.size() && (s[pos] == 'e' || s[pos] == 'E')) {
                is_float = true; ++pos;
                if (pos < s.size() && (s[pos] == '+' || s[pos] == '-')) ++pos;
                while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos])))
                    ++pos;
            }
            std::string num = s.substr(start, pos - start);
            if (is_float) return json(std::stod(num));
            try {
                return json(static_cast<long long>(std::stoll(num)));
            } catch (...) {
                return json(static_cast<unsigned long long>(std::stoull(num)));
            }
        }

        json parse_array() {
            expect('[');
            json result = json::array();
            if (peek() == ']') { next(); return result; }
            while (true) {
                result.push_back(parse_value());
                char c = next();
                if (c == ']') break;
                if (c != ',')
                    throw std::runtime_error(
                        "json: expected ',' or ']' in array at pos " + std::to_string(pos));
            }
            return result;
        }

        json parse_object() {
            expect('{');
            json result = json::object();
            if (peek() == '}') { next(); return result; }
            while (true) {
                std::string key = parse_string().get_string();
                expect(':');
                result[key] = parse_value();
                char c = next();
                if (c == '}') break;
                if (c != ',')
                    throw std::runtime_error(
                        "json: expected ',' or '}' in object at pos " + std::to_string(pos));
            }
            return result;
        }
    };
};

// ── Out-of-line iteration definitions (see note in-class) ───────────────────
namespace detail {
inline const json::object_t& empty_object_map() {
    static const json::object_t empty;
    return empty;
}
} // namespace detail

inline json::object_t::const_iterator json::begin() const {
    if (!is_object()) return detail::empty_object_map().end();
    return std::get<std::shared_ptr<object_t>>(data_)->begin();
}
inline json::object_t::const_iterator json::end() const {
    if (!is_object()) return detail::empty_object_map().end();
    return std::get<std::shared_ptr<object_t>>(data_)->end();
}

// ── Convenience functions ────────────────────────────────────────────────────
inline json json_parse(const std::string& s)     { return json::parse(s); }
inline json json_parse_file(const std::string& p) { return json::parse_file(p); }

} // namespace mini_json

// Re-export in bullet_scrape namespace for drop-in compatibility
namespace bullet_scrape {
using json = mini_json::json;
inline json json_parse(const std::string& s)     { return mini_json::json::parse(s); }
inline json json_parse_file(const std::string& p) { return mini_json::json::parse_file(p); }
}
