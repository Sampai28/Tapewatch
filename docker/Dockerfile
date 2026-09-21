# Tapewatch engine: build, test, and the API server runtime.
#
# Base is gcc:13 so the container compiler matches the g++ 13 the engine is
# developed and measured against. A code generation difference between the
# two would mean the tests validate a different binary from the one being
# timed, which quietly defeats having both.

# ---------------------------------------------------------------------------
# Stage 1: build
# ---------------------------------------------------------------------------
FROM gcc:13 AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
        cmake \
        ninja-build \
        git \
        ca-certificates \
        libsqlite3-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

# Build definitions first, so editing a source file does not invalidate the
# layer that fetched cpp-httplib.
COPY CMakeLists.txt ./
COPY include/ ./include/
COPY src/ ./src/
COPY tests/ ./tests/

RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j"$(nproc)"

# ---------------------------------------------------------------------------
# Stage 2: test -- `docker build --target test` runs everything
# ---------------------------------------------------------------------------
FROM build AS test

# python3 only; the Tapewatch Python side is standard-library only, on
# purpose, so there is no pip step here and nothing to pin.
RUN apt-get update && apt-get install -y --no-install-recommends python3 \
    && rm -rf /var/lib/apt/lists/*

COPY python/ ./python/
COPY tools/ ./tools/
COPY configs/ ./configs/

ENV PYTHONPATH=/src/python
CMD set -e; \
    echo "=== C++ unit tests ==="; \
    ./build/tapewatch-tests; \
    echo "=== Python unit tests ==="; \
    python3 -m unittest discover -s python/tests -t . -v; \
    echo "=== end-to-end on a short tape ==="; \
    python3 tools/mkdetcfg.py --config configs/smoke.yaml --out /tmp/detectors.json; \
    python3 -m generate.main --config configs/smoke.yaml --out /tmp/run; \
    python3 tools/check_tape.py /tmp/run/tape.csv || true; \
    ./build/tapewatch-detect --input /tmp/run/tape.csv --config /tmp/detectors.json \
        --alerts /tmp/run/alerts.jsonl --manifest /tmp/run/detect.json; \
    python3 -m eval.main --config configs/smoke.yaml --run /tmp/run; \
    python3 -m eval.report --run /tmp/run --out /tmp/run/report.html; \
    echo "ALL CHECKS PASSED"

# ---------------------------------------------------------------------------
# Stage 3: runtime -- just the API server
# ---------------------------------------------------------------------------
FROM debian:bookworm-slim AS runtime

# curl is here only for the compose healthcheck. The obvious zero-dependency
# alternative, bash's /dev/tcp, does not work: `CMD-SHELL` runs the check in
# /bin/sh, which on Debian is dash, and dash has no /dev/tcp.
RUN apt-get update && apt-get install -y --no-install-recommends \
        libsqlite3-0 \
        ca-certificates \
        curl \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --create-home --shell /usr/sbin/nologin tapewatch

# GCC 13's libstdc++, copied from the build image.
#
# Debian bookworm ships GCC 12's, which stops at GLIBCXX_3.4.30; a binary
# compiled by gcc:13 needs 3.4.32 and dies at load with a message that points
# at the runtime image while the cause is the compiler choice. Copying the
# library beats pinning the build to GCC 12, because matching the development
# compiler is the reason the build image is gcc:13 in the first place.
COPY --from=build /usr/local/lib64/libstdc++.so.6* /usr/local/lib64/
ENV LD_LIBRARY_PATH=/usr/local/lib64

WORKDIR /app
COPY --from=build /src/build/tapewatch-api /app/tapewatch-api

USER tapewatch
EXPOSE 8090

# The database and the tape are produced on the host by `make all` and
# mounted in; the container does not generate data.
ENTRYPOINT ["/app/tapewatch-api", "--host", "0.0.0.0", "--port", "8090"]
CMD ["--db", "/data/tapewatch.db", "--tape", "/data/tape.csv"]
