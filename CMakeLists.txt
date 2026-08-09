cmake_minimum_required(VERSION 3.14)
project(bullet_scrape VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Options ──────────────────────────────────────────────────────────────────
option(BULLET_BUILD_TESTS   "Build unit tests"    OFF)
option(BULLET_BUILD_BENCH   "Build benchmarks"    OFF)
option(BULLET_BUILD_EXAMPLES "Build examples"      OFF)
option(BULLET_FAST_BUILD    "Skip nlohmann fetch, assume installed" OFF)

# ── Dependencies ─────────────────────────────────────────────────────────────

# libcurl
find_package(CURL REQUIRED)
if(NOT CURL_FOUND)
    message(FATAL_ERROR "libcurl is required. Install:\n"
                        "  Ubuntu:  sudo apt install libcurl4-openssl-dev\n"
                        "  macOS:   brew install curl\n"
                        "  Windows: vcpkg install curl")
endif()

# nlohmann/json — single header, fetch at configure time
if(BULLET_FAST_BUILD)
    find_package(nlohmann_json REQUIRED)
else()
    include(FetchContent)
    FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG        v3.11.3
        GIT_SHALLOW    ON
    )
    FetchContent_MakeAvailable(nlohmann_json)
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

# ── Include directories ──────────────────────────────────────────────────────
include_directories(${CMAKE_SOURCE_DIR}/include)

# ── Source files ─────────────────────────────────────────────────────────────
set(BULLET_SOURCES
    src/core/config.cpp
    src/core/http_client.cpp
    src/core/extractor.cpp
    src/core/output.cpp
    src/core/scraper.cpp
)

# ── Library ──────────────────────────────────────────────────────────────────
add_library(bullet_scrape STATIC ${BULLET_SOURCES})
target_link_libraries(bullet_scrape
    PUBLIC CURL::libcurl
    PUBLIC nlohmann_json::nlohmann_json
)
target_include_directories(bullet_scrape PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

if(HAVE_PUGIXML)
    target_link_libraries(bullet_scrape PUBLIC PugiXML::PugiXML)
    target_compile_definitions(bullet_scrape PUBLIC BULLET_HAVE_XPATH)
endif()

# ── CLI binary ───────────────────────────────────────────────────────────────
add_executable(bullet-scrape src/cli/main.cpp)
target_link_libraries(bullet-scrape PRIVATE bullet_scrape)
target_compile_definitions(bullet-scrape PRIVATE BULLET_VERSION="1.0.0")

# ── Tests ────────────────────────────────────────────────────────────────────
if(BULLET_BUILD_TESTS)
    enable_testing()
    add_executable(bullet_scrape_tests tests/test_main.cpp
        tests/test_extractor.cpp
        tests/test_config.cpp
    )
    target_link_libraries(bullet_scrape_tests PRIVATE bullet_scrape)
    add_test(NAME bullet_scrape_extractor  COMMAND bullet_scrape_tests --filter extractor)
    add_test(NAME bullet_scrape_config     COMMAND bullet_scrape_tests --filter config)
endif()

# ── Benchmarks ───────────────────────────────────────────────────────────────
if(BULLET_BUILD_BENCH)
    add_executable(bullet_scrape_bench bench.cpp)
    target_link_libraries(bullet_scrape_bench PRIVATE bullet_scrape)
endif()

# ── Install ──────────────────────────────────────────────────────────────────
install(TARGETS bullet_scrape bullet-scrape
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
)
install(DIRECTORY include/bullet_scrape
    DESTINATION include
)

# ── Print config ─────────────────────────────────────────────────────────────
message(STATUS "")
message(STATUS "  Bullet Scrape ${PROJECT_VERSION}")
message(STATUS "  C++ standard : ${CMAKE_CXX_STANDARD}")
message(STATUS "  libcurl      : ${CURL_VERSION_STRING}")
message(STATUS "  nlohmann/json: ${nlohmann_json_VERSION}")
message(STATUS "  pugixml      : ${PugiXML_FOUND}")
message(STATUS "  build tests  : ${BULLET_BUILD_TESTS}")
message(STATUS "")
