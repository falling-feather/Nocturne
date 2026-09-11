#include "Database.h"
#include "WorkspaceStore.h"

#include <QCryptographicHash>
#include <QDir>
#include <QMetaType>
#include <QRegularExpression>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QStringList>
#include <QVariant>
#include <QVector>
#include <QCoreApplication>

#include <algorithm>

namespace {

constexpr int kSchemaVersion = 7;
constexpr int kExcerptLength = 180;

void clearError(QString *error)
{
    if (error)
        error->clear();
}

void setError(QString *error, const QString &message)
{
    if (error)
        *error = message;
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

QVariant nullableId(qint64 id)
{
    if (id > 0)
        return QVariant::fromValue(id);
    return QVariant(QMetaType::fromType<qint64>());
}

QDateTime fromStorageDateTime(const QVariant &value)
{
    if (value.isNull())
        return {};

    const QString text = value.toString();
    QDateTime dateTime = QDateTime::fromString(text, Qt::ISODateWithMs);
    if (!dateTime.isValid())
        dateTime = QDateTime::fromString(text, Qt::ISODate);
    return dateTime;
}

QByteArray contentHash(const QString &html)
{
    return QCryptographicHash::hash(html.toUtf8(), QCryptographicHash::Sha256);
}

QString noteExcerpt(const QString &plainText)
{
    QString excerpt = plainText;
    excerpt.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
    return excerpt.trimmed().left(kExcerptLength);
}

QString normalizedKind(const QString &kind)
{
    return kind == QStringLiteral("sticky") ? QStringLiteral("sticky")
                                             : QStringLiteral("note");
}

bool syncTodoLinks(QSqlDatabase& database, qint64 noteId, const QString& html, QString* error)
{
    QSqlQuery links(database);
    links.prepare(QStringLiteral("SELECT todo_id, anchor FROM todo_links WHERE note_id = ?"));
    links.addBindValue(noteId);
    if (!links.exec()) { setError(error, links.lastError().text()); return false; }
    QList<QPair<qint64, bool>> states;
    while (links.next()) states.append({links.value(0).toLongLong(),
        html.contains(QStringLiteral("nocturne-todo:") + links.value(1).toString())});
    links.finish();
    QSqlQuery update(database);
    update.prepare(QStringLiteral("UPDATE todo_links SET active = ? WHERE todo_id = ?"));
    for (const auto& state : states) {
        update.bindValue(0, state.second ? 1 : 0); update.bindValue(1, state.first);
        if (!update.exec()) { setError(error, update.lastError().text()); return false; }
    }
    return true;
}

QString stickyTitle(const QString &text)
{
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const QString candidate = line.simplified();
        if (!candidate.isEmpty())
            return candidate.left(42);
    }
    return QStringLiteral("快速便签");
}

QString stickyHtml(const QString &text)
{
    QString escaped = text;
    escaped.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    escaped.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    escaped = escaped.toHtmlEscaped();
    escaped.replace(QLatin1Char('\n'), QStringLiteral("<br>"));
    return QStringLiteral("<p>%1</p>").arg(escaped);
}

QString likePattern(const QString &filter)
{
    QString escaped = filter;
    escaped.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    escaped.replace(QLatin1Char('%'), QStringLiteral("\\%"));
    escaped.replace(QLatin1Char('_'), QStringLiteral("\\_"));
    return QStringLiteral("%") + escaped + QStringLiteral("%");
}

QString ftsPhrase(QString filter)
{
    filter.replace(QLatin1Char('"'), QStringLiteral("\"\""));
    return QStringLiteral("\"") + filter + QStringLiteral("\"");
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
    if (database.transaction())
        return true;
    setError(error, databaseError(QStringLiteral("无法开始数据库事务"), database));
    return false;
}

bool commitTransaction(QSqlDatabase &database, QString *error)
{
    if (database.commit())
        return true;
    setError(error, databaseError(QStringLiteral("无法提交数据库事务"), database));
    database.rollback();
    return false;
}

bool executeSchemaStatement(QSqlDatabase &database,
                            const QString &statement,
                            QString *error)
{
    QSqlQuery query(database);
    if (query.exec(statement))
        return true;
    setError(error, queryError(QStringLiteral("初始化数据库失败"), query));
    return false;
}

bool tableHasColumn(QSqlDatabase &database,
                    const QString &table,
                    const QString &column,
                    QString *error)
{
    QSqlQuery query(database);
    if (!query.exec(QStringLiteral("PRAGMA table_info(%1)").arg(table))) {
        setError(error, queryError(QStringLiteral("检查数据库字段失败"), query));
        return false;
    }
    while (query.next()) {
        if (query.value(1).toString() == column)
            return true;
    }
    return false;
}

bool ensureColumn(QSqlDatabase &database,
                  const QString &table,
                  const QString &column,
                  const QString &definition,
                  QString *error)
{
    QString probeError;
    if (tableHasColumn(database, table, column, &probeError))
        return true;
    if (!probeError.isEmpty()) {
        setError(error, probeError);
        return false;
    }
    return executeSchemaStatement(
        database,
        QStringLiteral("ALTER TABLE %1 ADD COLUMN %2 %3").arg(table, column, definition),
        error);
}

NoteSummary summaryFromQuery(const QSqlQuery &query)
{
    NoteSummary record;
    record.id = query.value(0).toLongLong();
    record.title = query.value(1).toString();
    record.excerpt = query.value(2).toString();
    record.folderId = query.value(3).isNull() ? 0 : query.value(3).toLongLong();
    record.kind = query.value(4).toString();
    record.bodyRevision = query.value(5).toInt();
    record.contentHash = query.value(6).toByteArray();
    record.createdAt = fromStorageDateTime(query.value(7));
    record.updatedAt = fromStorageDateTime(query.value(8));
    return record;
}

NoteRecord noteFromQuery(const QSqlQuery &query)
{
    NoteRecord record;
    record.id = query.value(0).toLongLong();
    record.title = query.value(1).toString();
    record.html = query.value(2).toString();
    record.plainText = query.value(3).toString();
    record.excerpt = query.value(4).toString();
    record.folderId = query.value(5).isNull() ? 0 : query.value(5).toLongLong();
    record.kind = query.value(6).toString();
    record.bodyRevision = query.value(7).toInt();
    record.contentHash = query.value(8).toByteArray();
    record.createdAt = fromStorageDateTime(query.value(9));
    record.updatedAt = fromStorageDateTime(query.value(10));
    return record;
}

FolderRecord folderFromQuery(const QSqlQuery &query)
{
    FolderRecord record;
    record.id = query.value(0).toLongLong();
    record.parentId = query.value(1).isNull() ? 0 : query.value(1).toLongLong();
    record.name = query.value(2).toString();
    record.sortOrder = query.value(3).toInt();
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
      dataDirectory_(qApp && !qApp->property("nocturneDataDirectory").toString().isEmpty()
          ? qApp->property("nocturneDataDirectory").toString()
          : QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
{
}

Database::~Database()
{
    if (!QSqlDatabase::contains(connectionName_))
        return;

    {
        QSqlDatabase database = QSqlDatabase::database(connectionName_, false);
        if (database.isValid())
            database.close();
    }
    QSqlDatabase::removeDatabase(connectionName_);
}

bool Database::open(QString *error)
{
    clearError(error);
    ftsEnabled_ = false;

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

    int previousSchemaVersion = 0;
    if (pragma.exec(QStringLiteral("PRAGMA user_version")) && pragma.next())
        previousSchemaVersion = pragma.value(0).toInt();
    pragma.finish();

    if (previousSchemaVersion > kSchemaVersion) {
        setError(error,
                 QStringLiteral("数据库版本 %1 高于当前程序支持的版本 %2，请升级夜航。")
                     .arg(previousSchemaVersion)
                     .arg(kSchemaVersion));
        return false;
    }

    if (previousSchemaVersion < kSchemaVersion) {
        if (!beginTransaction(database, error))
            return false;

        const QStringList baseSchemaStatements = {
            QStringLiteral(
                "CREATE TABLE IF NOT EXISTS folders ("
                "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                "parent_id INTEGER NULL REFERENCES folders(id) ON DELETE SET NULL,"
                "name TEXT NOT NULL,"
                "sort_order INTEGER NOT NULL DEFAULT 0,"
                "created_at TEXT NOT NULL,"
                "updated_at TEXT NOT NULL)"),
            QStringLiteral(
                "CREATE TABLE IF NOT EXISTS notes ("
                "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                "folder_id INTEGER NULL REFERENCES folders(id) ON DELETE SET NULL,"
                "kind TEXT NOT NULL DEFAULT 'note' CHECK(kind IN ('note', 'sticky')),"
                "title TEXT NOT NULL DEFAULT '',"
                "html TEXT NOT NULL DEFAULT '',"
                "plain_text TEXT NOT NULL DEFAULT '',"
                "excerpt TEXT NOT NULL DEFAULT '',"
                "body_revision INTEGER NOT NULL DEFAULT 1,"
                "content_hash BLOB NOT NULL DEFAULT X'',"
                "created_at TEXT NOT NULL,"
                "updated_at TEXT NOT NULL,"
                "deleted_at TEXT NULL)"),
            QStringLiteral(
                "CREATE TABLE IF NOT EXISTS todos ("
                "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                "text TEXT NOT NULL,"
                "done INTEGER NOT NULL DEFAULT 0 CHECK(done IN (0, 1)),"
                "due_at TEXT NULL,"
                "sort_order INTEGER NOT NULL DEFAULT 0)"),
            QStringLiteral(
                "CREATE TABLE IF NOT EXISTS settings ("
                "key TEXT PRIMARY KEY,"
                "value TEXT NOT NULL DEFAULT '')"),
            QStringLiteral(
                "CREATE TABLE IF NOT EXISTS note_sources ("
                "note_id INTEGER NOT NULL REFERENCES notes(id) ON DELETE CASCADE,"
                "source_note_id INTEGER NOT NULL,"
                "source_kind TEXT NOT NULL,"
                "source_updated_at TEXT NOT NULL,"
                "position INTEGER NOT NULL DEFAULT 0,"
                "PRIMARY KEY(note_id, position),"
                "UNIQUE(note_id, source_note_id))")
        };

        for (const QString &statement : baseSchemaStatements) {
            if (!executeSchemaStatement(database, statement, error)) {
                database.rollback();
                return false;
            }
        }

        for (const QString& statement : {
            QStringLiteral("CREATE TABLE IF NOT EXISTS todo_links (todo_id INTEGER PRIMARY KEY REFERENCES todos(id) ON DELETE CASCADE, note_id INTEGER NOT NULL REFERENCES notes(id) ON DELETE CASCADE, anchor TEXT NOT NULL UNIQUE, active INTEGER NOT NULL DEFAULT 1)"),
            QStringLiteral("CREATE INDEX IF NOT EXISTS idx_todo_links_note ON todo_links(note_id)"),
            QStringLiteral("CREATE TABLE IF NOT EXISTS import_folders (source_path TEXT COLLATE NOCASE PRIMARY KEY, folder_id INTEGER NOT NULL REFERENCES folders(id) ON DELETE CASCADE)"),
            QStringLiteral("CREATE TABLE IF NOT EXISTS import_sources (source_path TEXT COLLATE NOCASE NOT NULL, source_hash BLOB NOT NULL, note_id INTEGER NOT NULL UNIQUE REFERENCES notes(id) ON DELETE CASCADE, PRIMARY KEY(source_path, source_hash))"),
            QStringLiteral("CREATE TABLE IF NOT EXISTS linked_folders (source_path TEXT COLLATE NOCASE PRIMARY KEY, folder_id INTEGER NOT NULL REFERENCES folders(id) ON DELETE CASCADE)"),
            QStringLiteral("CREATE TABLE IF NOT EXISTS linked_sources (source_path TEXT COLLATE NOCASE PRIMARY KEY, source_kind TEXT NOT NULL, source_hash BLOB NOT NULL, note_id INTEGER NOT NULL UNIQUE REFERENCES notes(id) ON DELETE CASCADE, linked_at TEXT NOT NULL)")}) {
            if (!executeSchemaStatement(database, statement, error)) { database.rollback(); return false; }
        }

        const struct {
            const char *name;
            const char *definition;
        } noteColumns[] = {
            {"folder_id", "INTEGER NULL REFERENCES folders(id) ON DELETE SET NULL"},
            {"kind", "TEXT NOT NULL DEFAULT 'note'"},
            {"excerpt", "TEXT NOT NULL DEFAULT ''"},
            {"body_revision", "INTEGER NOT NULL DEFAULT 1"},
            {"content_hash", "BLOB NOT NULL DEFAULT X''"}
        };

        for (const auto &column : noteColumns) {
            if (!ensureColumn(database,
                              QStringLiteral("notes"),
                              QString::fromLatin1(column.name),
                              QString::fromLatin1(column.definition),
                              error)) {
                database.rollback();
                return false;
            }
        }

        const QStringList indexStatements = {
            QStringLiteral(
                "CREATE INDEX IF NOT EXISTS idx_notes_active_updated "
                "ON notes(deleted_at, updated_at DESC, id DESC)"),
            QStringLiteral(
                "CREATE INDEX IF NOT EXISTS idx_notes_folder_active_updated "
                "ON notes(folder_id, deleted_at, updated_at DESC, id DESC)"),
            QStringLiteral(
                "CREATE UNIQUE INDEX IF NOT EXISTS idx_folders_parent_name "
                "ON folders(COALESCE(parent_id, 0), name COLLATE NOCASE)"),
            QStringLiteral(
                "CREATE INDEX IF NOT EXISTS idx_folders_order "
                "ON folders(parent_id, sort_order, name COLLATE NOCASE)"),
            QStringLiteral(
                "CREATE INDEX IF NOT EXISTS idx_todos_order "
                "ON todos(done, sort_order, id)"),
            QStringLiteral(
                "CREATE INDEX IF NOT EXISTS idx_note_sources_source "
                "ON note_sources(source_note_id, note_id)")
        };

        for (const QString &statement : indexStatements) {
            if (!executeSchemaStatement(database, statement, error)) {
                database.rollback();
                return false;
            }
        }

        // schema v2 limited the database to one active sticky. Multiple desktop
        // sticky windows share the same notes table in v3, so remove that guard.
        if (!executeSchemaStatement(database,
                                    QStringLiteral("DROP INDEX IF EXISTS idx_notes_single_sticky"),
                                    error)) {
            database.rollback();
            return false;
        }

        QSqlQuery normalize(database);
        if (!normalize.exec(QStringLiteral(
                "UPDATE notes SET "
                "kind = CASE WHEN kind = 'sticky' THEN 'sticky' ELSE 'note' END, "
                "body_revision = CASE WHEN body_revision < 1 THEN 1 ELSE body_revision END, "
                "excerpt = CASE WHEN excerpt = '' THEN "
                "substr(trim(replace(replace(plain_text, char(13), ' '), char(10), ' ')), 1, 180) "
                "ELSE excerpt END"))) {
            setError(error, queryError(QStringLiteral("迁移笔记摘要失败"), normalize));
            database.rollback();
            return false;
        }

        QVector<QPair<qint64, QByteArray>> missingHashes;
        QSqlQuery hashScan(database);
        if (!hashScan.exec(QStringLiteral(
                "SELECT id, html FROM notes WHERE length(content_hash) = 0"))) {
            setError(error, queryError(QStringLiteral("读取待迁移笔记失败"), hashScan));
            database.rollback();
            return false;
        }
        while (hashScan.next())
            missingHashes.append({hashScan.value(0).toLongLong(),
                                  contentHash(hashScan.value(1).toString())});
        hashScan.finish();

        QSqlQuery hashUpdate(database);
        hashUpdate.prepare(QStringLiteral(
            "UPDATE notes SET content_hash = :content_hash WHERE id = :id"));
        for (const auto &entry : missingHashes) {
            hashUpdate.bindValue(QStringLiteral(":content_hash"), entry.second);
            hashUpdate.bindValue(QStringLiteral(":id"), entry.first);
            if (!hashUpdate.exec()) {
                setError(error, queryError(QStringLiteral("迁移笔记哈希失败"), hashUpdate));
                database.rollback();
                return false;
            }
            hashUpdate.finish();
        }

        QString legacyQuickNote;
        bool legacyQuickNoteExists = false;
        QSqlQuery legacyQuery(database);
        legacyQuery.prepare(QStringLiteral(
            "SELECT value FROM settings WHERE key = 'quick_note'"));
        if (!legacyQuery.exec()) {
            setError(error, queryError(QStringLiteral("读取旧快捷便签失败"), legacyQuery));
            database.rollback();
            return false;
        }
        if (legacyQuery.next()) {
            legacyQuickNoteExists = true;
            legacyQuickNote = legacyQuery.value(0).toString();
        }
        legacyQuery.finish();

        if (legacyQuickNoteExists && !legacyQuickNote.trimmed().isEmpty()) {
            QSqlQuery existingSticky(database);
            if (!existingSticky.exec(QStringLiteral(
                    "SELECT id FROM notes WHERE kind = 'sticky' "
                    "AND deleted_at IS NULL LIMIT 1"))) {
                setError(error, queryError(QStringLiteral("检查便签迁移状态失败"), existingSticky));
                database.rollback();
                return false;
            }
            const bool hasSticky = existingSticky.next();
            existingSticky.finish();
            if (!hasSticky) {
                const QString html = stickyHtml(legacyQuickNote);
                const QString now = toStorageDateTime(QDateTime::currentDateTimeUtc());
                QSqlQuery insertSticky(database);
                insertSticky.prepare(QStringLiteral(
                    "INSERT INTO notes(folder_id, kind, title, html, plain_text, excerpt, "
                    "body_revision, content_hash, created_at, updated_at) "
                    "VALUES(NULL, 'sticky', :title, :html, :plain_text, :excerpt, 1, "
                    ":content_hash, :created_at, :updated_at)"));
                insertSticky.bindValue(QStringLiteral(":title"), stickyTitle(legacyQuickNote));
                insertSticky.bindValue(QStringLiteral(":html"), html);
                insertSticky.bindValue(QStringLiteral(":plain_text"), legacyQuickNote);
                insertSticky.bindValue(QStringLiteral(":excerpt"), noteExcerpt(legacyQuickNote));
                insertSticky.bindValue(QStringLiteral(":content_hash"), contentHash(html));
                insertSticky.bindValue(QStringLiteral(":created_at"), now);
                insertSticky.bindValue(QStringLiteral(":updated_at"), now);
                if (!insertSticky.exec()) {
                    setError(error, queryError(QStringLiteral("迁移快捷便签失败"), insertSticky));
                    database.rollback();
                    return false;
                }
            }
        }

        if (legacyQuickNoteExists) {
            QSqlQuery removeLegacy(database);
            if (!removeLegacy.exec(QStringLiteral(
                    "DELETE FROM settings WHERE key = 'quick_note'"))) {
                setError(error, queryError(QStringLiteral("清理旧快捷便签设置失败"), removeLegacy));
                database.rollback();
                return false;
            }
        }

        if (!WorkspaceStore::initialize(database, error)) { database.rollback(); return false; }
        if (!executeSchemaStatement(database,
                                    QStringLiteral("PRAGMA user_version = %1")
                                        .arg(kSchemaVersion),
                                    error)) {
            database.rollback();
            return false;
        }
        if (!commitTransaction(database, error))
            return false;
    }

    QSqlQuery ftsProbe(database);
    bool ftsTableExisted = false;
    ftsProbe.prepare(QStringLiteral(
        "SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'notes_fts'"));
    if (ftsProbe.exec())
        ftsTableExisted = ftsProbe.next();

    QSqlQuery fts(database);
    if (fts.exec(QStringLiteral(
            "CREATE VIRTUAL TABLE IF NOT EXISTS notes_fts USING fts5("
            "title, plain_text, content='notes', content_rowid='id', tokenize='trigram')"))) {
        QSqlQuery legacyUpdateTrigger(database);
        bool triggersReady = legacyUpdateTrigger.exec(QStringLiteral(
            "DROP TRIGGER IF EXISTS notes_fts_au"));
        const QStringList triggerStatements = {
            QStringLiteral(
                "CREATE TRIGGER IF NOT EXISTS notes_fts_ai AFTER INSERT ON notes "
                "WHEN new.deleted_at IS NULL BEGIN "
                "INSERT INTO notes_fts(rowid, title, plain_text) "
                "VALUES(new.id, new.title, new.plain_text); END"),
            QStringLiteral(
                "CREATE TRIGGER IF NOT EXISTS notes_fts_ad AFTER DELETE ON notes "
                "WHEN old.deleted_at IS NULL BEGIN "
                "INSERT INTO notes_fts(notes_fts, rowid, title, plain_text) "
                "VALUES('delete', old.id, old.title, old.plain_text); END"),
            QStringLiteral(
                "CREATE TRIGGER IF NOT EXISTS notes_fts_au_v2 "
                "AFTER UPDATE OF title, plain_text, deleted_at ON notes BEGIN "
                "INSERT INTO notes_fts(notes_fts, rowid, title, plain_text) "
                "SELECT 'delete', old.id, old.title, old.plain_text "
                "WHERE old.deleted_at IS NULL; "
                "INSERT INTO notes_fts(rowid, title, plain_text) "
                "SELECT new.id, new.title, new.plain_text "
                "WHERE new.deleted_at IS NULL; END")
        };

        for (const QString &statement : triggerStatements) {
            if (!triggersReady)
                break;
            QSqlQuery trigger(database);
            if (!trigger.exec(statement)) {
                triggersReady = false;
                break;
            }
        }

        if (triggersReady && (!ftsTableExisted || previousSchemaVersion < kSchemaVersion)) {
            QSqlQuery rebuild(database);
            triggersReady = rebuild.exec(QStringLiteral(
                "INSERT INTO notes_fts(notes_fts) VALUES('rebuild')"));
        }
        ftsEnabled_ = triggersReady;
    }

    return true;
}

QString Database::dataDirectory() const
{
    return dataDirectory_;
}

QList<NoteSummary> Database::listNoteSummaries(const QString &filter,
                                               QString *error,
                                               qint64 folderFilter) const
{
    clearError(error);
    QList<NoteSummary> records;
    QSqlDatabase database = openedDatabase(connectionName_, error);
    if (!database.isValid())
        return records;

    QString sql = QStringLiteral(
        "SELECT n.id, n.title, n.excerpt, n.folder_id, n.kind, "
        "n.body_revision, n.content_hash, n.created_at, n.updated_at "
        "FROM notes n WHERE n.deleted_at IS NULL ");

    if (folderFilter == UnfiledFolder)
        sql += QStringLiteral("AND n.folder_id IS NULL ");
    else if (folderFilter > 0)
        sql += QStringLiteral("AND n.folder_id IN (WITH RECURSIVE tree(id) AS (SELECT :folder_id UNION SELECT f.id FROM folders f JOIN tree t ON f.parent_id=t.id) SELECT id FROM tree) ");

    const QString trimmedFilter = filter.trimmed();
    if (!trimmedFilter.isEmpty()) {
        if (ftsEnabled_ && trimmedFilter.size() >= 3) {
            sql += QStringLiteral(
                "AND n.id IN (SELECT rowid FROM notes_fts "
                "WHERE notes_fts MATCH :fts_query) ");
        } else {
            sql += QStringLiteral(
                "AND (n.title LIKE :filter ESCAPE '\\' "
                "OR n.plain_text LIKE :filter ESCAPE '\\') ");
        }
    }
    sql += QStringLiteral("ORDER BY n.updated_at DESC, n.id DESC");

    QSqlQuery query(database);
    query.prepare(sql);
    if (folderFilter > 0)
        query.bindValue(QStringLiteral(":folder_id"), folderFilter);
    if (!trimmedFilter.isEmpty()) {
        if (ftsEnabled_ && trimmedFilter.size() >= 3)
            query.bindValue(QStringLiteral(":fts_query"), ftsPhrase(trimmedFilter));
        else
            query.bindValue(QStringLiteral(":filter"), likePattern(trimmedFilter));
    }

    if (!query.exec()) {
        setError(error, queryError(QStringLiteral("读取笔记摘要失败"), query));
        return {};
    }
    while (query.next())
        records.append(summaryFromQuery(query));
    return records;
}

std::optional<NoteRecord> Database::note(qint64 id, QString *error) const
{
    clearError(error);
    QSqlDatabase database = openedDatabase(connectionName_, error);
    if (!database.isValid())
        return std::nullopt;

    QSqlQuery query(database);
    query.prepare(QStringLiteral(
        "SELECT id, title, html, plain_text, excerpt, folder_id, kind, "
        "body_revision, content_hash, created_at, updated_at "
        "FROM notes WHERE id = :id AND deleted_at IS NULL"));
    query.bindValue(QStringLiteral(":id"), id);
    if (!query.exec()) {
        setError(error, queryError(QStringLiteral("读取笔记失败"), query));
        return std::nullopt;
    }
    if (!query.next())
        return std::nullopt;
    return noteFromQuery(query);
}

qint64 Database::createNote(const QString &title,
                            const QString &html,
                            const QString &plainText,
                            QString *error,
                            qint64 folderId,
                            const QString &kind)
{
    clearError(error);
    QSqlDatabase database = openedDatabase(connectionName_, error);
    if (!database.isValid() || !beginTransaction(database, error))
        return 0;

    const QString safeHtml = nonNullText(html);
    const QString safePlainText = nonNullText(plainText);
    const QString now = toStorageDateTime(QDateTime::currentDateTimeUtc());
    QSqlQuery query(database);
    query.prepare(QStringLiteral(
        "INSERT INTO notes(folder_id, kind, title, html, plain_text, excerpt, "
        "body_revision, content_hash, created_at, updated_at) "
        "VALUES(:folder_id, :kind, :title, :html, :plain_text, :excerpt, 1, "
        ":content_hash, :created_at, :updated_at)"));
    query.bindValue(QStringLiteral(":folder_id"), nullableId(folderId));
    query.bindValue(QStringLiteral(":kind"), normalizedKind(kind));
    query.bindValue(QStringLiteral(":title"), nonNullText(title));
    query.bindValue(QStringLiteral(":html"), safeHtml);
    query.bindValue(QStringLiteral(":plain_text"), safePlainText);
    query.bindValue(QStringLiteral(":excerpt"), noteExcerpt(safePlainText));
    query.bindValue(QStringLiteral(":content_hash"), contentHash(safeHtml));
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
    if (!commitTransaction(database, error))
        return 0;
    return id;
}

bool Database::updateNote(qint64 id,
                          const QString &title,
                          const QString &html,
                          const QString &plainText,
                          QString *error, const QString& reason,
                          const QByteArray& expectedHash, const QByteArray* sourceBytes,
                          const QString& newSourcePath, const QString& newSourceKind)
{
    clearError(error);
    QSqlDatabase database = openedDatabase(connectionName_, error);
    if (!database.isValid() || !beginTransaction(database, error))
        return false;

    WorkspaceStore store(*this);
    if (!store.capture(id, QStringLiteral("保存前"), error)) { database.rollback(); return false; }
    const QString safeHtml = nonNullText(html);
    const QString safePlainText = nonNullText(plainText);
    const QByteArray hash = contentHash(safeHtml);
    QSqlQuery query(database);
    query.prepare(QStringLiteral(
        "UPDATE notes SET title = :title, html = :html, "
        "plain_text = :plain_text, excerpt = :excerpt, "
        "body_revision = CASE WHEN content_hash = :old_hash "
        "THEN body_revision ELSE body_revision + 1 END, "
        "content_hash = :content_hash, updated_at = :updated_at "
        "WHERE id = :id AND deleted_at IS NULL")
        + (expectedHash.isEmpty() ? QString() : QStringLiteral(" AND content_hash = :expected_hash")));
    if (!expectedHash.isEmpty()) query.bindValue(QStringLiteral(":expected_hash"), expectedHash);
    query.bindValue(QStringLiteral(":title"), nonNullText(title));
    query.bindValue(QStringLiteral(":html"), safeHtml);
    query.bindValue(QStringLiteral(":plain_text"), safePlainText);
    query.bindValue(QStringLiteral(":excerpt"), noteExcerpt(safePlainText));
    query.bindValue(QStringLiteral(":old_hash"), hash);
    query.bindValue(QStringLiteral(":content_hash"), hash);
    query.bindValue(QStringLiteral(":updated_at"),
                    toStorageDateTime(QDateTime::currentDateTimeUtc()));
    query.bindValue(QStringLiteral(":id"), id);
    if (!query.exec()) {
        setError(error, queryError(QStringLiteral("保存笔记失败"), query));
        database.rollback();
        return false;
    }
    if (query.numRowsAffected() == 0) {
        setError(error, QStringLiteral("保存笔记失败：笔记已变化、被删除或不存在。请比较当前版本。"));
        database.rollback();
        return false;
    }
    if (!syncTodoLinks(database, id, safeHtml, error)) { database.rollback(); return false; }
    if (sourceBytes) {
        QSqlQuery source(database); source.prepare(newSourcePath.isEmpty()?QStringLiteral("UPDATE linked_sources SET source_hash=?,linked_at=? WHERE note_id=?")
            :QStringLiteral("UPDATE linked_sources SET source_hash=?,linked_at=?,source_path=?,source_kind=? WHERE note_id=?"));
        source.addBindValue(QCryptographicHash::hash(*sourceBytes, QCryptographicHash::Sha256));
        source.addBindValue(toStorageDateTime(QDateTime::currentDateTimeUtc()));
        if(!newSourcePath.isEmpty()){source.addBindValue(newSourcePath);source.addBindValue(newSourceKind);}
        source.addBindValue(id);
        if (!store.cacheSource(id, *sourceBytes, error) || !source.exec() || source.numRowsAffected()!=1) {
            if (error && error->isEmpty()) *error = source.lastError().text(); database.rollback(); return false;
        }
    }
    if (!store.capture(id, reason, error)) { database.rollback(); return false; }
    return commitTransaction(database, error);
}

bool Database::renameNote(qint64 id,
                          const QString &title,
                          QString *error)
{
    clearError(error);
    QSqlDatabase database = openedDatabase(connectionName_, error);
    if (!database.isValid() || !beginTransaction(database, error))
        return false;

    if(!WorkspaceStore(*this).capture(id,QStringLiteral("重命名前"),error)){database.rollback();return false;}
    QSqlQuery query(database);
    query.prepare(QStringLiteral(
        "UPDATE notes SET title = :title, updated_at = :updated_at "
        "WHERE id = :id AND deleted_at IS NULL"));
    query.bindValue(QStringLiteral(":title"), nonNullText(title));
    query.bindValue(QStringLiteral(":updated_at"),
                    toStorageDateTime(QDateTime::currentDateTimeUtc()));
    query.bindValue(QStringLiteral(":id"), id);
    if (!query.exec()) {
        setError(error, queryError(QStringLiteral("重命名笔记失败"), query));
        database.rollback();
        return false;
    }
    if (query.numRowsAffected() == 0) {
        setError(error, QStringLiteral("重命名笔记失败：笔记不存在或已删除。"));
        database.rollback();
        return false;
    }
    if(!WorkspaceStore(*this).capture(id,QStringLiteral("重命名"),error)){database.rollback();return false;}
    return commitTransaction(database, error);
}

bool Database::moveNoteToFolder(qint64 id, qint64 folderId, QString *error)
{
    clearError(error);
    QSqlDatabase database = openedDatabase(connectionName_, error);
    if (!database.isValid() || !beginTransaction(database, error))
        return false;

    QSqlQuery query(database);
    query.prepare(QStringLiteral(
        "UPDATE notes SET folder_id = :folder_id, updated_at = :updated_at "
        "WHERE id = :id AND deleted_at IS NULL"));
    query.bindValue(QStringLiteral(":folder_id"), nullableId(folderId));
    query.bindValue(QStringLiteral(":updated_at"),
                    toStorageDateTime(QDateTime::currentDateTimeUtc()));
    query.bindValue(QStringLiteral(":id"), id);
    if (!query.exec()) {
        setError(error, queryError(QStringLiteral("移动笔记失败"), query));
        database.rollback();
        return false;
    }
    if (query.numRowsAffected() == 0) {
        setError(error, QStringLiteral("移动笔记失败：笔记或分组不存在。"));
        database.rollback();
        return false;
    }
    return commitTransaction(database, error);
}

bool Database::softDeleteNote(qint64 id, QString *error)
{
    clearError(error);
    QSqlDatabase database = openedDatabase(connectionName_, error);
    if (!database.isValid() || !beginTransaction(database, error))
        return false;

    if (!WorkspaceStore(*this).capture(id, QStringLiteral("移入回收站前"), error)) { database.rollback(); return false; }
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

qint64 Database::collectStickyNotes(const QList<qint64> &stickyIds,
                                    const QString &title,
                                    qint64 folderId,
                                    QString *error)
{
    clearError(error);
    if (stickyIds.isEmpty()) {
        setError(error, QStringLiteral("请至少选择一枚便签。"));
        return 0;
    }

    QList<NoteRecord> sources;
    QSet<qint64> seenIds;
    sources.reserve(stickyIds.size());
    for (const qint64 id : stickyIds) {
        if (id <= 0 || seenIds.contains(id))
            continue;
        const std::optional<NoteRecord> source = note(id, error);
        if (!source.has_value()) {
            if (error && error->isEmpty())
                *error = QStringLiteral("无法读取编号为 %1 的便签。").arg(id);
            return 0;
        }
        if (source->kind != QStringLiteral("sticky")) {
            setError(error,
                     QStringLiteral("编号为 %1 的记录不是桌面便签。").arg(id));
            return 0;
        }
        seenIds.insert(id);
        sources.append(*source);
    }
    if (sources.isEmpty()) {
        setError(error, QStringLiteral("没有可入册的有效便签。"));
        return 0;
    }

    const QString safeTitle = title.simplified().isEmpty()
        ? QStringLiteral("夜航拾遗 · %1")
              .arg(QDate::currentDate().toString(QStringLiteral("yyyy-MM-dd")))
        : title.simplified();
    const QDateTime collectedDateTime = QDateTime::currentDateTime();
    const QString collectedAt = collectedDateTime.toString(QStringLiteral("yyyy-MM-dd HH:mm"));
    QString html = QStringLiteral(
        "<p><em>由夜航“收舟入册”汇集 %1 枚桌面便签 · %2</em></p><hr>")
                       .arg(sources.size())
                       .arg(collectedAt.toHtmlEscaped());
    QString plainText = QStringLiteral("由夜航“收舟入册”汇集 %1 枚桌面便签 · %2")
                            .arg(sources.size())
                            .arg(collectedAt);

    for (int index = 0; index < sources.size(); ++index) {
        const NoteRecord &source = sources.at(index);
        const QString sourceTitle = source.title.simplified().isEmpty()
            ? QStringLiteral("便签 %1").arg(index + 1)
            : source.title.simplified();
        const QString updatedIso = source.updatedAt.toUTC().toString(Qt::ISODateWithMs);
        const QString updatedDisplay = source.updatedAt.isValid()
            ? source.updatedAt.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"))
            : QStringLiteral("时间未知");
        html += QStringLiteral(
                    "<section data-nocturne-source=\"sticky\" "
                    "data-nocturne-source-id=\"%1\" "
                    "data-nocturne-source-updated=\"%2\">"
                    "<h2>%3</h2><p><small>便签 %4 · %5</small></p>%6</section>%7")
                    .arg(source.id)
                    .arg(updatedIso.toHtmlEscaped(),
                         sourceTitle.toHtmlEscaped())
                    .arg(index + 1)
                    .arg(updatedDisplay.toHtmlEscaped(),
                         stickyHtml(source.plainText),
                         index + 1 < sources.size() ? QStringLiteral("<hr>")
                                                    : QString());
        plainText += QStringLiteral("\n\n%1\n便签 %2 · %3\n%4")
                         .arg(sourceTitle)
                         .arg(index + 1)
                         .arg(updatedDisplay, source.plainText);
    }

    QSqlDatabase database = openedDatabase(connectionName_, error);
    if (!database.isValid() || !beginTransaction(database, error))
        return 0;

    const QString storageNow = toStorageDateTime(collectedDateTime);
    QSqlQuery insertNote(database);
    insertNote.prepare(QStringLiteral(
        "INSERT INTO notes(folder_id, kind, title, html, plain_text, excerpt, "
        "body_revision, content_hash, created_at, updated_at) "
        "VALUES(:folder_id, 'note', :title, :html, :plain_text, :excerpt, 1, "
        ":content_hash, :created_at, :updated_at)"));
    insertNote.bindValue(QStringLiteral(":folder_id"), nullableId(folderId));
    insertNote.bindValue(QStringLiteral(":title"), safeTitle);
    insertNote.bindValue(QStringLiteral(":html"), html);
    insertNote.bindValue(QStringLiteral(":plain_text"), plainText);
    insertNote.bindValue(QStringLiteral(":excerpt"), noteExcerpt(plainText));
    insertNote.bindValue(QStringLiteral(":content_hash"), contentHash(html));
    insertNote.bindValue(QStringLiteral(":created_at"), storageNow);
    insertNote.bindValue(QStringLiteral(":updated_at"), storageNow);
    if (!insertNote.exec()) {
        setError(error, queryError(QStringLiteral("创建合册笔记失败"), insertNote));
        database.rollback();
        return 0;
    }
    const qint64 collectedNoteId = insertNote.lastInsertId().toLongLong();
    if (collectedNoteId <= 0) {
        setError(error, QStringLiteral("创建合册笔记失败：数据库未返回有效编号。"));
        database.rollback();
        return 0;
    }

    QSqlQuery insertSource(database);
    insertSource.prepare(QStringLiteral(
        "INSERT INTO note_sources(note_id, source_note_id, source_kind, "
        "source_updated_at, position) "
        "VALUES(:note_id, :source_note_id, :source_kind, :source_updated_at, :position)"));
    for (int index = 0; index < sources.size(); ++index) {
        const NoteRecord &source = sources.at(index);
        insertSource.bindValue(QStringLiteral(":note_id"), collectedNoteId);
        insertSource.bindValue(QStringLiteral(":source_note_id"), source.id);
        insertSource.bindValue(QStringLiteral(":source_kind"), source.kind);
        insertSource.bindValue(QStringLiteral(":source_updated_at"),
                               toStorageDateTime(source.updatedAt));
        insertSource.bindValue(QStringLiteral(":position"), index);
        if (!insertSource.exec()) {
            setError(error,
                     queryError(QStringLiteral("记录合册来源失败"), insertSource));
            database.rollback();
            return 0;
        }
        insertSource.finish();
    }

    if (!commitTransaction(database, error))
        return 0;
    return collectedNoteId;
}

QList<NoteSourceRecord> Database::noteSources(qint64 noteId, QString *error) const
{
    clearError(error);
    QList<NoteSourceRecord> sources;
    QSqlDatabase database = openedDatabase(connectionName_, error);
    if (!database.isValid())
        return sources;

    QSqlQuery query(database);
    query.prepare(QStringLiteral(
        "SELECT note_id, source_note_id, source_kind, source_updated_at, position "
        "FROM note_sources WHERE note_id = :note_id ORDER BY position, source_note_id"));
    query.bindValue(QStringLiteral(":note_id"), noteId);
    if (!query.exec()) {
        setError(error, queryError(QStringLiteral("读取合册来源失败"), query));
        return {};
    }
    while (query.next()) {
        NoteSourceRecord source;
        source.noteId = query.value(0).toLongLong();
        source.sourceNoteId = query.value(1).toLongLong();
        source.sourceKind = query.value(2).toString();
        source.sourceUpdatedAt = fromStorageDateTime(query.value(3));
        source.position = query.value(4).toInt();
        sources.append(source);
    }
    return sources;
}

QList<FolderRecord> Database::listFolders(QString *error) const
{
    clearError(error);
    QList<FolderRecord> records;
    QSqlDatabase database = openedDatabase(connectionName_, error);
    if (!database.isValid())
        return records;

    QSqlQuery query(database);
    if (!query.exec(QStringLiteral(
            "SELECT id, parent_id, name, sort_order FROM folders "
            "ORDER BY parent_id IS NOT NULL, parent_id, sort_order, name COLLATE NOCASE"))) {
        setError(error, queryError(QStringLiteral("读取分组失败"), query));
        return {};
    }
    while (query.next())
        records.append(folderFromQuery(query));
    return records;
}

qint64 Database::createFolder(const QString &name,
                              QString *error,
                              qint64 parentId)
{
    clearError(error);
    const QString safeName = name.simplified();
    if (safeName.isEmpty()) {
        setError(error, QStringLiteral("分组名称不能为空。"));
        return 0;
    }

    QSqlDatabase database = openedDatabase(connectionName_, error);
    if (!database.isValid() || !beginTransaction(database, error))
        return 0;

    QSqlQuery orderQuery(database);
    orderQuery.prepare(QStringLiteral(
        "SELECT COALESCE(MAX(sort_order), -1) + 1 FROM folders "
        "WHERE COALESCE(parent_id, 0) = :parent_id"));
    orderQuery.bindValue(QStringLiteral(":parent_id"), std::max<qint64>(0, parentId));
    if (!orderQuery.exec() || !orderQuery.next()) {
        setError(error, queryError(QStringLiteral("计算分组顺序失败"), orderQuery));
        database.rollback();
        return 0;
    }
    const int sortOrder = orderQuery.value(0).toInt();
    orderQuery.finish();

    const QString now = toStorageDateTime(QDateTime::currentDateTimeUtc());
    QSqlQuery query(database);
    query.prepare(QStringLiteral(
        "INSERT INTO folders(parent_id, name, sort_order, created_at, updated_at) "
        "VALUES(:parent_id, :name, :sort_order, :created_at, :updated_at)"));
    query.bindValue(QStringLiteral(":parent_id"), nullableId(parentId));
    query.bindValue(QStringLiteral(":name"), safeName);
    query.bindValue(QStringLiteral(":sort_order"), sortOrder);
    query.bindValue(QStringLiteral(":created_at"), now);
    query.bindValue(QStringLiteral(":updated_at"), now);
    if (!query.exec()) {
        setError(error, queryError(QStringLiteral("创建分组失败"), query));
        database.rollback();
        return 0;
    }
    const qint64 id = query.lastInsertId().toLongLong();
    if (id <= 0) {
        setError(error, QStringLiteral("创建分组失败：数据库未返回有效编号。"));
        database.rollback();
        return 0;
    }
    if (!commitTransaction(database, error))
        return 0;
    return id;
}

bool Database::renameFolder(qint64 id,
                            const QString &name,
                            QString *error)
{
    clearError(error);
    const QString safeName = name.simplified();
    if (safeName.isEmpty()) {
        setError(error, QStringLiteral("分组名称不能为空。"));
        return false;
    }

    QSqlDatabase database = openedDatabase(connectionName_, error);
    if (!database.isValid() || !beginTransaction(database, error))
        return false;

    QSqlQuery query(database);
    query.prepare(QStringLiteral(
        "UPDATE folders SET name = :name, updated_at = :updated_at WHERE id = :id"));
    query.bindValue(QStringLiteral(":name"), safeName);
    query.bindValue(QStringLiteral(":updated_at"),
                    toStorageDateTime(QDateTime::currentDateTimeUtc()));
    query.bindValue(QStringLiteral(":id"), id);
    if (!query.exec()) {
        setError(error, queryError(QStringLiteral("重命名分组失败"), query));
        database.rollback();
        return false;
    }
    if (query.numRowsAffected() == 0) {
        setError(error, QStringLiteral("重命名分组失败：分组不存在。"));
        database.rollback();
        return false;
    }
    return commitTransaction(database, error);
}

bool Database::deleteFolder(qint64 id, QString *error)
{
    clearError(error);
    QSqlDatabase database = openedDatabase(connectionName_, error);
    if (!database.isValid() || !beginTransaction(database, error))
        return false;

    const QString now = toStorageDateTime(QDateTime::currentDateTimeUtc());
    QSqlQuery detach(database);
    detach.prepare(QStringLiteral(
        "UPDATE notes SET folder_id = NULL, updated_at = :updated_at "
        "WHERE folder_id = :id"));
    detach.bindValue(QStringLiteral(":updated_at"), now);
    detach.bindValue(QStringLiteral(":id"), id);
    if (!detach.exec()) {
        setError(error, queryError(QStringLiteral("移出分组中的笔记失败"), detach));
        database.rollback();
        return false;
    }

    QSqlQuery query(database);
    query.prepare(QStringLiteral("DELETE FROM folders WHERE id = :id"));
    query.bindValue(QStringLiteral(":id"), id);
    if (!query.exec()) {
        setError(error, queryError(QStringLiteral("删除分组失败"), query));
        database.rollback();
        return false;
    }
    if (query.numRowsAffected() == 0) {
        setError(error, QStringLiteral("删除分组失败：分组不存在。"));
        database.rollback();
        return false;
    }
    return commitTransaction(database, error);
}

std::optional<NoteRecord> Database::stickyNote(QString *error) const
{
    clearError(error);
    QSqlDatabase database = openedDatabase(connectionName_, error);
    if (!database.isValid())
        return std::nullopt;

    QSqlQuery query(database);
    if (!query.exec(QStringLiteral(
            "SELECT id, title, html, plain_text, excerpt, folder_id, kind, "
            "body_revision, content_hash, created_at, updated_at "
            "FROM notes WHERE kind = 'sticky' AND deleted_at IS NULL "
            "ORDER BY id LIMIT 1"))) {
        setError(error, queryError(QStringLiteral("读取快捷便签失败"), query));
        return std::nullopt;
    }
    if (!query.next())
        return std::nullopt;
    return noteFromQuery(query);
}

qint64 Database::saveStickyNote(const QString &text, QString *error)
{
    clearError(error);
    QSqlDatabase database = openedDatabase(connectionName_, error);
    if (!database.isValid())
        return -1;

    qint64 id = 0;
    QSqlQuery lookup(database);
    if (!lookup.exec(QStringLiteral(
            "SELECT id FROM notes WHERE kind = 'sticky' "
            "AND deleted_at IS NULL ORDER BY id LIMIT 1"))) {
        setError(error, queryError(QStringLiteral("查找快捷便签失败"), lookup));
        return -1;
    }
    if (lookup.next())
        id = lookup.value(0).toLongLong();
    lookup.finish();

    return saveStickyNote(id, text, error);
}

qint64 Database::saveStickyNote(qint64 id, const QString &text, QString *error)
{
    clearError(error);
    QSqlDatabase database = openedDatabase(connectionName_, error);
    if (!database.isValid() || !beginTransaction(database, error))
        return -1;

    if (id <= 0 && text.trimmed().isEmpty()) {
        if (!commitTransaction(database, error))
            return -1;
        return 0;
    }

    const QString html = stickyHtml(text);
    const QByteArray hash = contentHash(html);
    const QString now = toStorageDateTime(QDateTime::currentDateTimeUtc());
    const bool creating=id<=0;

    if (id > 0) {
        if(!WorkspaceStore(*this).capture(id,QStringLiteral("便签编辑前"),error)){database.rollback();return -1;}
        QSqlQuery update(database);
        update.prepare(QStringLiteral(
            "UPDATE notes SET html = :html, plain_text = :plain_text, "
            "excerpt = :excerpt, body_revision = CASE WHEN content_hash = :old_hash "
            "THEN body_revision ELSE body_revision + 1 END, "
            "content_hash = :content_hash, updated_at = :updated_at "
            "WHERE id = :id AND kind = 'sticky' AND deleted_at IS NULL"));
        update.bindValue(QStringLiteral(":html"), html);
        update.bindValue(QStringLiteral(":plain_text"), nonNullText(text));
        update.bindValue(QStringLiteral(":excerpt"), noteExcerpt(text));
        update.bindValue(QStringLiteral(":old_hash"), hash);
        update.bindValue(QStringLiteral(":content_hash"), hash);
        update.bindValue(QStringLiteral(":updated_at"), now);
        update.bindValue(QStringLiteral(":id"), id);
        if (!update.exec() || update.numRowsAffected() == 0) {
            setError(error, queryError(QStringLiteral("保存快捷便签失败"), update));
            database.rollback();
            return -1;
        }
    } else {
        QSqlQuery insert(database);
        insert.prepare(QStringLiteral(
            "INSERT INTO notes(folder_id, kind, title, html, plain_text, excerpt, "
            "body_revision, content_hash, created_at, updated_at) "
            "VALUES(NULL, 'sticky', :title, :html, :plain_text, :excerpt, 1, "
            ":content_hash, :created_at, :updated_at)"));
        insert.bindValue(QStringLiteral(":title"), stickyTitle(text));
        insert.bindValue(QStringLiteral(":html"), html);
        insert.bindValue(QStringLiteral(":plain_text"), nonNullText(text));
        insert.bindValue(QStringLiteral(":excerpt"), noteExcerpt(text));
        insert.bindValue(QStringLiteral(":content_hash"), hash);
        insert.bindValue(QStringLiteral(":created_at"), now);
        insert.bindValue(QStringLiteral(":updated_at"), now);
        if (!insert.exec()) {
            setError(error, queryError(QStringLiteral("创建快捷便签失败"), insert));
            database.rollback();
            return -1;
        }
        id = insert.lastInsertId().toLongLong();
        if (id <= 0) {
            setError(error, QStringLiteral("创建快捷便签失败：数据库未返回有效编号。"));
            database.rollback();
            return -1;
        }
    }

    if(!WorkspaceStore(*this).capture(id,creating?QStringLiteral("便签创建"):QStringLiteral("编辑保存"),error)){database.rollback();return -1;}
    if (!commitTransaction(database, error))
        return -1;
    return id;
}

QList<TodoRecord> Database::listTodos(QString *error) const
{
    clearError(error);
    QList<TodoRecord> records;
    QSqlDatabase database = openedDatabase(connectionName_, error);
    if (!database.isValid())
        return records;

    QSqlQuery query(database);
    query.prepare(QStringLiteral(
        "SELECT t.id, t.text, t.done, t.due_at, t.sort_order, l.note_id, l.anchor, n.title FROM todos t "
        "LEFT JOIN todo_links l ON l.todo_id=t.id LEFT JOIN notes n ON n.id=l.note_id "
        "WHERE l.todo_id IS NULL OR (l.active=1 AND n.deleted_at IS NULL) "
        "ORDER BY t.done ASC, t.sort_order ASC, t.id ASC"));
    if (!query.exec()) {
        setError(error, queryError(QStringLiteral("读取待办失败"), query));
        return {};
    }
    while (query.next()) {
        TodoRecord todo = todoFromQuery(query);
        todo.noteId = query.value(5).toLongLong();
        todo.anchor = query.value(6).toString();
        todo.noteTitle = query.value(7).toString();
        records.append(todo);
    }
    return records;
}

qint64 Database::createTodo(const QString &text, QString *error)
{
    clearError(error);
    QSqlDatabase database = openedDatabase(connectionName_, error);
    if (!database.isValid() || !beginTransaction(database, error))
        return 0;

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
    if (!commitTransaction(database, error))
        return 0;
    return id;
}

bool Database::updateTodoDone(qint64 id, bool done, QString *error)
{
    clearError(error);
    QSqlDatabase database = openedDatabase(connectionName_, error);
    if (!database.isValid() || !beginTransaction(database, error))
        return false;

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
    if (!database.isValid() || !beginTransaction(database, error))
        return false;

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

bool Database::deleteTodo(qint64 id, QString* error)
{
    clearError(error);
    QSqlDatabase database = openedDatabase(connectionName_, error);
    if (!database.isValid() || !beginTransaction(database, error)) return false;
    QSqlQuery query(database);
    query.prepare(QStringLiteral("DELETE FROM todos WHERE id=?"));
    query.addBindValue(id);
    if (!query.exec() || query.numRowsAffected() != 1) {
        setError(error, query.lastError().isValid() ? queryError(QStringLiteral("删除待办失败"), query)
                                                 : QStringLiteral("这条待办已不存在。"));
        database.rollback(); return false;
    }
    return commitTransaction(database, error);
}

QString Database::quickNote(QString *error) const
{
    const std::optional<NoteRecord> record = stickyNote(error);
    return record.has_value() ? record->plainText : QString();
}

bool Database::saveQuickNote(const QString &text, QString *error)
{
    return saveStickyNote(text, error) >= 0;
}

bool Database::moveFolder(qint64 id, qint64 parentId, QString* error)
{
    clearError(error);
    QSqlDatabase db = openedDatabase(connectionName_, error);
    if (!db.isValid() || !beginTransaction(db, error)) return false;
    QSqlQuery cycle(db);
    cycle.prepare(QStringLiteral("WITH RECURSIVE tree(id) AS (SELECT ? UNION SELECT f.id FROM folders f JOIN tree ON f.parent_id=tree.id) SELECT id FROM tree WHERE id=?"));
    cycle.addBindValue(id); cycle.addBindValue(parentId);
    if (!cycle.exec() || cycle.next()) {
        setError(error, QStringLiteral("不能把目录移入自身或其子目录。")); db.rollback(); return false;
    }
    cycle.finish();
    QSqlQuery query(db);
    query.prepare(QStringLiteral("UPDATE folders SET parent_id=?, updated_at=? WHERE id=?"));
    query.addBindValue(nullableId(parentId));
    query.addBindValue(toStorageDateTime(QDateTime::currentDateTimeUtc())); query.addBindValue(id);
    if (!query.exec() || query.numRowsAffected() != 1) {
        setError(error, QStringLiteral("移动失败：目标不存在或存在同名目录。")); db.rollback(); return false;
    }
    return commitTransaction(db, error);
}

qint64 Database::importNote(const QString& sourcePath, const QByteArray& sourceHash,
    const QString& title, const QString& html, const QString& plainText,
    qint64 folderId, bool* skipped, QString* error)
{
    clearError(error);
    if (skipped) *skipped = false;
    QSqlDatabase db = openedDatabase(connectionName_, error);
    if (!db.isValid() || !beginTransaction(db, error)) return 0;
    QSqlQuery existing(db);
    existing.prepare(QStringLiteral("SELECT n.id, n.deleted_at FROM import_sources s JOIN notes n ON n.id=s.note_id WHERE s.source_path=? AND s.source_hash=?"));
    existing.addBindValue(sourcePath); existing.addBindValue(sourceHash);
    if (!existing.exec()) { setError(error, existing.lastError().text()); db.rollback(); return 0; }
    if (existing.next() && existing.value(1).isNull()) {
        const qint64 id = existing.value(0).toLongLong(); existing.finish(); db.rollback();
        if (skipped) *skipped = true;
        return id;
    }
    existing.finish();
    const QString now = toStorageDateTime(QDateTime::currentDateTimeUtc());
    QSqlQuery insert(db);
    insert.prepare(QStringLiteral("INSERT INTO notes(folder_id,kind,title,html,plain_text,excerpt,body_revision,content_hash,created_at,updated_at) VALUES(?,'note',?,?,?,?,1,?,?,?)"));
    insert.addBindValue(nullableId(folderId)); insert.addBindValue(nonNullText(title));
    insert.addBindValue(nonNullText(html)); insert.addBindValue(nonNullText(plainText));
    insert.addBindValue(noteExcerpt(plainText)); insert.addBindValue(contentHash(html));
    insert.addBindValue(now); insert.addBindValue(now);
    if (!insert.exec()) { setError(error, insert.lastError().text()); db.rollback(); return 0; }
    const qint64 id = insert.lastInsertId().toLongLong();
    QSqlQuery source(db);
    source.prepare(QStringLiteral("INSERT OR REPLACE INTO import_sources(source_path,source_hash,note_id) VALUES(?,?,?)"));
    source.addBindValue(sourcePath); source.addBindValue(sourceHash); source.addBindValue(id);
    if (!source.exec()) { setError(error, source.lastError().text()); db.rollback(); return 0; }
    return commitTransaction(db, error) ? id : 0;
}

qint64 Database::linkNote(const QString& sourcePath, const QByteArray& sourceHash,
    const QString& sourceKind, const QString& title, const QString& html,
    const QString& plainText, qint64 folderId, bool* skipped, QString* error,const QByteArray* sourceBytes)
{
    clearError(error);
    if (skipped) *skipped = false;
    QSqlDatabase db = openedDatabase(connectionName_, error);
    if (!db.isValid() || !beginTransaction(db, error)) return 0;
    QSqlQuery existing(db);
    existing.prepare(QStringLiteral("SELECT s.note_id,s.source_hash,n.deleted_at FROM linked_sources s JOIN notes n ON n.id=s.note_id WHERE s.source_path=?"));
    existing.addBindValue(sourcePath);
    if (!existing.exec()) { setError(error, existing.lastError().text()); db.rollback(); return 0; }
    if (existing.next()) {
        const qint64 id = existing.value(0).toLongLong();
        const QByteArray previousHash = existing.value(1).toByteArray();
        const bool deleted=!existing.value(2).isNull();
        existing.finish();
        if (deleted) {
            db.rollback(); if (skipped) *skipped = true; return id;
        }
        if(previousHash==sourceHash){
            if(sourceBytes && !WorkspaceStore(*this).cacheSource(id,*sourceBytes,error)){db.rollback();return 0;}
            if(!commitTransaction(db,error))return 0;if(skipped)*skipped=true;return id;
        }
        if(!WorkspaceStore(*this).capture(id,QStringLiteral("外部更新前"),error)){db.rollback();return 0;}
        QSqlQuery update(db);
        update.prepare(QStringLiteral(
            "UPDATE notes SET html=?, plain_text=?, excerpt=?, content_hash=?, "
            "body_revision=body_revision+1, updated_at=? WHERE id=? AND deleted_at IS NULL"));
        update.addBindValue(nonNullText(html)); update.addBindValue(nonNullText(plainText));
        update.addBindValue(noteExcerpt(plainText)); update.addBindValue(contentHash(html));
        update.addBindValue(toStorageDateTime(QDateTime::currentDateTimeUtc())); update.addBindValue(id);
        if (!update.exec() || update.numRowsAffected() != 1) {
            setError(error, update.lastError().text()); db.rollback(); return 0;
        }
        QSqlQuery source(db);
        source.prepare(QStringLiteral("UPDATE linked_sources SET source_kind=?, source_hash=?, linked_at=? WHERE source_path=?"));
        source.addBindValue(sourceKind); source.addBindValue(sourceHash);
        source.addBindValue(toStorageDateTime(QDateTime::currentDateTimeUtc())); source.addBindValue(sourcePath);
        if (!source.exec()) { setError(error, source.lastError().text()); db.rollback(); return 0; }
        if(sourceBytes && !WorkspaceStore(*this).cacheSource(id,*sourceBytes,error)){db.rollback();return 0;}
        if(!WorkspaceStore(*this).capture(id,QStringLiteral("外部同步"),error)){db.rollback();return 0;}
        return commitTransaction(db, error) ? id : 0;
    }
    existing.finish();
    const QString now = toStorageDateTime(QDateTime::currentDateTimeUtc());
    QSqlQuery insert(db);
    insert.prepare(QStringLiteral(
        "INSERT INTO notes(folder_id,kind,title,html,plain_text,excerpt,body_revision,content_hash,created_at,updated_at) "
        "VALUES(?,'note',?,?,?,?,1,?,?,?)"));
    insert.addBindValue(nullableId(folderId)); insert.addBindValue(nonNullText(title));
    insert.addBindValue(nonNullText(html)); insert.addBindValue(nonNullText(plainText));
    insert.addBindValue(noteExcerpt(plainText)); insert.addBindValue(contentHash(html));
    insert.addBindValue(now); insert.addBindValue(now);
    if (!insert.exec()) { setError(error, insert.lastError().text()); db.rollback(); return 0; }
    const qint64 id = insert.lastInsertId().toLongLong();
    QSqlQuery source(db);
    source.prepare(QStringLiteral("INSERT INTO linked_sources(source_path,source_kind,source_hash,note_id,linked_at) VALUES(?,?,?,?,?)"));
    source.addBindValue(sourcePath); source.addBindValue(sourceKind); source.addBindValue(sourceHash);
    source.addBindValue(id); source.addBindValue(now);
    if (!source.exec()) { setError(error, source.lastError().text()); db.rollback(); return 0; }
    if(sourceBytes && !WorkspaceStore(*this).cacheSource(id,*sourceBytes,error)){db.rollback();return 0;}
    return commitTransaction(db, error) ? id : 0;
}

QString Database::noteSourcePath(qint64 noteId) const
{
    QSqlQuery query(QSqlDatabase::database(connectionName_));
    query.prepare(QStringLiteral(
        "SELECT source_path FROM linked_sources WHERE note_id=? "
        "UNION ALL SELECT source_path FROM import_sources WHERE note_id=? LIMIT 1"));
    query.addBindValue(noteId);
    query.addBindValue(noteId);
    return query.exec() && query.next() ? query.value(0).toString() : QString();
}

std::optional<LinkedSourceRecord> Database::linkedSource(qint64 noteId, QString* error) const
{
    clearError(error);
    QSqlQuery query(QSqlDatabase::database(connectionName_));
    query.prepare(QStringLiteral("SELECT note_id,source_path,source_kind,source_hash,linked_at FROM linked_sources WHERE note_id=?"));
    query.addBindValue(noteId);
    if (!query.exec()) { setError(error, query.lastError().text()); return std::nullopt; }
    if (!query.next()) return std::nullopt;
    LinkedSourceRecord record;
    record.noteId = query.value(0).toLongLong();
    record.sourcePath = query.value(1).toString();
    record.sourceKind = query.value(2).toString();
    record.sourceHash = query.value(3).toByteArray();
    record.linkedAt = fromStorageDateTime(query.value(4).toString());
    return record;
}

bool Database::updateLinkedSourceHash(qint64 noteId, const QByteArray& sourceHash, QString* error)
{
    clearError(error);
    QSqlDatabase db = openedDatabase(connectionName_, error);
    if (!db.isValid() || !beginTransaction(db, error)) return false;
    QSqlQuery query(db);
    query.prepare(QStringLiteral("UPDATE linked_sources SET source_hash=?, linked_at=? WHERE note_id=?"));
    query.addBindValue(sourceHash); query.addBindValue(toStorageDateTime(QDateTime::currentDateTimeUtc())); query.addBindValue(noteId);
    if (!query.exec() || query.numRowsAffected() != 1) {
        setError(error, query.lastError().text()); db.rollback(); return false;
    }
    return commitTransaction(db, error);
}

qint64 Database::createLinkedTodo(qint64 noteId, int expectedRevision, const QString& text,
    const QString& anchor, const QString& html, const QString& plainText, QString* error)
{
    clearError(error);
    if (text.trimmed().isEmpty() || anchor.isEmpty()) { setError(error, QStringLiteral("请选择需要设置为待办的文字。")); return 0; }
    QSqlDatabase db = openedDatabase(connectionName_, error);
    if (!db.isValid() || !beginTransaction(db, error)) return 0;
    if(!WorkspaceStore(*this).capture(noteId,QStringLiteral("待办标记前"),error)){db.rollback();return 0;}
    QSqlQuery noteUpdate(db);
    noteUpdate.prepare(QStringLiteral("UPDATE notes SET html=?, plain_text=?, excerpt=?, content_hash=?, body_revision=body_revision+1, updated_at=? WHERE id=? AND body_revision=? AND kind='note' AND deleted_at IS NULL"));
    noteUpdate.addBindValue(html); noteUpdate.addBindValue(plainText); noteUpdate.addBindValue(noteExcerpt(plainText));
    noteUpdate.addBindValue(contentHash(html)); noteUpdate.addBindValue(toStorageDateTime(QDateTime::currentDateTimeUtc()));
    noteUpdate.addBindValue(noteId); noteUpdate.addBindValue(expectedRevision);
    if (!noteUpdate.exec() || noteUpdate.numRowsAffected() != 1) {
        setError(error, QStringLiteral("笔记已变化或不可编辑，请重新打开后再试。")); db.rollback(); return 0;
    }
    QSqlQuery todo(db);
    todo.prepare(QStringLiteral("INSERT INTO todos(text,done,sort_order) SELECT ?,0,COALESCE(MAX(sort_order),-1)+1 FROM todos"));
    todo.addBindValue(text);
    if (!todo.exec()) { setError(error, todo.lastError().text()); db.rollback(); return 0; }
    const qint64 id = todo.lastInsertId().toLongLong();
    QSqlQuery link(db);
    link.prepare(QStringLiteral("INSERT INTO todo_links(todo_id,note_id,anchor) VALUES(?,?,?)"));
    link.addBindValue(id); link.addBindValue(noteId); link.addBindValue(anchor);
    if (!link.exec() || !syncTodoLinks(db, noteId, html, error)) {
        if (error && error->isEmpty()) *error = link.lastError().text();
        db.rollback(); return 0;
    }
    if(!WorkspaceStore(*this).capture(noteId,QStringLiteral("设置待办"),error)){db.rollback();return 0;}
    return commitTransaction(db, error) ? id : 0;
}

qint64 Database::ensureImportedFolder(const QString& sourcePath, const QString& name,
    qint64 parentId, QString* error)
{
    clearError(error);
    QSqlDatabase db = openedDatabase(connectionName_, error);
    if (!db.isValid() || !beginTransaction(db, error)) return 0;
    QSqlQuery known(db);
    known.prepare(QStringLiteral("SELECT folder_id FROM import_folders WHERE source_path=?"));
    known.addBindValue(sourcePath);
    if (!known.exec()) { setError(error, known.lastError().text()); db.rollback(); return 0; }
    if (known.next()) {
        const qint64 id = known.value(0).toLongLong(); known.finish(); db.rollback(); return id;
    }
    known.finish();
    QSqlQuery existing(db);
    existing.prepare(QStringLiteral("SELECT id FROM folders WHERE COALESCE(parent_id,0)=? AND name=? COLLATE NOCASE"));
    existing.addBindValue(qMax<qint64>(0, parentId)); existing.addBindValue(name.simplified());
    if (!existing.exec()) { setError(error, existing.lastError().text()); db.rollback(); return 0; }
    qint64 id = existing.next() ? existing.value(0).toLongLong() : 0;
    existing.finish();
    if (!id) {
        const QString now = toStorageDateTime(QDateTime::currentDateTimeUtc());
        QSqlQuery folder(db);
        folder.prepare(QStringLiteral("INSERT INTO folders(parent_id,name,sort_order,created_at,updated_at) SELECT ?,?,COALESCE(MAX(sort_order),-1)+1,?,? FROM folders WHERE COALESCE(parent_id,0)=?"));
        folder.addBindValue(nullableId(parentId)); folder.addBindValue(name.simplified());
        folder.addBindValue(now); folder.addBindValue(now); folder.addBindValue(qMax<qint64>(0, parentId));
        if (!folder.exec()) { setError(error, folder.lastError().text()); db.rollback(); return 0; }
        id = folder.lastInsertId().toLongLong();
    }
    QSqlQuery link(db);
    link.prepare(QStringLiteral("INSERT INTO import_folders(source_path,folder_id) VALUES(?,?)"));
    link.addBindValue(sourcePath); link.addBindValue(id);
    if (!link.exec()) { setError(error, link.lastError().text()); db.rollback(); return 0; }
    return commitTransaction(db, error) ? id : 0;
}

qint64 Database::ensureLinkedFolder(const QString& sourcePath, const QString& name,
    qint64 parentId, QString* error)
{
    clearError(error);
    QSqlDatabase db = openedDatabase(connectionName_, error);
    if (!db.isValid() || !beginTransaction(db, error)) return 0;
    QSqlQuery known(db);
    known.prepare(QStringLiteral("SELECT folder_id FROM linked_folders WHERE source_path=?"));
    known.addBindValue(sourcePath);
    if (!known.exec()) { setError(error, known.lastError().text()); db.rollback(); return 0; }
    if (known.next()) { const qint64 id = known.value(0).toLongLong(); db.rollback(); return id; }
    known.finish();
    QSqlQuery existing(db);
    existing.prepare(QStringLiteral("SELECT id FROM folders WHERE COALESCE(parent_id,0)=? AND name=? COLLATE NOCASE"));
    existing.addBindValue(qMax<qint64>(0, parentId)); existing.addBindValue(name.simplified());
    if (!existing.exec()) { setError(error, existing.lastError().text()); db.rollback(); return 0; }
    qint64 id = existing.next() ? existing.value(0).toLongLong() : 0;
    existing.finish();
    if (!id) {
        const QString now = toStorageDateTime(QDateTime::currentDateTimeUtc());
        QSqlQuery folder(db);
        folder.prepare(QStringLiteral("INSERT INTO folders(parent_id,name,sort_order,created_at,updated_at) SELECT ?,?,COALESCE(MAX(sort_order),-1)+1,?,? FROM folders WHERE COALESCE(parent_id,0)=?"));
        folder.addBindValue(nullableId(parentId)); folder.addBindValue(name.simplified());
        folder.addBindValue(now); folder.addBindValue(now); folder.addBindValue(qMax<qint64>(0, parentId));
        if (!folder.exec()) { setError(error, folder.lastError().text()); db.rollback(); return 0; }
        id = folder.lastInsertId().toLongLong();
    }
    QSqlQuery link(db);
    link.prepare(QStringLiteral("INSERT INTO linked_folders(source_path,folder_id) VALUES(?,?)"));
    link.addBindValue(sourcePath); link.addBindValue(id);
    if (!link.exec()) { setError(error, link.lastError().text()); db.rollback(); return 0; }
    return commitTransaction(db, error) ? id : 0;
}
