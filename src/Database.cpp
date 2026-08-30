#include "Database.h"

#include <QDir>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QStringList>
#include <QVariant>

namespace {

void clearError(QString *error)
{
    if (error) {
        error->clear();
    }
}

void setError(QString *error, const QString &message)
{
    if (error) {
        *error = message;
    }
}

QString queryError(const QString &operation, const QSqlQuery &query)
{
    return QStringLiteral("%1: %2").arg(operation, query.lastError().text());
}

QString databaseError(const QString &operation, const QSqlDatabase &database)
{
    return QStringLiteral("%1: %2").arg(operation, database.lastError().text());
}

QString toStorageDateTime(const QDateTime &dateTime)
{
    return dateTime.toUTC().toString(Qt::ISODateWithMs);
}

QString nonNullText(const QString &value)
{
    return value.isNull() ? QStringLiteral("") : value;
}

QDateTime fromStorageDateTime(const QVariant &value)
{
    if (value.isNull()) {
        return {};
    }

    const QString text = value.toString();
    QDateTime dateTime = QDateTime::fromString(text, Qt::ISODateWithMs);
    if (!dateTime.isValid()) {
        dateTime = QDateTime::fromString(text, Qt::ISODate);
    }
    return dateTime;
}

QSqlDatabase openedDatabase(const QString &connectionName, QString *error)
{
    if (!QSqlDatabase::contains(connectionName)) {
        setError(error, QStringLiteral("数据库尚未打开。"));
        return {};
    }

    QSqlDatabase database = QSqlDatabase::database(connectionName, false);
    if (!database.isValid() || !database.isOpen()) {
        setError(error, QStringLiteral("数据库连接不可用。"));
        return {};
    }
    return database;
}

bool beginTransaction(QSqlDatabase &database, QString *error)
{
    if (database.transaction()) {
        return true;
    }
    setError(error, databaseError(QStringLiteral("无法开始数据库事务"), database));
    return false;
}

bool commitTransaction(QSqlDatabase &database, QString *error)
{
    if (database.commit()) {
        return true;
    }
    setError(error, databaseError(QStringLiteral("无法提交数据库事务"), database));
    database.rollback();
    return false;
}

bool executeSchemaStatement(QSqlDatabase &database,
                            const QString &statement,
                            QString *error)
{
    QSqlQuery query(database);
    if (query.exec(statement)) {
        return true;
    }
    setError(error, queryError(QStringLiteral("初始化数据库失败"), query));
    return false;
}

NoteRecord noteFromQuery(const QSqlQuery &query)
{
    NoteRecord record;
    record.id = query.value(0).toLongLong();
    record.title = query.value(1).toString();
    record.html = query.value(2).toString();
    record.plainText = query.value(3).toString();
    record.createdAt = fromStorageDateTime(query.value(4));
    record.updatedAt = fromStorageDateTime(query.value(5));
    return record;
}

TodoRecord todoFromQuery(const QSqlQuery &query)
{
    TodoRecord record;
    record.id = query.value(0).toLongLong();
    record.text = query.value(1).toString();
    record.done = query.value(2).toInt() != 0;
    record.dueAt = fromStorageDateTime(query.value(3));
    record.sortOrder = query.value(4).toInt();
    return record;
}

} // namespace

Database::Database()
    : connectionName_(QStringLiteral("notebook-%1")
                          .arg(reinterpret_cast<quintptr>(this), 0, 16)),
      dataDirectory_(QStandardPaths::writableLocation(
          QStandardPaths::AppLocalDataLocation))
{
}

Database::~Database()
{
    if (!QSqlDatabase::contains(connectionName_)) {
        return;
    }

    {
        QSqlDatabase database = QSqlDatabase::database(connectionName_, false);
        if (database.isValid()) {
            database.close();
        }
    }
    QSqlDatabase::removeDatabase(connectionName_);
}

bool Database::open(QString *error)
{
    clearError(error);

    if (dataDirectory_.isEmpty()) {
        setError(error, QStringLiteral("系统未提供可写的应用数据目录。"));
        return false;
    }
    if (!QDir().mkpath(dataDirectory_)) {
        setError(error,
                 QStringLiteral("无法创建应用数据目录：%1").arg(dataDirectory_));
        return false;
    }

    QSqlDatabase database;
    if (QSqlDatabase::contains(connectionName_)) {
        database = QSqlDatabase::database(connectionName_, false);
    } else {
        database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                              connectionName_);
        database.setDatabaseName(
            QDir(dataDirectory_).filePath(QStringLiteral("notebook.sqlite3")));
        database.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=5000"));
    }

    if (!database.isOpen() && !database.open()) {
        setError(error, databaseError(QStringLiteral("无法打开数据库"), database));
        return false;
    }

    QSqlQuery pragma(database);
    if (!pragma.exec(QStringLiteral("PRAGMA busy_timeout = 5000"))) {
        setError(error, queryError(QStringLiteral("设置数据库超时失败"), pragma));
        return false;
    }
    if (!pragma.exec(QStringLiteral("PRAGMA journal_mode = WAL"))) {
        setError(error, queryError(QStringLiteral("启用 WAL 模式失败"), pragma));
        return false;
    }
    if (!pragma.exec(QStringLiteral("PRAGMA synchronous = NORMAL"))) {
        setError(error, queryError(QStringLiteral("设置数据库同步模式失败"), pragma));
        return false;
    }
    if (!pragma.exec(QStringLiteral("PRAGMA foreign_keys = ON"))) {
        setError(error, queryError(QStringLiteral("启用外键约束失败"), pragma));
        return false;
    }

    if (!beginTransaction(database, error)) {
        return false;
    }

    const QStringList schemaStatements = {
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS notes ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "title TEXT NOT NULL DEFAULT '',"
            "html TEXT NOT NULL DEFAULT '',"
            "plain_text TEXT NOT NULL DEFAULT '',"
            "created_at TEXT NOT NULL,"
            "updated_at TEXT NOT NULL,"
            "deleted_at TEXT NULL)"),
        QStringLiteral(
            "CREATE INDEX IF NOT EXISTS idx_notes_active_updated "
            "ON notes(deleted_at, updated_at DESC)"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS todos ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "text TEXT NOT NULL,"
            "done INTEGER NOT NULL DEFAULT 0 CHECK(done IN (0, 1)),"
            "due_at TEXT NULL,"
            "sort_order INTEGER NOT NULL DEFAULT 0)"),
        QStringLiteral(
            "CREATE INDEX IF NOT EXISTS idx_todos_order "
            "ON todos(done, sort_order, id)"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS settings ("
            "key TEXT PRIMARY KEY,"
            "value TEXT NOT NULL DEFAULT '')")
    };

    for (const QString &statement : schemaStatements) {
        if (!executeSchemaStatement(database, statement, error)) {
            database.rollback();
            return false;
        }
    }

    return commitTransaction(database, error);
}

QString Database::dataDirectory() const
{
    return dataDirectory_;
}

QList<NoteRecord> Database::listNotes(const QString &filter,
                                      QString *error) const
{
    clearError(error);
    QList<NoteRecord> records;
    QSqlDatabase database = openedDatabase(connectionName_, error);
    if (!database.isValid()) {
        return records;
    }

    QSqlQuery query(database);
    if (filter.isEmpty()) {
        query.prepare(QStringLiteral(
            "SELECT id, title, html, plain_text, created_at, updated_at "
            "FROM notes WHERE deleted_at IS NULL "
            "ORDER BY updated_at DESC, id DESC"));
    } else {
        query.prepare(QStringLiteral(
            "SELECT id, title, html, plain_text, created_at, updated_at "
            "FROM notes WHERE deleted_at IS NULL "
            "AND (title LIKE :filter OR plain_text LIKE :filter) "
            "ORDER BY updated_at DESC, id DESC"));
        query.bindValue(QStringLiteral(":filter"),
                        QStringLiteral("%") + filter + QStringLiteral("%"));
    }

    if (!query.exec()) {
        setError(error, queryError(QStringLiteral("读取笔记失败"), query));
        return {};
    }
    while (query.next()) {
        records.append(noteFromQuery(query));
    }
    return records;
}

std::optional<NoteRecord> Database::note(qint64 id, QString *error) const
{
    clearError(error);
    QSqlDatabase database = openedDatabase(connectionName_, error);
    if (!database.isValid()) {
        return std::nullopt;
    }

    QSqlQuery query(database);
    query.prepare(QStringLiteral(
        "SELECT id, title, html, plain_text, created_at, updated_at "
        "FROM notes WHERE id = :id AND deleted_at IS NULL"));
    query.bindValue(QStringLiteral(":id"), id);
    if (!query.exec()) {
        setError(error, queryError(QStringLiteral("读取笔记失败"), query));
        return std::nullopt;
    }
    if (!query.next()) {
        return std::nullopt;
    }
    return noteFromQuery(query);
}

qint64 Database::createNote(const QString &title,
                            const QString &html,
                            const QString &plainText,
                            QString *error)
{
    clearError(error);
    QSqlDatabase database = openedDatabase(connectionName_, error);
    if (!database.isValid() || !beginTransaction(database, error)) {
        return 0;
    }

    const QString now = toStorageDateTime(QDateTime::currentDateTimeUtc());
    QSqlQuery query(database);
    query.prepare(QStringLiteral(
        "INSERT INTO notes(title, html, plain_text, created_at, updated_at) "
        "VALUES(:title, :html, :plain_text, :created_at, :updated_at)"));
    query.bindValue(QStringLiteral(":title"), nonNullText(title));
    query.bindValue(QStringLiteral(":html"), nonNullText(html));
    query.bindValue(QStringLiteral(":plain_text"), nonNullText(plainText));
    query.bindValue(QStringLiteral(":created_at"), now);
    query.bindValue(QStringLiteral(":updated_at"), now);

    if (!query.exec()) {
        setError(error, queryError(QStringLiteral("创建笔记失败"), query));
        database.rollback();
        return 0;
    }
    const qint64 id = query.lastInsertId().toLongLong();
    if (id <= 0) {
        setError(error, QStringLiteral("创建笔记失败：数据库未返回有效编号。"));
        database.rollback();
        return 0;
    }
    if (!commitTransaction(database, error)) {
        return 0;
    }
    return id;
}

bool Database::updateNote(qint64 id,
                          const QString &title,
                          const QString &html,
                          const QString &plainText,
                          QString *error)
{
    clearError(error);
    QSqlDatabase database = openedDatabase(connectionName_, error);
    if (!database.isValid() || !beginTransaction(database, error)) {
        return false;
    }

    QSqlQuery query(database);
    query.prepare(QStringLiteral(
        "UPDATE notes SET title = :title, html = :html, "
        "plain_text = :plain_text, updated_at = :updated_at "
        "WHERE id = :id AND deleted_at IS NULL"));
    query.bindValue(QStringLiteral(":title"), nonNullText(title));
    query.bindValue(QStringLiteral(":html"), nonNullText(html));
    query.bindValue(QStringLiteral(":plain_text"), nonNullText(plainText));
    query.bindValue(QStringLiteral(":updated_at"),
                    toStorageDateTime(QDateTime::currentDateTimeUtc()));
    query.bindValue(QStringLiteral(":id"), id);
    if (!query.exec()) {
        setError(error, queryError(QStringLiteral("保存笔记失败"), query));
        database.rollback();
        return false;
    }
    if (query.numRowsAffected() == 0) {
        setError(error, QStringLiteral("保存笔记失败：笔记不存在或已删除。"));
        database.rollback();
        return false;
    }
    return commitTransaction(database, error);
}

bool Database::softDeleteNote(qint64 id, QString *error)
{
    clearError(error);
    QSqlDatabase database = openedDatabase(connectionName_, error);
    if (!database.isValid() || !beginTransaction(database, error)) {
        return false;
    }

    const QString now = toStorageDateTime(QDateTime::currentDateTimeUtc());
    QSqlQuery query(database);
    query.prepare(QStringLiteral(
        "UPDATE notes SET deleted_at = :deleted_at, updated_at = :updated_at "
        "WHERE id = :id AND deleted_at IS NULL"));
    query.bindValue(QStringLiteral(":deleted_at"), now);
    query.bindValue(QStringLiteral(":updated_at"), now);
    query.bindValue(QStringLiteral(":id"), id);
    if (!query.exec()) {
        setError(error, queryError(QStringLiteral("删除笔记失败"), query));
        database.rollback();
        return false;
    }
    if (query.numRowsAffected() == 0) {
        setError(error, QStringLiteral("删除笔记失败：笔记不存在或已删除。"));
        database.rollback();
        return false;
    }
    return commitTransaction(database, error);
}

QList<TodoRecord> Database::listTodos(QString *error) const
{
    clearError(error);
    QList<TodoRecord> records;
    QSqlDatabase database = openedDatabase(connectionName_, error);
    if (!database.isValid()) {
        return records;
    }

    QSqlQuery query(database);
    query.prepare(QStringLiteral(
        "SELECT id, text, done, due_at, sort_order FROM todos "
        "ORDER BY done ASC, sort_order ASC, id ASC"));
    if (!query.exec()) {
        setError(error, queryError(QStringLiteral("读取待办失败"), query));
        return {};
    }
    while (query.next()) {
        records.append(todoFromQuery(query));
    }
    return records;
}

qint64 Database::createTodo(const QString &text, QString *error)
{
    clearError(error);
    QSqlDatabase database = openedDatabase(connectionName_, error);
    if (!database.isValid() || !beginTransaction(database, error)) {
        return 0;
    }

    QSqlQuery orderQuery(database);
    if (!orderQuery.exec(QStringLiteral(
            "SELECT COALESCE(MAX(sort_order), -1) + 1 FROM todos"))
        || !orderQuery.next()) {
        setError(error, queryError(QStringLiteral("计算待办顺序失败"), orderQuery));
        database.rollback();
        return 0;
    }
    const int sortOrder = orderQuery.value(0).toInt();
    orderQuery.finish();

    QSqlQuery query(database);
    query.prepare(QStringLiteral(
        "INSERT INTO todos(text, done, due_at, sort_order) "
        "VALUES(:text, 0, NULL, :sort_order)"));
    query.bindValue(QStringLiteral(":text"), nonNullText(text));
    query.bindValue(QStringLiteral(":sort_order"), sortOrder);
    if (!query.exec()) {
        setError(error, queryError(QStringLiteral("创建待办失败"), query));
        database.rollback();
        return 0;
    }
    const qint64 id = query.lastInsertId().toLongLong();
    if (id <= 0) {
        setError(error, QStringLiteral("创建待办失败：数据库未返回有效编号。"));
        database.rollback();
        return 0;
    }
    if (!commitTransaction(database, error)) {
        return 0;
    }
    return id;
}

bool Database::updateTodoDone(qint64 id, bool done, QString *error)
{
    clearError(error);
    QSqlDatabase database = openedDatabase(connectionName_, error);
    if (!database.isValid() || !beginTransaction(database, error)) {
        return false;
    }

    QSqlQuery query(database);
    query.prepare(QStringLiteral(
        "UPDATE todos SET done = :done WHERE id = :id"));
    query.bindValue(QStringLiteral(":done"), done ? 1 : 0);
    query.bindValue(QStringLiteral(":id"), id);
    if (!query.exec()) {
        setError(error, queryError(QStringLiteral("更新待办失败"), query));
        database.rollback();
        return false;
    }
    if (query.numRowsAffected() == 0) {
        setError(error, QStringLiteral("更新待办失败：待办不存在。"));
        database.rollback();
        return false;
    }
    return commitTransaction(database, error);
}

bool Database::deleteCompletedTodos(QString *error)
{
    clearError(error);
    QSqlDatabase database = openedDatabase(connectionName_, error);
    if (!database.isValid() || !beginTransaction(database, error)) {
        return false;
    }

    QSqlQuery query(database);
    query.prepare(QStringLiteral("DELETE FROM todos WHERE done = :done"));
    query.bindValue(QStringLiteral(":done"), 1);
    if (!query.exec()) {
        setError(error, queryError(QStringLiteral("清理已完成待办失败"), query));
        database.rollback();
        return false;
    }
    return commitTransaction(database, error);
}

QString Database::quickNote(QString *error) const
{
    clearError(error);
    QSqlDatabase database = openedDatabase(connectionName_, error);
    if (!database.isValid()) {
        return {};
    }

    QSqlQuery query(database);
    query.prepare(QStringLiteral(
        "SELECT value FROM settings WHERE key = :key"));
    query.bindValue(QStringLiteral(":key"), QStringLiteral("quick_note"));
    if (!query.exec()) {
        setError(error, queryError(QStringLiteral("读取快捷便签失败"), query));
        return {};
    }
    return query.next() ? query.value(0).toString() : QString();
}

bool Database::saveQuickNote(const QString &text, QString *error)
{
    clearError(error);
    QSqlDatabase database = openedDatabase(connectionName_, error);
    if (!database.isValid() || !beginTransaction(database, error)) {
        return false;
    }

    QSqlQuery query(database);
    query.prepare(QStringLiteral(
        "INSERT INTO settings(key, value) VALUES(:key, :value) "
        "ON CONFLICT(key) DO UPDATE SET value = excluded.value"));
    query.bindValue(QStringLiteral(":key"), QStringLiteral("quick_note"));
    query.bindValue(QStringLiteral(":value"), nonNullText(text));
    if (!query.exec()) {
        setError(error, queryError(QStringLiteral("保存快捷便签失败"), query));
        database.rollback();
        return false;
    }
    return commitTransaction(database, error);
}
