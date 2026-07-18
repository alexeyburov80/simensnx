cmake_minimum_required(VERSION 3.16)
project(job-orchestrator LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt5 5.15 REQUIRED COMPONENTS Core Network Sql)

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

add_executable(job-orchestrator
    src/main.cpp
    src/HttpServer.cpp
    src/QtAmqpConsumer.cpp
)

target_link_libraries(job-orchestrator PRIVATE Qt5::Core Qt5::Network Qt5::Sql amqpcpp)
