#include "PostgresConnection.h"

#include <QSqlDatabase>

bool openPostgresConnection(const QUrl &databaseUrl) {
    QSqlDatabase db = QSqlDatabase::addDatabase("QPSQL");
    db.setHostName(databaseUrl.host());
    db.setPort(databaseUrl.port(5432));
    QString dbName = databaseUrl.path();
    if (dbName.startsWith('/')) dbName.remove(0, 1);
    db.setDatabaseName(dbName);
    db.setUserName(databaseUrl.userName());
    db.setPassword(databaseUrl.password());
    db.setConnectOptions("connect_timeout=5");
    return db.open();
}
