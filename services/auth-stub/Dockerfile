FROM debian:bookworm AS build
WORKDIR /src

RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake \
    ninja-build \
    build-essential \
    qtbase5-dev \
    && rm -rf /var/lib/apt/lists/*

COPY . .
RUN cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build --parallel

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    libqt5core5a \
    libqt5network5 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /src/build/auth-stub /usr/local/bin/auth-stub

RUN useradd --system --no-create-home --shell /usr/sbin/nologin nx
USER nx

EXPOSE 8081
ENTRYPOINT ["/usr/local/bin/auth-stub"]
