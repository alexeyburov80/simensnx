cmake_minimum_required(VERSION 3.16)
project(auth-stub LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# Только Core + Network: чистый HTTP-стаб, ни RabbitMQ, ни PostgreSQL ему на
# Phase 0 не нужны (реальная БД пользователей/токенов появится в Phase 3
# вместе с настоящим OAuth2/OIDC).
find_package(Qt5 5.15 REQUIRED COMPONENTS Core Network)

# HttpServer/HttpTypes/Metrics — общие для всех сервисов, см. common/CMakeLists.txt.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../common ${CMAKE_CURRENT_BINARY_DIR}/common)

add_executable(auth-stub
    src/main.cpp
)

target_link_libraries(auth-stub PRIVATE simensnx-common)

