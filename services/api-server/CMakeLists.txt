cmake_minimum_required(VERSION 3.16)
project(api-server LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# Тесты — отдельный проект/цель, не часть main.cpp (ROADMAP, Phase 0:
# "добавить проект unit тестов QTest отдельно от основного кода"). Включены
# по умолчанию всегда — этот CMakeLists.txt не используется как
# add_subdirectory() из какого-либо родительского монорепо-билда (каждый
# сервис собирается независимо, см. docker-compose.yml/Dockerfile), так что
# специально определять "мы тут top-level или нет" не нужно. Раньше здесь
# было "${PROJECT_IS_TOP_LEVEL}" — эта переменная существует только с
# CMake 3.21+, на более старом CMake (в т.ч. встроенном в некоторые версии
# Qt Creator) она разворачивалась в пустую строку, option() трактовал её
# как OFF, и tests/ молча не подключался.
option(API_SERVER_BUILD_TESTS "Собирать QTest-юниты для api-server" ON)
# Позволяет сконфигурировать ТОЛЬКО api-server-core + tests, вообще не
# трогая FetchContent(amqpcpp) (а значит и не требуя OpenSSL dev-заголовков
# и сети до GitHub). Актуально для быстрого CI-джоба "юнит-тесты" отдельно
# от джоба "собрать образ": `cmake -DAPI_SERVER_BUILD_APP=OFF -DAPI_SERVER_BUILD_TESTS=ON`.
option(API_SERVER_BUILD_APP "Собирать сам исполняемый api-server (тянет amqpcpp через FetchContent)" ON)
# Покрытие кода тестами (gcov). Инструментируем ТОЛЬКО api-server-core —
# именно её код гоняют tst_*, инструментировать main.cpp/QtAmqpConnection.cpp
# смысла нет, они тестами не вызываются вовсе. Включать явно
# (-DAPI_SERVER_COVERAGE=ON), а не всегда: --coverage раздувает бинарники и
# исключает оптимизации, для обычной локальной сборки/CI-джоба "юнит-тесты"
# не нужен.
option(API_SERVER_COVERAGE "Собрать api-server-core с инструментацией gcov (--coverage)" OFF)

find_package(Qt5 5.15 REQUIRED COMPONENTS Core Network Sql)

# api-server-core — всё, что НЕ зависит от amqpcpp/QtAmqpConnection и не
# является main(). Вынесено в отдельную статическую библиотеку ровно для
# того, чтобы tests/ мог слинковаться с реальным production-кодом, не
# перетягивая AMQP-CPP (FetchContent, сборка из исходников) в тестовую
# сборку — тестам она не нужна, а собирается заметно дольше самого кода.
add_library(api-server-core STATIC
    src/HttpServer.cpp
    src/HttpServer.h
    src/HttpTypes.h
    src/Metrics.h
    src/config/env.h
    src/config/appConfig.h
    src/config/appConfig.cpp
    src/config/rabbit/rabbitConfig.h
    src/config/http/httpStatus.h
    src/config/http/routes.h
    src/storage/jobrepository.h
    src/storage/jobrepository.cpp
)
target_include_directories(api-server-core PUBLIC src)
target_link_libraries(api-server-core PUBLIC Qt5::Core Qt5::Network Qt5::Sql)

if(API_SERVER_COVERAGE)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR "API_SERVER_COVERAGE поддержан только для GCC/Clang (--coverage)")
    endif()
    # -O0 -g поверх --coverage — без этого gcov даёт недостоверные строки
    # покрытия (оптимизатор схлопывает/переупорядочивает блоки кода).
    target_compile_options(api-server-core PUBLIC --coverage -O0 -g)
    target_link_options(api-server-core PUBLIC --coverage)
endif()

if(API_SERVER_BUILD_APP)
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
        src/QtAmqpConnection.cpp
    )
    target_link_libraries(api-server PRIVATE api-server-core amqpcpp)
endif()

if(API_SERVER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
