# syntax=docker/dockerfile:1
FROM gcc:13-bookworm AS build
WORKDIR /src
RUN apt-get update && apt-get install -y --no-install-recommends cmake && rm -rf /var/lib/apt/lists/*
COPY CMakeLists.txt .
COPY src ./src
RUN rm -rf build && \
	cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --parallel

FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates && rm -rf /var/lib/apt/lists/*
COPY --from=build /src/build/log-collector-stub /usr/local/bin/log-collector-stub
EXPOSE 8085
ENTRYPOINT ["/usr/local/bin/log-collector-stub"]
