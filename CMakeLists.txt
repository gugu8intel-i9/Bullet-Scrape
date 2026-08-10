cmake_minimum_required(VERSION 3.14)
project(bullet_scrape VERSION 1.0.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Hide symbols by default; C API exports explicitly
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ── Options ──────────────────────────────────────────────────────────────────
option(BULLET_BUILD_TESTS    "Build unit tests"           OFF)
option(BULLET_BUILD_BENCH    "Build benchmarks"           OFF)
option(BULLET_BUILD_EXAMPLES "Build examples"             OFF)
option(BULLET_BUILD_SHARED   "Build shared library (C API / Python)" ON)
option(BULLET_FAST_BUILD     "Skip nlohmann fetch, assume installed" OFF)
option(BULLET_FORCE_POSIX    "Force POSIX HTTP (ignore libcurl)" OFF)

# ── Dependencies ─────────────────────────────────────────────────────────────

# libcurl — preferred backend (HTTPS, HTTP/2, gzip, connection pooling)
set(BULLET_WITH_CURL OFF)
if(NOT BULLET_FORCE_POSIX)
    find_package(CURL)
    if(CURL_FOUND)
        set(BULLET_WITH_CURL ON)
        message(STATUS "libcurl found — HTTPS/HTTP2/gzip enabled")
    else()
        message(STATUS "libcurl not found — using POSIX HTTP fallback (no HTTPS)")
    endif()
else()
    message(STATUS "BULLET_FORCE_POSIX=ON — skipping libcurl")
endif()

# nlohmann/json — optional; we ship mini_json. Keep fetch for compatibility.
if(BULLET_FAST_BUILD)
    find_package(nlohmann_json QUIET)
endif()

# pugixml — optional, for XPath support
find_package(PugiXML QUIET)
if(PugiXML_FOUND)
    message(STATUS "pugixml found — XPath queries enabled")
    set(HAVE_PUGIXML ON)
else()
    message(STATUS "pugixml not found — XPath queries disabled")
    set(HAVE_PUGIXML OFF)
endif()

# Threads
find_package(Threads REQUIRED)

# ── Include directories ──────────────────────────────────────────────────────
include_directories(${CMAKE_SOURCE_DIR}/include)

# ── Source files ─────────────────────────────────────────────────────────────
set(BULLET_SOURCES
    src/core/config.cpp
    src/core/http_client.cpp
    src/core/extractor.cpp
    src/core/output.cpp
    src/core/scraper.cpp
    src/core/c_api.cpp
)

# ── Static library ───────────────────────────────────────────────────────────
add_library(bullet_scrape_static STATIC ${BULLET_SOURCES})
set_target_properties(bullet_scrape_static PROPERTIES OUTPUT_NAME bullet_scrape)
target_include_directories(bullet_scrape_static PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(bullet_scrape_static PUBLIC Threads::Threads)

if(BULLET_WITH_CURL)
    target_link_libraries(bullet_scrape_static PUBLIC CURL::libcurl)
    target_compile_definitions(bullet_scrape_static PUBLIC BULLET_HAVE_CURL HAVE_CURL)
endif()

if(HAVE_PUGIXML)
    target_link_libraries(bullet_scrape_static PUBLIC PugiXML::PugiXML)
    target_compile_definitions(bullet_scrape_static PUBLIC BULLET_HAVE_XPATH)
endif()

# Alias used by the rest of the tree
add_library(bullet_scrape ALIAS bullet_scrape_static)

# ── Shared library (C API — used by Python / Colab) ──────────────────────────
if(BULLET_BUILD_SHARED)
    add_library(bullet_scrape_shared SHARED ${BULLET_SOURCES})
    set_target_properties(bullet_scrape_shared PROPERTIES
        OUTPUT_NAME bullet_scrape
        VERSION ${PROJECT_VERSION}
        SOVERSION 1
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
    )
    target_include_directories(bullet_scrape_shared PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    )
    target_link_libraries(bullet_scrape_shared PUBLIC Threads::Threads)
    target_compile_definitions(bullet_scrape_shared PRIVATE BULLET_BUILD_SHARED)

    if(BULLET_WITH_CURL)
        target_link_libraries(bullet_scrape_shared PUBLIC CURL::libcurl)
        target_compile_definitions(bullet_scrape_shared PUBLIC BULLET_HAVE_CURL HAVE_CURL)
    endif()

    if(HAVE_PUGIXML)
        target_link_libraries(bullet_scrape_shared PUBLIC PugiXML::PugiXML)
        target_compile_definitions(bullet_scrape_shared PUBLIC BULLET_HAVE_XPATH)
    endif()
endif()

# ── CLI binary ───────────────────────────────────────────────────────────────
add_executable(bullet-scrape src/cli/main.cpp)
target_link_libraries(bullet-scrape PRIVATE bullet_scrape_static)
target_compile_definitions(bullet-scrape PRIVATE BULLET_VERSION="1.0.0")

# ── Tests ────────────────────────────────────────────────────────────────────
if(BULLET_BUILD_TESTS)
    enable_testing()
    add_executable(bullet_scrape_tests
        tests/test_main.cpp
        tests/test_extractor.cpp
    )
    target_link_libraries(bullet_scrape_tests PRIVATE bullet_scrape_static)
    add_test(NAME bullet_scrape_extractor COMMAND bullet_scrape_tests --filter extractor)
endif()

# ── Benchmarks ───────────────────────────────────────────────────────────────
if(BULLET_BUILD_BENCH)
    add_executable(bullet_scrape_bench bench.cpp)
    target_link_libraries(bullet_scrape_bench PRIVATE bullet_scrape_static)
endif()

# ── Install ──────────────────────────────────────────────────────────────────
set(_install_targets bullet_scrape_static bullet-scrape)
if(BULLET_BUILD_SHARED)
    list(APPEND _install_targets bullet_scrape_shared)
endif()

install(TARGETS ${_install_targets}
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)
install(DIRECTORY include/bullet_scrape
    DESTINATION include
)
install(FILES include/bullet_scrape/c_api.h
    DESTINATION include/bullet_scrape
)

# ── Print config ─────────────────────────────────────────────────────────────
message(STATUS "")
message(STATUS "  Bullet Scrape ${PROJECT_VERSION}")
message(STATUS "  C++ standard : ${CMAKE_CXX_STANDARD}")
message(STATUS "  libcurl      : ${BULLET_WITH_CURL}")
message(STATUS "  pugixml      : ${HAVE_PUGIXML}")
message(STATUS "  shared lib   : ${BULLET_BUILD_SHARED}")
message(STATUS "  build tests  : ${BULLET_BUILD_TESTS}")
message(STATUS "")
