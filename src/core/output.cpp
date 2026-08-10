#include "bullet_scrape/output.hpp"
#include "bullet_scrape/exceptions.hpp"
#include "bullet_scrape/mini_json.hpp"
#include <fstream>
#include <iostream>
#include <algorithm>
#include <set>
#include <sstream>
#include <iomanip>

namespace bullet_scrape {

using json = mini_json::json;

// ── Value helpers ───────────────────────────────────────────────────────────

static std::string json_to_flat_string(const json& j) {
    if (j.is_null())    return "";
    if (j.is_bool())    return j.get_bool() ? "true" : "false";
    if (j.is_int())     return std::to_string(j.get_int());
    if (j.is_number()) {
        double v = j.get_float();
        std::ostringstream ss;
        ss << std::setprecision(15) << v;
        return ss.str();
    }
    if (j.is_string())  return j.get_string();
    if (j.is_array()) {
        // Flatten single-element arrays (common extract shape) to scalar text
        if (j.size() == 1) return json_to_flat_string(j[0]);
        std::ostringstream ss;
        for (size_t i = 0; i < j.size(); ++i) {
            if (i) ss << "; ";
            ss << json_to_flat_string(j[i]);
        }
        return ss.str();
    }
    return j.dump();
}

// ── CSV escape ──────────────────────────────────────────────────────────────

std::string CsvWriter::csv_escape(const std::string& val) {
    if (val.empty()) return "";
    bool needs_quotes = val.find(delimiter_) != std::string::npos ||
                        val.find('"') != std::string::npos ||
                        val.find('\n') != std::string::npos ||
                        val.find('\r') != std::string::npos;
    std::string out;
    if (needs_quotes) {
        out += '"';
        for (char c : val) {
            if (c == '"') out += "\"\"";
            else          out += c;
        }
        out += '"';
    } else {
        out = val;
    }
    return out;
}

// ── JsonArrayWriter ─────────────────────────────────────────────────────────

void JsonArrayWriter::flush() {
    json j = json::array();
    for (auto& r : buf_)
        j.push_back(r);

    if (to_stdout_) {
        std::cout << j.dump(2) << std::flush;
    } else if (!path_.empty()) {
        std::ofstream f(path_);
        if (!f) throw io_error("cannot write output: " + path_);
        f << j.dump(2);
    }
    buf_.clear();
}

// ── JsonlWriter ─────────────────────────────────────────────────────────────

JsonlWriter::JsonlWriter(std::string path) : path_(std::move(path)) {
    if (path_ == "stdout" || path_.empty()) {
        os_ = &std::cout;
    } else {
        file_.open(path_);
        if (!file_) throw io_error("cannot open output file: " + path_);
        os_ = &file_;
    }
}

void JsonlWriter::write(const Record& record) {
    *os_ << json(record).dump() << "\n";
}

void JsonlWriter::flush() {
    if (file_.is_open()) file_.close();
}

// ── CsvWriter ───────────────────────────────────────────────────────────────

CsvWriter::CsvWriter(std::string path, std::vector<std::string> header)
    : header_(std::move(header)), path_(std::move(path)) {
    if (path_ == "stdout" || path_.empty()) {
        os_ = &std::cout;
    } else {
        file_.open(path_);
        if (!file_) throw io_error("cannot open output file: " + path_);
        os_ = &file_;
    }
    header_locked_ = !header_.empty();
}

void CsvWriter::write_header() {
    for (size_t i = 0; i < header_.size(); ++i) {
        if (i) *os_ << delimiter_;
        *os_ << csv_escape(header_[i]);
    }
    *os_ << "\n";
    first_ = false;
}

void CsvWriter::write_row(const Record& record) {
    for (size_t i = 0; i < header_.size(); ++i) {
        if (i) *os_ << delimiter_;
        auto it = record.find(header_[i]);
        if (it != record.end())
            *os_ << csv_escape(json_to_flat_string(it->second));
        else
            *os_ << "";
    }
    *os_ << "\n";
}

void CsvWriter::write(const Record& record) {
    if (!header_locked_) {
        // Infer header from keys as they appear; buffer first records briefly
        pending_.push_back(record);
        for (const auto& [k, _] : record) {
            if (std::find(header_.begin(), header_.end(), k) == header_.end())
                header_.push_back(k);
        }
        // Flush once we have a stable-ish header (after first record is enough)
        if (pending_.size() >= 1) {
            header_locked_ = true;
            write_header();
            for (auto& r : pending_) write_row(r);
            pending_.clear();
        }
        return;
    }

    if (first_) write_header();
    write_row(record);
}

void CsvWriter::flush() {
    if (!header_locked_ && !pending_.empty()) {
        header_locked_ = true;
        // Collect all keys
        for (auto& r : pending_)
            for (const auto& [k, _] : r)
                if (std::find(header_.begin(), header_.end(), k) == header_.end())
                    header_.push_back(k);
        write_header();
        for (auto& r : pending_) write_row(r);
        pending_.clear();
    }
    if (file_.is_open()) file_.close();
}

// ── TxtWriter ───────────────────────────────────────────────────────────────

std::string TxtWriter::value_to_text(const json& j) {
    return json_to_flat_string(j);
}

TxtWriter::TxtWriter(std::string path, std::vector<std::string> field_order)
    : path_(std::move(path)), field_order_(std::move(field_order)) {
    if (path_ == "stdout" || path_.empty()) {
        os_ = &std::cout;
    } else {
        file_.open(path_);
        if (!file_) throw io_error("cannot open output file: " + path_);
        os_ = &file_;
    }
}

void TxtWriter::write(const Record& record) {
    ++index_;
    if (show_index_)
        *os_ << "--- record " << index_ << " ---\n";

    auto write_kv = [&](const std::string& k, const json& v) {
        *os_ << k << ": " << value_to_text(v) << "\n";
    };

    if (!field_order_.empty()) {
        for (const auto& k : field_order_) {
            auto it = record.find(k);
            if (it != record.end()) write_kv(k, it->second);
        }
        // Any remaining keys not in field_order
        for (const auto& [k, v] : record) {
            if (std::find(field_order_.begin(), field_order_.end(), k) == field_order_.end())
                write_kv(k, v);
        }
    } else {
        // Stable-ish: sort keys for deterministic text output
        std::vector<std::string> keys;
        keys.reserve(record.size());
        for (const auto& [k, _] : record) keys.push_back(k);
        std::sort(keys.begin(), keys.end());
        for (const auto& k : keys)
            write_kv(k, record.at(k));
    }
    *os_ << record_sep_;
}

void TxtWriter::flush() {
    if (file_.is_open()) file_.close();
}

// ── Factory ─────────────────────────────────────────────────────────────────

// Discard writer — used by the C/Python API which captures records in-memory.
class NullWriter : public OutputWriter {
public:
    void write(const Record&) override {}
    void flush() override {}
    std::string extension() const override { return ""; }
};

std::unique_ptr<OutputWriter> make_writer(const OutputConfig& cfg) {
    auto& fmt = cfg.format;
    if (fmt == "none" || fmt == "null" || fmt == "memory")
        return std::make_unique<NullWriter>();
    if (fmt == "stdout" || fmt == "json") {
        if (cfg.path.empty() || cfg.path == "stdout")
            return std::make_unique<JsonArrayWriter>("", true);
        else
            return std::make_unique<JsonArrayWriter>(cfg.path);
    }
    if (fmt == "jsonl") {
        std::string p = cfg.path.empty() ? "stdout" : cfg.path;
        return std::make_unique<JsonlWriter>(p);
    }
    if (fmt == "csv") {
        // csv_fields optional — inferred from record keys when empty
        return std::make_unique<CsvWriter>(
            cfg.path.empty() ? "stdout" : cfg.path,
            cfg.csv_fields);
    }
    if (fmt == "txt" || fmt == "text") {
        return std::make_unique<TxtWriter>(
            cfg.path.empty() ? "stdout" : cfg.path,
            cfg.csv_fields); // reuse field order if provided
    }
    throw config_error("unknown output format: " + fmt);
}

// ── Summary ─────────────────────────────────────────────────────────────────

std::string format_summary(const ScrapeResult& result) {
    std::ostringstream ss;
    ss << "═══ Bullet Scrape — Run Complete ═══\n";
    ss << "Config  : " << result.config_name << "\n";
    ss << "URLs    : " << result.results.size() << "\n";
    ss << "Succeed : " << result.succeeded << "\n";
    ss << "Failed  : " << result.failed    << "\n";
    ss << "Total   : " << result.results.size() << " pages\n";
    ss << "Records: " << result.results.size() << " batches\n";
    ss << "Time    : " << result.total_ms.count() << " ms ("
       << std::fixed << std::setprecision(1)
       << (result.total_ms.count() / 1000.0) << " s)\n";
    ss << "Bytes   : " << result.total_bytes << " B ("
       << std::fixed << std::setprecision(1)
       << (result.total_bytes / 1024.0) << " KB)\n";
    ss << "Throughput: ";
    if (result.total_ms.count() > 0) {
        double pages_per_s = result.results.size() / (result.total_ms.count() / 1000.0);
        ss << std::fixed << std::setprecision(1) << pages_per_s << " pages/s";
    } else {
        ss << "—";
    }
    ss << "\n═══\n";
    return ss.str();
}

} // namespace bullet_scrape
