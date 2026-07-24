#pragma once

#include <QCoreApplication>
#include <QString>

// Раньше был продублирован (логически идентично, см. историю рефакторинга)
// в api-server, job-state-service и nx-worker-stub: g_sigFd + unixSignalHandler
// + installSignalHandlers. Инфраструктурный код, к бизнес-логике конкретного
// сервиса отношения не имеет — вынесен сюда.
//
// Почему self-pipe, а не просто std::signal(SIGTERM, [](int){ qApp->quit(); }):
// обработчик POSIX-сигнала выполняется в async-signal-unsafe контексте, где
// нельзя звать почти ничего (ни malloc, ни большинство Qt API, ни даже
// qInfo()). Единственное безопасное действие — write() в заранее открытый
// файловый дескриптор. Реальная реакция (app.quit(), логирование) происходит
// уже в Qt event loop, когда QSocketNotifier увидит байт в трубе.
//
// serviceName попадает только в лог о получении сигнала — под каждый сервис
// отдельно вызывать эту функцию с своим именем, глобального состояния
// с привязкой к конкретному сервису здесь нет.
void installSignalHandlers(QCoreApplication &app, const QString &serviceName);
