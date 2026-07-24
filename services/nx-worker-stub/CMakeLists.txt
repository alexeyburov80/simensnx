cmake_minimum_required(VERSION 3.16)
project(nx-worker-stub LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt5 5.15 REQUIRED COMPONENTS Core Network)

# HttpServer/HttpTypes/Metrics — общие для всех сервисов, см. common/CMakeLists.txt.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../common ${CMAKE_CURRENT_BINARY_DIR}/common)

# См. подробный комментарий в services/api-server/CMakeLists.txt: AMQP-CPP
# не пакетирован для Debian bookworm/Ubuntu 24.04, поэтому FetchContent с
# пином на тег, а не apt и не vendoring в git.
include(FetchContent)
FetchContent_Declare(
    amqpcpp
    GIT_REPOSITORY https://github.com/CopernicaMarketingSoftware/AMQP-CPP.git
    GIT_TAG v4.3.27
    GIT_SHALLOW TRUE
)
set(AMQP-CPP_LINUX_TCP ON CACHE BOOL "" FORCE)
set(AMQP-CPP_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(AMQP-CPP_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(AMQP-CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(amqpcpp)

add_executable(nx-worker-stub
    src/main.cpp
    src/QtAmqpConsumer.cpp
)

target_link_libraries(nx-worker-stub PRIVATE simensnx-common amqpcpp)
