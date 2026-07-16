// Заглушка сервиса. Реальная бизнес-логика появится в рамках ROADMAP.md.
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

int main() {
    const std::string service_name = "auth-stub";
    const std::string port = std::getenv("PORT") ? std::getenv("PORT") : "8081";

    std::cout << "[" << service_name << "] stub started, would listen on port "
              << port << std::endl;
    std::cout << "[" << service_name << "] status: STUB — no business logic, see ROADMAP.md"
              << std::endl;

    // Заглушка живёт бесконечно и раз в 10 секунд пишет heartbeat в лог,
    // чтобы было видно в docker/k8s логах, что контейнер жив.
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        std::cout << "[" << service_name << "] heartbeat" << std::endl;
    }
    return 0;
}
