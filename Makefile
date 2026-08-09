# Bullet Scrape — Makefile
# =========================
# Builds the library and CLI binary without CMake.
#
# Requirements:
#   g++ >= 8 (C++17)
#   libcurl (libcurl4-openssl-dev on Debian/Ubuntu)
#   nlohmann/json — single header, auto-fetched if missing
#
# Usage:
#   make              # build bullet-scrape binary
#   make test         # run unit tests
#   make bench        # run micro-benchmark
#   make clean        # remove build artifacts
#   make install      # install to /usr/local
#   make fetch-json   # download nlohmann/json single header

CXX       := g++
CXXSTD    := -std=c++17
WARN      := -Wall -Wextra -Wpedantic -Werror=return-type
OPT       := -O2 -DNDEBUG
#OPT       := -O2 -g -DDEBUG   # uncomment for debug build

# Detect OS
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    CURL_FLAGS := $(shell pkg-config --cflags --libs libcurl 2>/dev/null || echo "-lcurl")
else
    CURL_FLAGS := $(shell pkg-config --cflags --libs libcurl 2>/dev/null || echo "-lcurl")
endif

# nlohmann/json single-header location
JSON_HEADER_DIR := include/third_party
JSON_HEADER     := $(JSON_HEADER_DIR)/json.hpp

# Source files
SRCDIR   := src
INCDIR   := include

LIB_SRCS := \
    $(SRCDIR)/core/config.cpp \
    $(SRCDIR)/core/http_client.cpp \
    $(SRCDIR)/core/extractor.cpp \
    $(SRCDIR)/core/output.cpp \
    $(SRCDIR)/core/scraper.cpp

LIB_OBJS := $(patsubst %.cpp,%.o,$(LIB_SRCS))

CLI_SRCS := $(SRCDIR)/cli/main.cpp
CLI_OBJS := $(patsubst %.cpp,%.o,$(CLI_SRCS))

TEST_SRCS := \
    tests/test_main.cpp \
    tests/test_extractor.cpp
TEST_OBJS := $(patsubst %.cpp,%.o,$(TEST_SRCS))

# ── Default target ───────────────────────────────────────────────────────────

.PHONY: all
all: bullet-scrape

# ── nlohmann/json fetch ──────────────────────────────────────────────────────

.PHONY: fetch-json
fetch-json:
	@echo "Fetching nlohmann/json single header..."
	@mkdir -p $(JSON_HEADER_DIR)
	@curl -fsSL \
	  "https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp" \
	  -o $(JSON_HEADER) || \
	curl -fsSL \
	  "https://raw.githubusercontent.com/nlohmann/json/develop/single_include/nlohmann/json.hpp" \
	  -o $(JSON_HEADER) || \
	(echo "ERROR: could not fetch json.hpp — place it at $(JSON_HEADER) manually" && exit 1)
	@echo "Done: $(JSON_HEADER)"

# ── Ensure JSON header exists ───────────────────────────────────────────────

NOTICE_JSON := $(shell \
    if [ ! -f $(JSON_HEADER) ]; then \
        echo "NOTE: nlohmann/json.hpp not found at $(JSON_HEADER)"; \
        echo "       Run: make fetch-json  (requires internet)"; \
        echo "       Or download manually and place at that path."; \
        echo ""; \
    fi)

# ── Library objects ──────────────────────────────────────────────────────────

CXXFLAGS := $(CXXSTD) $(WARN) $(OPT) -I$(INCDIR) -I$(INCDIR)/bullet_scrape

$(SRCDIR)/core/%.o: $(SRCDIR)/core/%.cpp $(JSON_HEADER)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(SRCDIR)/cli/%.o: $(SRCDIR)/cli/%.cpp $(JSON_HEADER)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(TEST_SRCS:.cpp=.o): $(TEST_SRCS) $(JSON_HEADER)
	@mkdir -p tests
	$(CXX) $(CXXFLAGS) -c $< -o $@

# ── Binary: bullet-scrape ───────────────────────────────────────────────────

bullet-scrape: $(LIB_OBJS) $(CLI_OBJS)
	@echo "$(NOTICE_JSON)"
	$(CXX) $(CXXSTD) $(OPT) -o $@ \
		$(LIB_OBJS) $(CLI_OBJS) \
		$(CURL_FLAGS) -lpthread
	@echo "Built: ./bullet-scrape"

# ── Test binary ──────────────────────────────────────────────────────────────

tests/test_main.o: tests/test_main.cpp $(JSON_HEADER)
	@mkdir -p tests
	$(CXX) $(CXXFLAGS) -c $< -o $@

tests/test_extractor.o: tests/test_extractor.cpp $(JSON_HEADER)
	@mkdir -p tests
	$(CXX) $(CXXFLAGS) -c $< -o $@

bullet_scrape_tests: tests/test_main.o tests/test_extractor.o $(LIB_OBJS)
	@echo "$(NOTICE_JSON)"
	$(CXX) $(CXXSTD) $(OPT) -o $@ \
		tests/test_main.o tests/test_extractor.o $(LIB_OBJS) \
		$(CURL_FLAGS) -lpthread
	@echo "Built: ./bullet_scrape_tests"

.PHONY: test
test: bullet_scrape_tests
	@echo "=== Running unit tests ==="
	./bullet_scrape_tests
	@echo "=== Done ==="

# ── Benchmark ────────────────────────────────────────────────────────────────

.PHONY: bench
bench: bullet-scrape
	@echo "=== Running micro-benchmark ==="
	./bullet-scrape --bench
	@echo "=== Done ==="

# ── Example config ───────────────────────────────────────────────────────────

.PHONY: example
example: bullet-scrape
	./bullet-scrape --example

# ── Clean ────────────────────────────────────────────────────────────────────

.PHONY: clean
clean:
	rm -f $(LIB_OBJS) $(CLI_OBJS) $(TEST_OBJS)
	rm -f bullet-scrape bullet_scrape_tests
	rm -rf build/

# ── Install ──────────────────────────────────────────────────────────────────

PREFIX ?= /usr/local

.PHONY: install
install: bullet-scrape
	install -d $(PREFIX)/bin
	install -m 755 bullet-scrape $(PREFIX)/bin/bullet-scrape
	install -d $(PREFIX)/include/bullet_scrape
	cp -r include/bullet_scrape/* $(PREFIX)/include/bullet_scrape/
	@echo "Installed to $(PREFIX)"

# ── Help ─────────────────────────────────────────────────────────────────────

.PHONY: help
help:
	@echo "Bullet Scrape — Makefile targets"
	@echo ""
	@echo "  make            Build the bullet-scrape binary"
	@echo "  make test       Build and run unit tests"
	@echo "  make bench      Run micro-benchmark"
	@echo "  make example    Print example JSON config"
	@echo "  make fetch-json Download nlohmann/json header (if missing)"
	@echo "  make clean      Remove all build artifacts"
	@echo "  make install    Install binary and headers to PREFIX ($(PREFIX))"
	@echo ""
	@echo "Requirements: g++ >= 8, libcurl, nlohmann/json header"
