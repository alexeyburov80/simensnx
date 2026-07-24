cmake_minimum_required(VERSION 3.16)

# Не самостоятельный проект — подключается через add_subdirectory() из
# CMakeLists.txt каждого сервиса. project()/cmake_minimum_required() в
# сервисном CMakeLists.txt уже заданы к моменту, когда мы сюда попадаем.

find_package(Qt5 5.15 REQUIRED COMPONENTS Core Network)

# HttpTypes.h/Metrics.h/RabbitTopics.h — header-only, но всё равно входят
# сюда (не просто "добавь include-путь"), чтобы IDE и dependency-трекинг
# CMake видели их как часть цели, а не терялись при рефакторинге.
add_library(simensnx-common STATIC
    HttpServer.cpp
    HttpServer.h
    HttpTypes.h
    Metrics.h
    RabbitTopics.h
    GracefulShutdown.cpp
    GracefulShutdown.h
    JobPublisher.h
)

# PUBLIC + сам каталог common/ (а не common/http и т.п.) — специально,
# чтобы существующие "#include \"HttpServer.h\"" (без под-каталога) в
# main.cpp каждого сервиса продолжали работать без единой правки.
target_include_directories(simensnx-common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(simensnx-common PUBLIC Qt5::Core Qt5::Network)

# Отдельный таргет, не часть simensnx-common: PostgresConnection тянет
# Qt5::Sql, который нужен только api-server и job-state-service — остальные
# 4 сервиса (auth-stub, license-server-stub, file-storage-service,
# nx-worker-stub) вообще не трогают Postgres, и заставлять их линковать
# Qt5::Sql только потому что он нужен двум другим сервисам — лишняя
# зависимость без причины (тот же принцип, что ISP: не заставляй клиента
# зависеть от того, чем он не пользуется).
find_package(Qt5 5.15 REQUIRED COMPONENTS Sql)
add_library(simensnx-common-sql STATIC
    PostgresConnection.cpp
    PostgresConnection.h
)
target_include_directories(simensnx-common-sql PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(simensnx-common-sql PUBLIC Qt5::Core Qt5::Sql)
