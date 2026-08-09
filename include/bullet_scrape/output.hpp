#pragma once
#include "bullet_scrape/config.hpp"
#include <string>
#include <vector>
#include <functional>

namespace bullet_scrape {

// ── Output writers ──────────────────────────────────────────────────────────
//
// Each writer receives Records incrementally and serialises them.
// `flush()` is called at end of run to close files / flush buffers.

class OutputWriter {
public:
    using Record = bullet_scrape::Record;

    virtual ~OutputWriter() = default;

    // Called once per record (or batch)
    virtual void write(const Record& record) = 0;
    virtual void write_batch(const std::vector<Record>& records) {
        for (auto& r : records) write(r);
    }
    virtual void flush() {}
    virtual std::string extension() const = 0;
};

// ── JSON array writer ───────────────────────────────────────────────────────
class JsonArrayWriter : public OutputWriter {
    std::vector<Record> buf_;
    std::string         path_;
    bool                to_stdout_;

public:
    JsonArrayWriter(std::string path, bool stdout_ = false)
        : path_(std::move(path)), to_stdout_(stdout_) {}

    void write(const Record& record) override { buf_.push_back(record); }
    void flush() override;
    std::string extension() const override { return "json"; }
};

// ── JSONL writer (one JSON object per line) ─────────────────────────────────
class JsonlWriter : public OutputWriter {
    std::ostream* os_ = nullptr;
    std::ofstream file_;
    std::string   path_;

public:
    JsonlWriter(std::string path);
    void write(const Record& record) override;
    void flush() override;
    std::string extension() const override { return "jsonl"; }
};

// ── CSV writer ──────────────────────────────────────────────────────────────
class CsvWriter : public OutputWriter {
    std::vector<std::string> header_;
    std::ostream*            os_ = nullptr;
    std::ofstream           file_;
    std::string             path_;
    bool                    first_ = true;
    char                    delimiter_ = ',';

    std::string csv_escape(const std::string& val);

public:
    CsvWriter(std::string path, std::vector<std::string> header);
    void write(const Record& record) override;
    void flush() override;
    std::string extension() const override { return "csv"; }
};

// ── Factory ─────────────────────────────────────────────────────────────────
std::unique_ptr<OutputWriter> make_writer(const OutputConfig& cfg);

// ── Write result summary ────────────────────────────────────────────────────
std::string format_summary(const ScrapeResult& result);

} // namespace bullet_scrape
