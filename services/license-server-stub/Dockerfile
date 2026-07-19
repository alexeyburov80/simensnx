FROM debian:bookworm AS build
WORKDIR /src

RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake \
    ninja-build \
    build-essential \
    qtbase5-dev \
    && rm -rf /var/lib/apt/lists/*

COPY . .
RUN rm -rf build && \
    cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build --parallel

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    libqt5core5a \
    libqt5network5 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /src/build/license-server-stub /usr/local/bin/license-server-stub

RUN useradd --system --no-create-home --shell /usr/sbin/nologin nx
USER nx

EXPOSE 8083
ENTRYPOINT ["/usr/local/bin/license-server-stub"]
