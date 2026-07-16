cmake_minimum_required(VERSION 3.15)
project(api-server)

set(CMAKE_CXX_STANDARD 17)

# Fetch httplib
include(FetchContent)
FetchContent_Declare(
    httplib
    URL https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.18.1.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(httplib)

add_executable(api-server src/main.cpp)
target_link_libraries(api-server PRIVATE httplib)