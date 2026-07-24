#pragma once

#include "HttpServer.h"
#include "HttpTypes.h"

#include "config/rabbit/rabbitConfig.h"

class IJobPublisher;
class JobRepository;
class Metrics;
class AuthStubClient;

// Раньше это были две вложенные лямбды прямо внутри main() — обработчик
// POST /jobs один занимал 111 строк (парсинг, проверка идемпотентности,
// вызов auth-stub, публикация в две очереди — всё в одной функции). Из-за
// этого код не был покрыт тестами: тестировалось только то, что уже было
// вынесено в api-server-core (JobRepository, HttpServer, Metrics).
//
// Зависимости — через конструктор, через абстракции там, где это разрывает
// тяжёлую зависимость (IJobPublisher вместо amqpcpp), и напрямую — там, где
// зависимость и так лёгкая и не мешает тестам (JobRepository уже
// тестируется через SQLite, AuthStubClient — через QNetworkAccessManager
// на реальный локальный сервер, как в tst_httpserver.cpp).
class JobsController {
public:
    JobsController(IJobPublisher &publisher, JobRepository &jobRepository,
                    AuthStubClient &authClient, Metrics &metrics, RabbitConfig rabbitConfig);

    // Асинхронный — respond() может быть вызван не сразу (после ответа
    // auth-stub), поэтому HttpServer::RespondFn, а не прямой возврат.
    void handleCreate(const HttpRequest &req, HttpServer::RespondFn respond) const;

    // Синхронный — чтение из Postgres через JobRepository::findById,
    // ничего асинхронного внутри нет.
    HttpResponse handleGet(const HttpRequest &req) const;

private:
    IJobPublisher &m_publisher;
    JobRepository &m_jobRepository;
    AuthStubClient &m_authClient;
    Metrics &m_metrics;
    RabbitConfig m_rabbitConfig;
};
