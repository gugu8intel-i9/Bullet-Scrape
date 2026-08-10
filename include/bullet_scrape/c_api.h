/* ============================================================================
 * bullet_scrape C API
 * ---------------------------------------------------------------------------
 * Stable C ABI for language bindings (Python ctypes/cffi, Go cgo, etc.).
 * Thread-safe: each bullet_scraper_t is independent; concurrent scrapers OK.
 *
 * Build a shared library:
 *   make shared          # produces libbullet_scrape.so
 * ============================================================================ */
#ifndef BULLET_SCRAPE_C_API_H
#define BULLET_SCRAPE_C_API_H

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) || defined(__CYGWIN__)
  #ifdef BULLET_BUILD_SHARED
    #define BULLET_API __declspec(dllexport)
  #elif defined(BULLET_USE_SHARED)
    #define BULLET_API __declspec(dllimport)
  #else
    #define BULLET_API
  #endif
#else
  #define BULLET_API __attribute__((visibility("default")))
#endif

#include <stddef.h>
#include <stdint.h>

/* Opaque scraper handle */
typedef struct bullet_scraper bullet_scraper_t;

/* Run statistics returned by bullet_scrape_run / bullet_scrape_extract */
typedef struct bullet_stats {
    int64_t  total_ms;
    int64_t  total_bytes;
    int      succeeded;
    int      failed;
    int      url_count;
    int      record_count;
    double   pages_per_sec;
    double   mb_per_sec;
} bullet_stats_t;

/* Version / capability queries ------------------------------------------------ */

/** Library version string, e.g. "1.0.0" */
BULLET_API const char* bullet_version(void);

/**
 * Capability bitmask:
 *   bit 0 = libcurl backend (HTTPS / HTTP/2 / gzip)
 *   bit 1 = XPath (pugixml)
 *   bit 2 = multi interface / connection reuse
 */
BULLET_API unsigned bullet_capabilities(void);

/** Human-readable backend description (static string). */
BULLET_API const char* bullet_backend_info(void);

/* Lifecycle ------------------------------------------------------------------- */

/** Create a scraper. Returns NULL on OOM. */
BULLET_API bullet_scraper_t* bullet_scraper_create(void);

/** Destroy a scraper and free all associated memory. */
BULLET_API void bullet_scraper_destroy(bullet_scraper_t* s);

/**
 * Load config from a JSON file path.
 * Returns 0 on success, non-zero on error (use bullet_last_error).
 */
BULLET_API int bullet_scraper_load_file(bullet_scraper_t* s, const char* path);

/**
 * Load config from a JSON string (UTF-8).
 * Returns 0 on success, non-zero on error.
 */
BULLET_API int bullet_scraper_load_json(bullet_scraper_t* s, const char* json_utf8);

/**
 * Override output path after load (empty / "stdout" → capture in memory).
 * Returns 0 on success.
 */
BULLET_API int bullet_scraper_set_output_path(bullet_scraper_t* s, const char* path);

/**
 * Override max concurrent workers after load.
 * Returns 0 on success.
 */
BULLET_API int bullet_scraper_set_concurrency(bullet_scraper_t* s, int n);

/* Execution ------------------------------------------------------------------- */

/**
 * Run the configured scrape job.
 *
 * On success returns 0 and writes a NUL-terminated JSON array of records into
 * *out_json (malloc'd — free with bullet_free). stats may be NULL.
 *
 * On failure returns non-zero; *out_json is set to NULL.
 */
BULLET_API int bullet_scrape_run(bullet_scraper_t* s,
                                 char** out_json,
                                 bullet_stats_t* stats);

/**
 * Offline extraction: run configured queries against raw HTML (no network).
 * Ideal for micro-benchmarks and unit tests from Python.
 *
 * html_len may be (size_t)-1 to use strlen.
 */
BULLET_API int bullet_scrape_extract(bullet_scraper_t* s,
                                     const char* html,
                                     size_t html_len,
                                     const char* base_url,
                                     char** out_json,
                                     bullet_stats_t* stats);

/**
 * Fetch a single URL with the scraper's HTTP settings (no extraction).
 * Returns response body (malloc'd) on success; sets *status and *bytes.
 */
BULLET_API int bullet_http_get(bullet_scraper_t* s,
                               const char* url,
                               char** out_body,
                               long* status,
                               size_t* bytes);

/* Errors / memory ------------------------------------------------------------- */

/** Thread-local last error message (valid until next API call on this thread). */
BULLET_API const char* bullet_last_error(void);

/** Free a buffer returned by the API (out_json / out_body). */
BULLET_API void bullet_free(void* p);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* BULLET_SCRAPE_C_API_H */
