# syntax=docker/dockerfile:1.7

# ── Builder stage ────────────────────────────────────────────────────
# Uses Debian 12 + gcc 12 + cmake. Builds KenshiMP.Server and
# KenshiMP.MasterServer. Client-only subprojects (Core, Injector,
# Scanner, tests) are skipped automatically by the platform-guarded
# CMakeLists.txt.
FROM debian:12-slim AS builder

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        ca-certificates \
        git \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
# Copy submodule contents alongside top-level files. Build context must
# include the lib/ submodules already (the GitHub Actions checkout uses
# `submodules: recursive`).
COPY . .

RUN cmake -S . -B /build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build /build --config Release --parallel \
    && strip /build/bin/KenshiMP.Server /build/bin/KenshiMP.MasterServer

# ── Runtime stage ────────────────────────────────────────────────────
# Tiny runtime image. Only stdlib and libgcc (already in -slim).
FROM debian:12-slim AS runtime

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        libstdc++6 \
        ca-certificates \
        tini \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --create-home --uid 10001 --shell /usr/sbin/nologin kenshi

# Binaries land in /usr/local/bin. Saves and config under /data.
COPY --from=builder /build/bin/KenshiMP.Server       /usr/local/bin/KenshiMP.Server
COPY --from=builder /build/bin/KenshiMP.MasterServer /usr/local/bin/KenshiMP.MasterServer
COPY dist/server.json /etc/kenshi-online/server.json.example

RUN mkdir -p /data && chown -R kenshi:kenshi /data

USER kenshi
WORKDIR /data
VOLUME ["/data"]

# Game traffic: 27800/UDP (server), 27801/UDP (master server, optional)
EXPOSE 27800/udp
EXPOSE 27801/udp

# tini handles SIGTERM cleanly so the server's atexit save runs
ENTRYPOINT ["/usr/bin/tini", "--", "/usr/local/bin/KenshiMP.Server"]

# Default: read /data/server.json (mounted from host). If absent, the
# server writes a default file on first run. Override the binary by
# passing a different command, e.g.:
#   docker run ... ghcr.io/.../kenshi-online-server /usr/local/bin/KenshiMP.MasterServer
CMD ["/data/server.json"]
