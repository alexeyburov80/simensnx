cmake_minimum_required(VERSION 3.16)
project(file-storage-service CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(file-storage-service src/main.cpp)
