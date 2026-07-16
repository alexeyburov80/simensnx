# syntax=docker/dockerfile:1
FROM gcc:13-bookworm AS build
WORKDIR /src
RUN apt-get update && apt-get install -y --no-install-recommends cmake && rm -rf /var/lib/apt/lists/*
COPY CMakeLists.txt .
COPY src ./src
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --parallel

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates && rm -rf /var/lib/apt/lists/*
COPY --from=build /src/build/api-server /usr/local/bin/api-server
EXPOSE 8080
ENTRYPOINT ["/usr/local/bin/api-server"]
