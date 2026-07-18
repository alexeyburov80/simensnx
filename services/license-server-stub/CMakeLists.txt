cmake_minimum_required(VERSION 3.16)
project(license-server-stub LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# Только Core + Network: состояние — in-memory (см. LicensePool.h), ни
# RabbitMQ, ни PostgreSQL этому сервису на Phase 0 не нужны.
find_package(Qt5 5.15 REQUIRED COMPONENTS Core Network)

add_executable(license-server-stub
    src/main.cpp
    src/HttpServer.cpp
    src/LicensePool.cpp
)

target_link_libraries(license-server-stub PRIVATE Qt5::Core Qt5::Network)
