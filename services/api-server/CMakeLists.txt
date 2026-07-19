cmake_minimum_required(VERSION 3.16)
project(api-server LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt5 5.15 REQUIRED COMPONENTS Core Network Sql)

# AMQP-CPP не пакетирован для Debian bookworm / Ubuntu 24.04 (проверено:
# отсутствует в apt на обоих дистрибутивах на момент написания, появляется
# только начиная с Debian trixie/Ubuntu 26.04). Поэтому тянем его через
# FetchContent с фиксацией на конкретный релизный тег, а не через apt и не
# кладём исходники в репозиторий:
#   - vendoring исходников в git раздул бы репозиторий (~компилируемая C++
#     библиотека) и снял бы с нас обязанность когда-либо обновлять её вручную;
#   - тег вместо ветки/HEAD даёт воспроизводимую сборку — GIT_TAG ниже пиннит
#     точную версию, обновление — осознанный PR с изменением одной строки.
# Если сборка когда-нибудь должна стать полностью офлайновой (air-gapped),
# это единственное место, которое нужно заменить на локальный submodule.
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

add_executable(api-server
    src/main.cpp
    src/HttpServer.cpp
    src/QtAmqpConnection.cpp
)

target_link_libraries(api-server PRIVATE Qt5::Core Qt5::Network Qt5::Sql amqpcpp)
