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
        gosu \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --create-home --uid 10001 --shell /usr/sbin/nologin kenshi

# Binaries land in /usr/local/bin. Saves and config under /data.
COPY --from=builder /build/bin/KenshiMP.Server       /usr/local/bin/KenshiMP.Server
COPY --from=builder /build/bin/KenshiMP.MasterServer /usr/local/bin/KenshiMP.MasterServer
COPY docker/entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod +x /usr/local/bin/entrypoint.sh

WORKDIR /data
VOLUME ["/data"]

# Game traffic: 27800/UDP (server), 27801/UDP (master server, optional)
EXPOSE 27800/udp
EXPOSE 27801/udp

# Entrypoint runs as root, chown's /data to PUID:PGID (defaults to
# kenshi/10001), then drops privileges via gosu and exec's tini + server.
# Override PUID/PGID for Unraid (e.g. PUID=99 PGID=100 for nobody:users).
# To run the master server instead:
#   docker run ... <image> --entrypoint=/usr/bin/tini -- gosu kenshi /usr/local/bin/KenshiMP.MasterServer
ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
CMD ["/data/server.json"]
