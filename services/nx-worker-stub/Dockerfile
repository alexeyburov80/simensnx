FROM debian:bookworm AS build
WORKDIR /src

RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake \
    ninja-build \
    build-essential \
    git \
    ca-certificates \
    qtbase5-dev \
    libssl-dev \
    && rm -rf /var/lib/apt/lists/*

COPY common/ common/
COPY services/nx-worker-stub/ services/nx-worker-stub/
RUN cmake -B build -S services/nx-worker-stub -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build --parallel

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    libqt5core5a \
    libqt5network5 \
    libssl3 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /src/build/nx-worker-stub /usr/local/bin/nx-worker-stub

RUN useradd --system --no-create-home --shell /usr/sbin/nologin nx
USER nx

# 8085 — только /health (liveness) и /metrics (Prometheus scrape), никакой
# бизнес-логики через HTTP сюда не приходит — воркер остаётся consumer'ом.
EXPOSE 8085
ENTRYPOINT ["/usr/local/bin/nx-worker-stub"]
