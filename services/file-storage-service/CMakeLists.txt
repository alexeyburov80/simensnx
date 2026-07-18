cmake_minimum_required(VERSION 3.16)
project(file-storage-service LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# Только Core + Network: сервис — чистое HTTP-хранилище файлов поверх PV
# (см. ARCHITECTURE.md), с RabbitMQ никак не взаимодействует, поэтому здесь
# сознательно нет AMQP-CPP/FetchContent, в отличие от api-server и
# nx-worker-stub.
find_package(Qt5 5.15 REQUIRED COMPONENTS Core Network)

add_executable(file-storage-service
    src/main.cpp
    src/HttpServer.cpp
)

target_link_libraries(file-storage-service PRIVATE Qt5::Core Qt5::Network)
