FROM debian:bookworm AS build
WORKDIR /src

# git нужен только FetchContent'у для клонирования AMQP-CPP на пиннутый тег
# (см. комментарий в CMakeLists.txt — библиотека не пакетирована для bookworm).
# libssl-dev — прямая зависимость сборки AMQP-CPP (используется в её
# CMakeLists.txt через find_package(OpenSSL REQUIRED), даже когда TLS сам
# по себе не используется нашим кодом).
RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake \
    ninja-build \
    build-essential \
    git \
    ca-certificates \
    qtbase5-dev \
    libssl-dev \
    && rm -rf /var/lib/apt/lists/*

# Контекст сборки — корень репозитория (см. docker-compose.yml), не
# services/api-server: common/ используется несколькими сервисами (см.
# common/CMakeLists.txt), а Docker не даёт выйти за пределы build context
# через "../..". Копируем точечно (не COPY . .), чтобы правка кода в
# другом сервисе не инвалидировала кэш этого слоя.
COPY common/ common/
COPY services/api-server/ services/api-server/
RUN cmake -B build -S services/api-server -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build --parallel

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    libqt5core5a \
    libqt5network5 \
    libqt5sql5-psql \
    libssl3 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /src/build/api-server /usr/local/bin/api-server

# Непривилегированный пользователь — раньше сервис всегда работал под root.
RUN useradd --system --no-create-home --shell /usr/sbin/nologin nx
USER nx

EXPOSE 8080
ENTRYPOINT ["/usr/local/bin/api-server"]
