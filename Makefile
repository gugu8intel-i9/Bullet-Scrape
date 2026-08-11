# Bullet Scrape — Makefile
# =========================
# Builds the library, CLI, shared lib (C API), and Python extension helpers.
#
# Requirements:
#   g++ >= 8 (C++17)
#   libcurl (libcurl4-openssl-dev) — strongly recommended for HTTPS
#   pthread
#
# Usage:
#   make              # build bullet-scrape binary + static lib
#   make shared       # build libbullet_scrape.so (Python / Colab)
#   make test         # run unit tests
#   make bench        # run micro-benchmark
#   make colab        # build shared lib optimised for Colab/manylinux
#   make clean

CXX       ?= g++
CXXSTD    := -std=c++17
WARN      := -Wall -Wextra -Wpedantic -Werror=return-type
OPT       := -O3 -DNDEBUG -ffast-math -funroll-loops
#OPT       := -O2 -g -DDEBUG   # uncomment for debug build
PIC       := -fPIC
VIS       := -fvisibility=hidden -fvisibility-inlines-hidden

# Detect OS
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    SOEXT := dylib
    SOFLAGS := -dynamiclib -install_name @rpath/libbullet_scrape.$(SOEXT)
    # Prefer brew curl on macOS
    BREW_CURL := $(shell brew --prefix curl 2>/dev/null)
    ifneq ($(BREW_CURL),)
        CURL_CFLAGS := -I$(BREW_CURL)/include
        CURL_LIBS   := -L$(BREW_CURL)/lib -lcurl
    else
        CURL_CFLAGS := $(shell pkg-config --cflags libcurl 2>/dev/null)
        CURL_LIBS   := $(shell pkg-config --libs libcurl 2>/dev/null || echo "-lcurl")
    endif
else
    SOEXT := so
    SOFLAGS := -shared -Wl,-soname,libbullet_scrape.$(SOEXT).1
    CURL_CFLAGS := $(shell pkg-config --cflags libcurl 2>/dev/null)
    CURL_LIBS   := $(shell pkg-config --libs libcurl 2>/dev/null || echo "-lcurl")
endif

# Auto-detect libcurl headers (must actually compile — preprocessor -E can lie)
HAS_CURL := $(shell printf '%s\n' '#include <curl/curl.h>' 'int main(){return 0;}' | $(CXX) -x c++ $(CURL_CFLAGS) -fsyntax-only - 2>/dev/null && echo yes || echo no)
ifeq ($(HAS_CURL),yes)
    CURL_DEFS  := -DBULLET_HAVE_CURL -DHAVE_CURL
    CURL_LINK  := $(CURL_LIBS)
    BACKEND_MSG := libcurl
else
    CURL_DEFS  :=
    CURL_LINK  :=
    BACKEND_MSG := posix-fallback
endif

# nlohmann/json single-header location (optional — we ship mini_json)
JSON_HEADER_DIR := include/third_party
JSON_HEADER     := $(JSON_HEADER_DIR)/json.hpp

SRCDIR   := src
INCDIR   := include
BUILDDIR := build/obj

LIB_SRCS := \
    $(SRCDIR)/core/cleaner.cpp \
    $(SRCDIR)/core/config.cpp \
    $(SRCDIR)/core/http_client.cpp \
    $(SRCDIR)/core/extractor.cpp \
    $(SRCDIR)/core/output.cpp \
    $(SRCDIR)/core/scraper.cpp \
    $(SRCDIR)/core/c_api.cpp

LIB_OBJS := $(patsubst $(SRCDIR)/%.cpp,$(BUILDDIR)/%.o,$(LIB_SRCS))

CLI_SRCS := $(SRCDIR)/cli/main.cpp
CLI_OBJS := $(patsubst $(SRCDIR)/%.cpp,$(BUILDDIR)/%.o,$(CLI_SRCS))

CXXFLAGS := $(CXXSTD) $(WARN) $(OPT) $(PIC) $(VIS) \
            -I$(INCDIR) $(CURL_CFLAGS) $(CURL_DEFS) \
            -pthread

LDFLAGS  := -pthread $(CURL_LINK)

# ── Default ──────────────────────────────────────────────────────────────────

.PHONY: all
all: bullet-scrape libbullet_scrape.a
	@echo ""
	@echo "Built with backend: $(BACKEND_MSG)"
	@echo "  ./bullet-scrape --help"

# ── Objects ──────────────────────────────────────────────────────────────────

# Header dependencies are auto-generated (-MMD -MP) so touching a header
# rebuilds the affected objects — plain `.cpp → .o` rules miss that.
DEPFLAGS = -MMD -MP

$(BUILDDIR)/%.o: $(SRCDIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -c $< -o $@

# ── Static library ───────────────────────────────────────────────────────────

libbullet_scrape.a: $(LIB_OBJS)
	ar rcs $@ $^
	@echo "Built: $@"

# ── Shared library (C API) ───────────────────────────────────────────────────

.PHONY: shared
shared: libbullet_scrape.$(SOEXT)

libbullet_scrape.$(SOEXT): $(LIB_OBJS)
	$(CXX) $(SOFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "Built: $@  (backend: $(BACKEND_MSG))"

# Colab / manylinux optimised shared object
.PHONY: colab
colab:
	$(MAKE) clean-objs
	$(MAKE) shared OPT="-O3 -DNDEBUG -ffast-math -funroll-loops -march=x86-64-v2" \
	               CXXFLAGS_EXTRA="-D_GLIBCXX_USE_CXX11_ABI=1"
	@mkdir -p python/bullet_scrape
	@cp -f libbullet_scrape.$(SOEXT) python/bullet_scrape/
	@echo "Copied libbullet_scrape.$(SOEXT) → python/bullet_scrape/"

# ── CLI binary ───────────────────────────────────────────────────────────────

bullet-scrape: $(LIB_OBJS) $(CLI_OBJS)
	$(CXX) $(CXXSTD) $(OPT) -o $@ \
		$(LIB_OBJS) $(CLI_OBJS) \
		$(LDFLAGS)
	@echo "Built: ./bullet-scrape  (backend: $(BACKEND_MSG))"

# ── Tests ────────────────────────────────────────────────────────────────────

TEST_SRCS := tests/test_main.cpp tests/test_extractor.cpp tests/test_cleaner.cpp tests/test_regex_engine.cpp
TEST_OBJS := $(patsubst tests/%.cpp,$(BUILDDIR)/tests/%.o,$(TEST_SRCS))

$(BUILDDIR)/tests/%.o: tests/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -c $< -o $@

bullet_scrape_tests: $(TEST_OBJS) $(LIB_OBJS)
	$(CXX) $(CXXSTD) $(OPT) -o $@ $(TEST_OBJS) $(LIB_OBJS) $(LDFLAGS)
	@echo "Built: ./bullet_scrape_tests"

-include $(LIB_OBJS:.o=.d) $(CLI_OBJS:.o=.d) $(TEST_OBJS:.o=.d)

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

# Detailed extraction benchmark (workload table)
bullet_scrape_bench: bench/bench.cpp $(LIB_OBJS)
	$(CXX) $(CXXFLAGS) $(OPT) -o $@ bench/bench.cpp $(LIB_OBJS) $(LDFLAGS)
	@echo "Built: ./bullet_scrape_bench"

.PHONY: bench-full
bench-full: bullet_scrape_bench
	./bullet_scrape_bench

.PHONY: example
example: bullet-scrape
	./bullet-scrape --example

# ── Python package install (editable, local) ─────────────────────────────────

.PHONY: python
python: shared
	@mkdir -p python/bullet_scrape
	@cp -f libbullet_scrape.$(SOEXT) python/bullet_scrape/
	@echo "Shared library staged in python/bullet_scrape/"
	@echo "Install with:  pip install -e ./python"

# ── Clean ────────────────────────────────────────────────────────────────────

.PHONY: clean clean-objs
clean-objs:
	rm -rf $(BUILDDIR)

clean: clean-objs
	rm -f bullet-scrape bullet_scrape_tests
	rm -f libbullet_scrape.a libbullet_scrape.so libbullet_scrape.dylib
	rm -f python/bullet_scrape/libbullet_scrape.so python/bullet_scrape/libbullet_scrape.dylib
	rm -rf build/

# ── Install ──────────────────────────────────────────────────────────────────

PREFIX ?= /usr/local

.PHONY: install
install: bullet-scrape libbullet_scrape.a shared
	install -d $(PREFIX)/bin
	install -m 755 bullet-scrape $(PREFIX)/bin/bullet-scrape
	install -d $(PREFIX)/lib
	install -m 644 libbullet_scrape.a $(PREFIX)/lib/
	install -m 755 libbullet_scrape.$(SOEXT) $(PREFIX)/lib/
	install -d $(PREFIX)/include/bullet_scrape
	cp -r include/bullet_scrape/* $(PREFIX)/include/bullet_scrape/
	@echo "Installed to $(PREFIX)"

# ── Help ─────────────────────────────────────────────────────────────────────

.PHONY: help
help:
	@echo "Bullet Scrape — Makefile targets"
	@echo ""
	@echo "  make            Build CLI + static library"
	@echo "  make shared     Build libbullet_scrape.$(SOEXT) (C API / Python)"
	@echo "  make colab      Optimised shared lib + stage into python/"
	@echo "  make python     Build shared lib and stage for pip install"
	@echo "  make test       Build and run unit tests"
	@echo "  make bench      Run micro-benchmark"
	@echo "  make example    Print example JSON config"
	@echo "  make clean      Remove all build artifacts"
	@echo "  make install    Install to PREFIX ($(PREFIX))"
	@echo ""
	@echo "Detected HTTP backend: $(BACKEND_MSG)"
	@echo "Requirements: g++ >= 8, pthread; libcurl recommended for HTTPS"
