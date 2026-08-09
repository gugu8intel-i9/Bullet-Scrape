#pragma once
#include <stdexcept>
#include <string>
#include <sstream>

namespace bullet_scrape {

enum class ErrorCode {
    Config,
    Http,
    Parse,
    Extract,
    Io,
    Timeout,
    Cancel,
    Unknown
};

class ScrapeError : public std::runtime_error {
public:
    ErrorCode code;
    int http_status = 0;
    std::string url;

    ScrapeError(ErrorCode c, std::string msg)
        : std::runtime_error(std::move(msg)), code(c) {}

    ScrapeError(ErrorCode c, int status, std::string url, std::string msg)
        : std::runtime_error(std::move(msg)), code(c),
          http_status(status), url(std::move(url)) {}

    std::string category() const {
        switch (code) {
            case ErrorCode::Config:    return "CONFIG";
            case ErrorCode::Http:      return "HTTP";
            case ErrorCode::Parse:     return "PARSE";
            case ErrorCode::Extract:   return "EXTRACT";
            case ErrorCode::Io:        return "IO";
            case ErrorCode::Timeout:   return "TIMEOUT";
            case ErrorCode::Cancel:    return "CANCEL";
            case ErrorCode::Unknown:   return "UNKNOWN";
        }
        return "UNKNOWN";
    }

    std::string to_string() const {
        std::ostringstream ss;
        ss << "[" << category() << "] ";
        if (!url.empty()) ss << "url=" << url << " ";
        if (http_status) ss << "status=" << http_status << " ";
        ss << what();
        return ss.str();
    }
};

// Convenience constructors
inline ScrapeError config_error(std::string msg) {
    return {ErrorCode::Config, std::move(msg)};
}
inline ScrapeError http_error(int status, std::string url, std::string msg) {
    return {ErrorCode::Http, status, std::move(url), std::move(msg)};
}
inline ScrapeError parse_error(std::string url, std::string msg) {
    return {ErrorCode::Parse, 0, std::move(url), std::move(msg)};
}
inline ScrapeError extract_error(std::string url, std::string msg) {
    return {ErrorCode::Extract, 0, std::move(url), std::move(msg)};
}
inline ScrapeError io_error(std::string msg) {
    return {ErrorCode::Io, std::move(msg)};
}
inline ScrapeError timeout_error(std::string url, std::string msg) {
    return {ErrorCode::Timeout, 0, std::move(url), std::move(msg)};
}

} // namespace bullet_scrape
