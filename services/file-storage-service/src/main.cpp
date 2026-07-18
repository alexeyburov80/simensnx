#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSocketNotifier>
#include <QStorageInfo>
#include <QUrl>
#include <QDebug>

#include <csignal>
#include <sys/socket.h>
#include <unistd.h>

#include "HttpServer.h"

namespace {

int g_sigFd[2];

void unixSignalHandler(int) {
    char a = 1;
    if (::write(g_sigFd[1], &a, sizeof(a)) != sizeof(a)) {
    }
}

void installSignalHandlers(QCoreApplication &app) {
    ::socketpair(AF_UNIX, SOCK_STREAM, 0, g_sigFd);
    auto *notifier = new QSocketNotifier(g_sigFd[0], QSocketNotifier::Read, &app);
    QObject::connect(notifier, &QSocketNotifier::activated, &app, [&app, notifier](QSocketDescriptor, QSocketNotifier::Type) {
        char tmp;
        const auto n = ::read(g_sigFd[0], &tmp, sizeof(tmp));
        (void)n;
        notifier->setEnabled(false);
        qInfo() << "[file-storage-service] shutdown signal received, stopping";
        app.quit();
    });

    struct sigaction sa{};
    sa.sa_handler = unixSignalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
}

QByteArray errorJson(const QString &message) {
    return QJsonDocument(QJsonObject{{"error", message}}).toJson(QJsonDocument::Compact);
}

// Декодирует и валидирует ключ файла из URL-пути. Единственная линия
// обороны от path traversal (../../etc/passwd) — раньше в репозитории
// подобной проверки не было нигде, потому что ни один сервис не принимал
// путь файла от клиента вообще.
//
// Возвращает пустую строку, если ключ невалиден.
QString sanitizeKey(const QString &rawEncoded) {
    const QString decoded = QUrl::fromPercentEncoding(rawEncoded.toUtf8());
    if (decoded.isEmpty() || decoded.size() > 512) return {};
    if (decoded.startsWith('/') || decoded.contains('\\') || decoded.contains('\0')) return {};

    const QStringList segments = decoded.split('/', Qt::KeepEmptyParts);
    for (const QString &seg : segments) {
        if (seg.isEmpty() || seg == "." || seg == "..") return {};
    }
    return decoded;
}

} // namespace

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    installSignalHandlers(app);

    bool portOk = false;
    const quint16 port = qEnvironmentVariable("PORT", "8084").toUShort(&portOk);
    if (!portOk) {
        qCritical() << "[file-storage-service] invalid PORT env var";
        return 1;
    }

    const QString storageDir = qEnvironmentVariable("STORAGE_DIR", "/data");
    QDir dir(storageDir);
    if (!dir.exists() && !dir.mkpath(".")) {
        qCritical() << "[file-storage-service] cannot create/access STORAGE_DIR:" << storageDir;
        return 1;
    }

    bool maxSizeOk = false;
    const qint64 maxBodyBytes = qEnvironmentVariable("MAX_UPLOAD_BYTES", "104857600").toLongLong(&maxSizeOk); // 100 МБ

    HttpServer server;
    server.setMaxBodyBytes(maxSizeOk && maxBodyBytes > 0 ? maxBodyBytes : 100LL * 1024 * 1024);

    server.addRoute("GET", "/health", [storageDir](const HttpRequest &) {
        QStorageInfo info(storageDir);
        if (!info.isValid() || !info.isReady()) {
            return HttpResponse::json(503, "Service Unavailable", errorJson("storage volume not ready"));
        }
        QJsonObject obj{
            {"status", "healthy"},
            {"bytes_available", QString::number(info.bytesAvailable())},
        };
        return HttpResponse::json(200, "OK", QJsonDocument(obj).toJson(QJsonDocument::Compact));
    });

    static const QRegularExpression filePattern(R"(^/files/(.+)$)");

    server.addRoutePattern("PUT", filePattern, [storageDir](const HttpRequest &req) -> HttpResponse {
        const QString key = sanitizeKey(req.pathParams.value(0));
        if (key.isEmpty()) {
            return HttpResponse::json(400, "Bad Request", errorJson("invalid or unsafe file key"));
        }

        const QString fullPath = QDir(storageDir).filePath(key);
        QDir().mkpath(QFileInfo(fullPath).absolutePath());

        // Пишем во временный файл и переименовываем — атомарно относительно
        // параллельного GET на тот же ключ (никогда не увидит частично
        // записанный файл), в отличие от прямой записи поверх существующего.
        QFile tmp(fullPath + ".tmp-" + QString::number(QCoreApplication::applicationPid()));
        if (!tmp.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            qWarning() << "[file-storage-service] failed to open for write:" << fullPath << tmp.errorString();
            return HttpResponse::json(500, "Internal Server Error", errorJson("failed to write file"));
        }
        const qint64 written = tmp.write(req.body);
        tmp.close();
        if (written != req.body.size()) {
            QFile::remove(tmp.fileName());
            return HttpResponse::json(500, "Internal Server Error", errorJson("short write to storage"));
        }
        QFile::remove(fullPath);
        if (!QFile::rename(tmp.fileName(), fullPath)) {
            QFile::remove(tmp.fileName());
            return HttpResponse::json(500, "Internal Server Error", errorJson("failed to finalize file"));
        }

        qInfo() << "[file-storage-service] stored" << key << "(" << req.body.size() << "bytes )";
        QJsonObject obj{{"key", key}, {"bytes", req.body.size()}};
        return HttpResponse::json(201, "Created", QJsonDocument(obj).toJson(QJsonDocument::Compact));
    });

    server.addRoutePattern("GET", filePattern, [storageDir](const HttpRequest &req) -> HttpResponse {
        const QString key = sanitizeKey(req.pathParams.value(0));
        if (key.isEmpty()) {
            return HttpResponse::json(400, "Bad Request", errorJson("invalid or unsafe file key"));
        }

        const QString fullPath = QDir(storageDir).filePath(key);
        QFile file(fullPath);
        if (!QFileInfo(fullPath).isFile() || !file.open(QIODevice::ReadOnly)) {
            return HttpResponse::json(404, "Not Found", errorJson("file not found"));
        }

        HttpResponse resp;
        resp.statusCode = 200;
        resp.statusText = "OK";
        resp.contentType = "application/octet-stream";
        resp.body = file.readAll();
        return resp;
    });

    server.addRoutePattern("DELETE", filePattern, [storageDir](const HttpRequest &req) -> HttpResponse {
        const QString key = sanitizeKey(req.pathParams.value(0));
        if (key.isEmpty()) {
            return HttpResponse::json(400, "Bad Request", errorJson("invalid or unsafe file key"));
        }

        const QString fullPath = QDir(storageDir).filePath(key);
        if (!QFileInfo(fullPath).isFile()) {
            return HttpResponse::json(404, "Not Found", errorJson("file not found"));
        }
        if (!QFile::remove(fullPath)) {
            return HttpResponse::json(500, "Internal Server Error", errorJson("failed to delete file"));
        }
        qInfo() << "[file-storage-service] deleted" << key;
        return HttpResponse::json(204, "No Content", "");
    });

    server.addRoute("GET", "/files", [storageDir](const HttpRequest &) {
        QJsonArray items;
        QDirIterator it(storageDir, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            const QFileInfo info = it.fileInfo();
            if (info.fileName().contains(".tmp-")) continue;
            QString relKey = QDir(storageDir).relativeFilePath(info.filePath());
            items.append(QJsonObject{{"key", relKey}, {"bytes", info.size()}});
        }
        return HttpResponse::json(200, "OK", QJsonDocument(items).toJson(QJsonDocument::Compact));
    });

    if (!server.listen(QHostAddress::AnyIPv4, port)) {
        qCritical() << "[file-storage-service] failed to listen on port" << port;
        return 1;
    }
    qInfo() << "[file-storage-service] listening on port" << port << "storage dir" << storageDir;

    return app.exec();
}
