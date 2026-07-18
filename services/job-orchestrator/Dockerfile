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

COPY . .
RUN cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build --parallel

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    libqt5core5a \
    libqt5network5 \
    libqt5sql5-psql \
    libssl3 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /src/build/job-orchestrator /usr/local/bin/job-orchestrator

RUN useradd --system --no-create-home --shell /usr/sbin/nologin nx
USER nx

EXPOSE 8082
ENTRYPOINT ["/usr/local/bin/job-orchestrator"]
