# Tapewatch.
#
#   make all        generate, detect, evaluate, report -- the whole pipeline
#   make up         bring up the API and the console in Docker
#
# Two build paths on purpose. CMake builds everything including the API
# server, which needs SQLite and cpp-httplib; a plain `g++` build handles the
# detection engine and its tests with nothing but a compiler, which is what
# you want when the machine has no package manager and you are not allowed to
# install one. `make build` picks whichever is available.

SHELL       := /bin/bash
CONFIG      ?= configs/small.yaml
RUN         ?= results/small
REPORT      ?= reports/$(notdir $(RUN)).html
PY          ?= python3
CXX         ?= g++
CXXFLAGS    ?= -std=c++20 -O2 -DNDEBUG -Wall -Wextra -Wno-unused-parameter
JOBS        ?= $(shell nproc 2>/dev/null || echo 2)

NATIVE_DIR  := build-native
CMAKE_DIR   := build
export PYTHONPATH := $(CURDIR)/python

CORE_SRC    := $(wildcard src/engine/*.cpp) $(wildcard src/detectors/*.cpp) \
               $(wildcard src/validation/*.cpp)
TEST_SRC    := $(wildcard tests/*.cpp)
DETECT_BIN  := $(NATIVE_DIR)/tapewatch-detect
TESTS_BIN   := $(NATIVE_DIR)/tapewatch-tests

HAVE_CMAKE  := $(shell command -v cmake >/dev/null 2>&1 && echo yes)

.PHONY: all build build-native build-cmake test generate detect eval report \
        ui-install ui-dev ui-build ui-test up down clean distclean check-tape help

help:
	@echo "make build      compile the engine and tests"
	@echo "make test       run the C++ suite and the Python suite"
	@echo "make all        build, test, generate, detect, eval, report"
	@echo "make up/down    start/stop the API and console containers"
	@echo "make ui-dev     run the console against a locally running API"
	@echo ""
	@echo "CONFIG=$(CONFIG)  RUN=$(RUN)"

# --- build -----------------------------------------------------------------

build:
ifeq ($(HAVE_CMAKE),yes)
	@$(MAKE) --no-print-directory build-cmake
	@$(MAKE) --no-print-directory build-native
else
	@echo "cmake not found -- building the engine and tests with $(CXX) only."
	@echo "The API server needs cmake, SQLite and cpp-httplib; use 'make up'."
	@$(MAKE) --no-print-directory build-native
endif

build-cmake:
	cmake -S . -B $(CMAKE_DIR) -DCMAKE_BUILD_TYPE=Release
	cmake --build $(CMAKE_DIR) -j$(JOBS)

# The native path is what every other target depends on, so that a checkout
# with nothing but a compiler can still run the whole analysis pipeline.
build-native: $(DETECT_BIN) $(TESTS_BIN)

# mkdir inside the recipe rather than an order-only prerequisite on the
# directory: make treats a directory target that is also the output's parent
# as a circular dependency and says so on every build.
$(DETECT_BIN): $(CORE_SRC) $(wildcard include/tapewatch/*.hpp) $(wildcard src/detectors/*.hpp)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -Iinclude -Isrc $(CORE_SRC) -o $@

$(TESTS_BIN): $(CORE_SRC) $(TEST_SRC) $(wildcard include/tapewatch/*.hpp) \
              $(wildcard tests/*.hpp) $(wildcard src/detectors/*.hpp)
	@mkdir -p $(@D)
	$(CXX) -std=c++20 -O1 -g -Wall -Wextra -Wno-unused-parameter -Iinclude -Isrc -Itests \
	  $(filter-out src/engine/detect_main.cpp,$(CORE_SRC)) $(TEST_SRC) -o $@

# --- test ------------------------------------------------------------------

# unittest, not pytest. The Python side has no third-party dependencies and
# adding one purely to run the tests would mean the suite cannot run on a
# machine with no pip -- which is the machine this was built on.
test: $(TESTS_BIN)
	$(TESTS_BIN)
	$(PY) -m unittest discover -s python/tests -t . -v

# --- pipeline --------------------------------------------------------------

generate:
	@mkdir -p $(RUN)
	$(PY) tools/mkdetcfg.py --config $(CONFIG) --out $(RUN)/detectors.json
	$(PY) -m generate.main --config $(CONFIG) --out $(RUN)

check-tape:
	$(PY) tools/check_tape.py $(RUN)/tape.csv

detect: $(DETECT_BIN)
	$(PY) tools/mkdetcfg.py --config $(CONFIG) --out $(RUN)/detectors.json
	$(DETECT_BIN) --input $(RUN)/tape.csv --config $(RUN)/detectors.json \
	  --alerts $(RUN)/alerts.jsonl --manifest $(RUN)/detect.json

eval:
	$(PY) -m eval.main --config $(CONFIG) --run $(RUN)

report:
	@mkdir -p reports
	$(PY) -m eval.report --run $(RUN) --out $(REPORT)

all: build test generate detect eval report
	@echo ""
	@echo "Pipeline complete. Report: $(REPORT)"

# --- console ---------------------------------------------------------------

ui/node_modules:
	cd ui && npm install

ui-install: ui/node_modules

ui-dev: ui/node_modules
	cd ui && npm run dev

ui-build: ui/node_modules
	cd ui && npm run build

ui-test: ui/node_modules
	cd ui && npm run test -- --run

# --- containers ------------------------------------------------------------

up:
	docker compose -f docker/docker-compose.yml up --build -d
	@echo "console  http://localhost:5180"
	@echo "api      http://localhost:8090/api/health"

down:
	docker compose -f docker/docker-compose.yml down -v

docker-test:
	docker build -f docker/Dockerfile --target test -t tapewatch-test .
	docker run --rm tapewatch-test

# --- housekeeping ----------------------------------------------------------

clean:
	rm -rf $(NATIVE_DIR) $(CMAKE_DIR)

distclean: clean
	rm -rf results/* reports/* ui/node_modules ui/dist
