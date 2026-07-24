find_package(Qt5 5.15 REQUIRED COMPONENTS Test)

# Один тестовый бинарник на один QObject-класс с тестами — стандартная для
# QTest практика (каждый .cpp сам себе QTEST_MAIN/QTEST_GUILESS_MAIN, свой
# event loop, свой процесс). Помогает и функция ниже: добавить новый файл
# тестов — значит просто дописать его в список qtest_add_test(...) вызовов,
# без копипасты add_executable/target_link_libraries/add_test.
function(qtest_add_test name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE api-server-core Qt5::Test)
    # QTEST_GUILESS_MAIN (см. каждый tst_*.cpp) не поднимает GUI-цикл — для
    # CI без X-сервера/Wayland это обязательно, иначе тесты падают на
    # старте с "could not connect to display".
    add_test(NAME ${name} COMMAND ${name})
    # Тесты HttpServer открывают реальный TCP-сокет на 127.0.0.1 — таймаут
    # с запасом на случай медленного/загруженного CI-раннера.
    set_tests_properties(${name} PROPERTIES TIMEOUT 60)
endfunction()

qtest_add_test(tst_httptypes tst_httptypes.cpp)
qtest_add_test(tst_metrics tst_metrics.cpp)
qtest_add_test(tst_appconfig tst_appconfig.cpp)
qtest_add_test(tst_jobrepository tst_jobrepository.cpp)
qtest_add_test(tst_jobscontroller tst_jobscontroller.cpp)
qtest_add_test(tst_httpserver tst_httpserver.cpp)
