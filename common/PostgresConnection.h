#pragma once

#include <QUrl>

// Раньше был побайтово продублирован (проверено diff'ом) в api-server и
// job-state-service под именем openDatabase(). Добавляет соединение с
// именем по умолчанию ("qt_sql_default_connection") через QSqlDatabase::
// addDatabase("QPSQL") — вызывающий код обращается к нему через QSqlQuery()
// без явного имени соединения, как и раньше.
bool openPostgresConnection(const QUrl &databaseUrl);
