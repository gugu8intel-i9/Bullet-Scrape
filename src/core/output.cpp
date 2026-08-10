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

// ── CSV escape ──────────────────────────────────────────────────────────────

std::string CsvWriter::csv_escape(const std::string& val) {
    if (val.empty()) return "";
    bool needs_quotes = val.find(',') != std::string::npos ||
                        val.find('"') != std::string::npos ||
                        val.find('\n') != std::string::npos;
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

static std::string json_to_csv_val(const json& j) {
    if (j.is_null())    return "";
    if (j.is_bool())    return j.get_bool() ? "true" : "false";
    if (j.is_number())  return std::to_string(j.get_float());
    return j.get_string();
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
}

void CsvWriter::write(const Record& record) {
    if (first_) {
        for (size_t i = 0; i < header_.size(); ++i) {
            if (i) *os_ << delimiter_;
            *os_ << csv_escape(header_[i]);
        }
        *os_ << "\n";
        first_ = false;
    }

    for (size_t i = 0; i < header_.size(); ++i) {
        if (i) *os_ << delimiter_;
        auto it = record.find(header_[i]);
        if (it != record.end())
            *os_ << csv_escape(json_to_csv_val(it->second));
        else
            *os_ << "";
    }
    *os_ << "\n";
}

void CsvWriter::flush() {
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
        auto header = cfg.csv_fields;
        if (header.empty())
            throw config_error("CSV output requires 'csv_fields' in output config");
        return std::make_unique<CsvWriter>(
            cfg.path.empty() ? "stdout" : cfg.path,
            std::move(header));
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
