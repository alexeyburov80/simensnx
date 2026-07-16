#include <httplib.h>
#include <cstdlib>
#include <iostream>
#include <string>

int main() {
    const std::string service_name = "api-server";
    const std::string port = std::getenv("PORT") ? std::getenv("PORT") : "8080";

    std::cout << "[" << service_name << "] starting HTTP server on port "
              << port << std::endl;
    std::cout << "[" << service_name << "] status: STUB — no business logic, see ROADMAP.md"
              << std::endl;

    httplib::Server svr;

    // Корневой путь
    svr.Get("/", [](const httplib::Request& req, httplib::Response& res) {
        res.set_content(R"({"status": "ok", "service": "api-server-stub"})", "application/json");
    });

    // Health check
    svr.Get("/health", [](const httplib::Request& req, httplib::Response& res) {
        res.set_content(R"({"status": "healthy"})", "application/json");
    });

    // Создание задачи (заглушка)
    svr.Post("/jobs", [](const httplib::Request& req, httplib::Response& res) {
        std::string response = R"({
            "job_id": "stub-job-123",
            "status": "pending",
            "message": "STUB: job accepted, real logic coming in Phase 2"
        })";
        res.set_content(response, "application/json");
    });

    // Получение статуса задачи (заглушка)
    svr.Get(R"(/jobs/(\d+))", [](const httplib::Request& req, httplib::Response& res) {
        std::string job_id = req.matches[1];
        std::string response = "{\n"
                               "    \"job_id\": \"" + job_id + "\",\n"
                                          "    \"status\": \"completed\",\n"
                                          "    \"result\": \"STUB: validation passed (fake)\",\n"
                                          "    \"message\": \"STUB: real logic coming in Phase 2\"\n"
                                          "}";
        res.set_content(response, "application/json");
    });

    // Запуск сервера
    if (!svr.listen("0.0.0.0", std::stoi(port))) {
        std::cerr << "[" << service_name << "] failed to start server" << std::endl;
        return 1;
    }

    return 0;
}
