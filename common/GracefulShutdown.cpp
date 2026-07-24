#include "GracefulShutdown.h"

#include <QSocketNotifier>
#include <QDebug>

#include <csignal>
#include <sys/socket.h>
#include <unistd.h>

namespace {

// Файловый дескриптор самопайпа — обязан быть файл-скоуп переменной (не
// членом класса, не локальной), потому что sigaction() принимает голый C
// function pointer без возможности захвата контекста. Один процесс — один
// набор обработчиков сигналов, так что глобальность здесь не потеря
// абстракции, а прямое следствие POSIX API.
int g_sigFd[2];

void unixSignalHandler(int) {
    char a = 1;
    if (::write(g_sigFd[1], &a, sizeof(a)) != sizeof(a)) {
        // Сознательно ничего не делаем: мы в обработчике сигнала, куда
        // нельзя safely звать даже qWarning(). Если write не удался,
        // следующий сигнал (SIGKILL от оркестратора после таймаута) всё
        // равно завершит процесс.
    }
}

} // namespace

void installSignalHandlers(QCoreApplication &app, const QString &serviceName) {
    ::socketpair(AF_UNIX, SOCK_STREAM, 0, g_sigFd);
    auto *notifier = new QSocketNotifier(g_sigFd[0], QSocketNotifier::Read, &app);
    QObject::connect(notifier, &QSocketNotifier::activated, &app,
                      [&app, notifier, serviceName](QSocketDescriptor, QSocketNotifier::Type) {
        char tmp;
        const auto n = ::read(g_sigFd[0], &tmp, sizeof(tmp));
        (void)n; // значение не важно — сам факт активации notifier'а достаточен
        notifier->setEnabled(false);
        qInfo().noquote() << QString("[%1] shutdown signal received, stopping").arg(serviceName);
        app.quit();
    });

    struct sigaction sa{};
    sa.sa_handler = unixSignalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
}
