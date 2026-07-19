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

COPY --from=build /src/build/file-storage-service /usr/local/bin/file-storage-service

# /data — точка монтирования volume (filestorage в docker-compose.yml,
# PVC в k8s). Раньше запись велась бы под root; создаём каталог заранее и
# отдаём его невладельцу-сервису.
RUN useradd --system --no-create-home --shell /usr/sbin/nologin nx \
    && mkdir -p /data && chown nx:nx /data
USER nx

EXPOSE 8084
ENTRYPOINT ["/usr/local/bin/file-storage-service"]
