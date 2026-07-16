# syntax=docker/dockerfile:1
FROM gcc:13-bookworm AS build
WORKDIR /src
RUN apt-get update && apt-get install -y --no-install-recommends cmake && rm -rf /var/lib/apt/lists/*
COPY CMakeLists.txt .
COPY src ./src
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --parallel

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates && rm -rf /var/lib/apt/lists/*
COPY --from=build /src/build/job-orchestrator /usr/local/bin/job-orchestrator
EXPOSE 8082
ENTRYPOINT ["/usr/local/bin/job-orchestrator"]
