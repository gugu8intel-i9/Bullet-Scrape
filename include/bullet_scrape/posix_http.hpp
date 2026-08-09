#pragma once
// ============================================================================
//  posix_http — Lightweight HTTP client using POSIX sockets
//  No external dependencies. Supports HTTP (not HTTPS) GET/POST with
//  redirects, custom headers, timeouts, and chunked transfer encoding.
//
//  For HTTPS support, define BULLET_HAVE_CURL and link with libcurl.
// ============================================================================
#include "bullet_scrape/exceptions.hpp"
#include "bullet_scrape/config.hpp"
#include <string>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <cstring>
#include <sstream>
#include <algorithm>

namespace bullet_scrape {

// ── Response ────────────────────────────────────────────────────────────────
struct HTTPResponse {
    std::string body;
    long        status  = 0;
    std::string final_url;
    std::string content_type;
    size_t      bytes   = 0;
    std::vector<std::string> set_cookies;
};

// ── POSIX HTTP client ────────────────────────────────────────────────────────
//
// Thread-safety: not thread-safe. Use one instance per thread, or protect
// with a mutex. Each `fetch()` call is independent.
//
class PosixHTTPClient {
public:
    struct Options {
        std::chrono::milliseconds timeout_ms{30000};
        std::string               user_agent = "BulletScrape/1.0";
        int                        max_redirects = 5;
        std::string               proxy;
    };

    explicit PosixHTTPClient(const Options& opt) : opts_(opt) {}
    PosixHTTPClient() : opts_() {}

    HTTPResponse fetch(const std::string& url,
                      const std::string& method   = "GET",
                      const std::string& body      = "",
                      const std::unordered_map<std::string, std::string>* headers = nullptr,
                      int max_retries = 0,
                      std::chrono::milliseconds retry_delay = std::chrono::milliseconds(1000),
                      int redirect_count = 0) {

        std::string current_url = url;
        std::string current_body = body;
        std::unordered_map<std::string, std::string> current_headers;

        // Copy custom headers
        if (headers) {
            for (auto& [k, v] : *headers)
                current_headers[k] = v;
        }

        for (int attempt = 0; ; ++attempt) {
            auto [resp, err] = do_fetch(current_url, method, current_body, current_headers, redirect_count);
            if (!err || attempt >= max_retries || !is_retryable(resp.status))
                return resp;

            std::this_thread::sleep_for(retry_delay);
        }
    }

private:
    Options opts_;

    static bool is_retryable(long status) {
        return status == 429 || (status >= 500 && status < 600);
    }

    // Parse URL into components: scheme://host:port/path
    struct ParsedURL {
        std::string scheme;  // "http"
        std::string host;
        int         port    = 80;
        std::string path    = "/";
        std::string query;
    };

    static ParsedURL parse_url(const std::string& url) {
        ParsedURL result;
        std::string s = url;

        auto scheme_end = s.find("://");
        if (scheme_end != std::string::npos) {
            result.scheme = s.substr(0, scheme_end);
            s = s.substr(scheme_end + 3);
        }

        // Split host:port from path
        auto path_start = s.find('/');
        std::string hostport = (path_start != std::string::npos)
            ? s.substr(0, path_start) : s;
        if (path_start != std::string::npos)
            result.path = s.substr(path_start);

        // Split host:port
        auto colon = hostport.find(':');
        if (colon != std::string::npos) {
            result.host = hostport.substr(0, colon);
            result.port = std::stoi(hostport.substr(colon + 1));
        } else {
            result.host = hostport;
            result.port = (result.scheme == "https") ? 443 : 80;
        }

        // Split path and query
        auto qpos = result.path.find('?');
        if (qpos != std::string::npos) {
            result.query = result.path.substr(qpos + 1);
            result.path = result.path.substr(0, qpos);
        }

        if (result.path.empty()) result.path = "/";
        return result;
    }

    // Split host:port string
    struct FetchResult {
        HTTPResponse resp;
        std::optional<ScrapeError> err;
    };

    FetchResult do_fetch(const std::string& url,
                         const std::string& method,
                         const std::string& body,
                         const std::unordered_map<std::string, std::string>& headers,
                         int redirect_count) {
        auto parsed = parse_url(url);
        if (parsed.scheme != "http" && parsed.scheme != "https") {
            return {HTTPResponse{}, http_error(-1, url, "unsupported scheme: " + parsed.scheme)};
        }

        // For HTTPS, we cannot do TLS without a TLS library.
        // Return a clear error.
        if (parsed.scheme == "https") {
            return {HTTPResponse{},
                    http_error(0, url,
                        "HTTPS is not supported by the built-in HTTP client. "
                        "Install libcurl development headers and rebuild with -DBULLET_HAVE_CURL=ON, "
                        "or use an HTTP URL.")};
        }

        int sock = -1;
        struct sockaddr_in addr{};
        struct hostent* he = nullptr;

        auto cleanup = [&]() {
            if (sock >= 0) close(sock);
        };

        try {
            // DNS resolution
            he = gethostbyname(parsed.host.c_str());
            if (!he) {
                return {HTTPResponse{},
                        http_error(0, url, "DNS resolution failed for: " + parsed.host)};
            }

            // Create socket
            sock = socket(AF_INET, SOCK_STREAM, 0);
            if (sock < 0) {
                return {HTTPResponse{},
                        io_error("socket creation failed: " + std::string(strerror(errno)))};
            }

            // Set non-blocking for timeout
            int flags = fcntl(sock, F_GETFL, 0);
            fcntl(sock, F_SETFL, flags | O_NONBLOCK);

            std::memset(&addr, 0, sizeof(addr));
            addr.sin_family      = AF_INET;
            addr.sin_port        = htons(static_cast<uint16_t>(parsed.port));
            std::memcpy(&addr.sin_addr.s_addr, he->h_addr_list[0], he->h_length);

            // Connect with timeout
            auto t0 = std::chrono::steady_clock::now();
            int ret = connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
            if (ret < 0 && errno != EINPROGRESS) {
                return {HTTPResponse{},
                        io_error("connect failed: " + std::string(strerror(errno)))};
            }

            if (ret < 0 && errno == EINPROGRESS) {
                // Wait for connection with timeout
                fd_set wfds;
                FD_ZERO(&wfds);
                FD_SET(sock, &wfds);
                timeval tv{
                    static_cast<time_t>(opts_.timeout_ms.count() / 1000),
                    static_cast<suseconds_t>(opts_.timeout_ms.count() % 1000)
                };
                ret = select(sock + 1, nullptr, &wfds, nullptr, &tv);
                if (ret <= 0) {
                    cleanup();
                    return {HTTPResponse{},
                            timeout_error(url, "connection timed out")};
                }
                int err = 0;
                socklen_t err_len = sizeof(err);
                getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &err_len);
                if (err != 0) {
                    cleanup();
                    return {HTTPResponse{},
                            io_error("connect error: " + std::string(strerror(err)))};
                }
            }

            // Set back to blocking
            fcntl(sock, F_SETFL, flags);

            // Build HTTP request
            std::ostringstream req;
            req << method << " " << parsed.path;
            if (!parsed.query.empty()) req << "?" << parsed.query;
            req << " HTTP/1.1\r\n";
            req << "Host: " << parsed.host << "\r\n";
            req << "User-Agent: " << opts_.user_agent << "\r\n";
            req << "Accept: */*\r\n";
            req << "Accept-Encoding: gzip, deflate\r\n";
            req << "Connection: close\r\n";

            // Add custom headers (skip Host, User-Agent if already set)
            for (auto& [k, v] : headers) {
                std::string lk = k;
                for (auto& c : lk) c = (char)std::tolower((unsigned char)c);
                if (lk == "host" || lk == "user-agent" || lk == "connection")
                    continue;
                req << k << ": " << v << "\r\n";
            }

            if (method == "POST" || method == "PUT" || method == "PATCH") {
                req << "Content-Length: " << body.size() << "\r\n";
                req << "Content-Type: application/x-www-form-urlencoded\r\n";
            }

            req << "\r\n";
            std::string request_str = req.str();
            if (!body.empty() && (method == "POST" || method == "PUT" || method == "PATCH"))
                request_str += body;

            // Send request
            size_t sent = 0;
            while (sent < request_str.size()) {
                ssize_t n = send(sock, request_str.data() + sent,
                                 request_str.size() - sent, 0);
                if (n < 0) {
                    if (errno == EINTR) continue;
                    cleanup();
                    return {HTTPResponse{},
                            io_error("send failed: " + std::string(strerror(errno)))};
                }
                if (n == 0) break;
                sent += n;
            }

            // Read response
            std::string response;
            char buf[8192];
            auto t1 = std::chrono::steady_clock::now();
            while (true) {
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - t1).count();
                if (elapsed > (long)opts_.timeout_ms.count()) {
                    cleanup();
                    return {HTTPResponse{},
                                    timeout_error(url, "read timed out")};
                }
                ret = recv(sock, buf, sizeof(buf), 0);
                if (ret < 0) {
                    if (errno == EINTR) continue;
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        continue;
                    }
                    break; // Connection closed or error
                }
                if (ret == 0) break; // Connection closed
                response.append(buf, ret);
            }

            cleanup();

            // Parse HTTP response
            return parse_response(response, url, redirect_count);

        } catch (const std::exception& e) {
            cleanup();
            return {HTTPResponse{}, io_error(std::string("exception: ") + e.what())};
        }
    }

    FetchResult parse_response(const std::string& raw,
                                const std::string& url,
                                int redirect_count) {
        HTTPResponse resp;
        resp.final_url = url;

        // Split headers and body
        auto header_end = raw.find("\r\n\r\n");
        if (header_end == std::string::npos) {
            return {HTTPResponse{},
                    parse_error(url, "invalid HTTP response: no header/body separator")};
        }

        std::string header_section = raw.substr(0, header_end);
        std::string body           = raw.substr(header_end + 4);

        // Handle chunked transfer encoding
        auto content_encoding_pos = header_section.find("Transfer-Encoding: chunked");
        if (content_encoding_pos != std::string::npos) {
            body = decode_chunked(body);
        }

        // Parse status line
        auto first_line_end = header_section.find("\r\n");
        if (first_line_end == std::string::npos) {
            return {HTTPResponse{},
                    parse_error(url, "invalid HTTP response: no status line")};
        }
        std::string status_line = header_section.substr(0, first_line_end);
        // "HTTP/1.1 200 OK"
        auto space1 = status_line.find(' ');
        auto space2 = status_line.find(' ', space1 + 1);
        if (space1 != std::string::npos && space2 != std::string::npos) {
            std::string status_str = status_line.substr(space1 + 1, space2 - space1 - 1);
            try { resp.status = std::stol(status_str); }
            catch (...) { resp.status = 0; }
        }

        // Parse headers
        std::istringstream hstream(header_section);
        std::string line;
        std::getline(hstream, line); // skip status line

        while (std::getline(hstream, line)) {
            if (line.empty() || line == "\r") continue;
            // Remove trailing \r
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;

            auto colon = line.find(':');
            if (colon == std::string::npos) continue;

            std::string key = line.substr(0, colon);
            std::string val = line.substr(colon + 1);
            // trim leading space
            if (!val.empty() && val[0] == ' ') val.erase(0, 1);

            std::string lk = key;
            for (auto& c : lk) c = (char)std::tolower((unsigned char)c);

            if (lk == "content-type") {
                resp.content_type = val;
            } else if (lk == "set-cookie") {
                resp.set_cookies.push_back(val);
            } else if (lk == "location" && resp.status >= 300 && resp.status < 400) {
                // Handle redirect
                if (redirect_count < opts_.max_redirects) {
                    std::string loc = val;
                    if (loc[0] == '/') {
                        auto parsed = parse_url(url);
                        loc = parsed.scheme + "://" + parsed.host + loc;
                    }
                    return do_fetch(loc, "GET", "", {}, redirect_count + 1);
                }
            }
        }

        resp.body = body;
        resp.bytes = body.size();

        // Handle gzip encoding (basic: just treat as raw if we can't decompress)
        if (resp.content_type.find("gzip") != std::string::npos) {
            // Can't decompress without zlib — document this limitation
            // In production, link with zlib to handle this.
        }

        return {resp, std::nullopt};
    }

    static std::string decode_chunked(const std::string& data) {
        std::string result;
        size_t pos = 0;
        while (pos < data.size()) {
            // Find the chunk size line
            auto line_end = data.find("\r\n", pos);
            if (line_end == std::string::npos) break;
            std::string size_str = data.substr(pos, line_end - pos);
            // Remove chunk extensions (after ';')
            auto semi = size_str.find(';');
            if (semi != std::string::npos) size_str = size_str.substr(0, semi);
            size_t chunk_size;
            try { chunk_size = std::stoul(size_str, nullptr, 16); }
            catch (...) { break; }
            if (chunk_size == 0) break;
            pos = line_end + 2;
            if (pos + chunk_size > data.size()) break;
            result.append(data, pos, chunk_size);
            pos += chunk_size + 2; // skip trailing \r\n
        }
        return result;
    }
};

} // namespace bullet_scrape
