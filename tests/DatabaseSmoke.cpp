#include "Database.h"

#include <QCoreApplication>
#include <QDir>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QString>
#include <QUuid>

#include <iostream>

namespace {

bool check(bool condition, const char* message)
{
    if (!condition)
        std::cerr << "FAIL: " << message << '\n';
    return condition;
}

bool execSql(QSqlQuery& query, const QString& sql, const char* message)
{
    if (query.exec(sql))
        return true;
    std::cerr << "FAIL: " << message << ": "
              << query.lastError().text().toStdString() << '\n';
    return false;
}

bool seedLegacyDatabase(const QString& dataDirectory)
{
    if (!QDir().mkpath(dataDirectory)) {
        std::cerr << "FAIL: legacy data directory creates\n";
        return false;
    }

    const QString connectionName = QStringLiteral("legacy-seed-%1")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    bool ok = true;
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                          connectionName);
        database.setDatabaseName(QDir(dataDirectory).filePath(
            QStringLiteral("notebook.sqlite3")));
        if (!database.open()) {
            std::cerr << "FAIL: legacy database opens: "
                      << database.lastError().text().toStdString() << '\n';
            ok = false;
        } else {
            QSqlQuery query(database);
            ok &= execSql(query,
                          QStringLiteral(
                              "CREATE TABLE notes ("
                              "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                              "title TEXT NOT NULL DEFAULT '',"
                              "html TEXT NOT NULL DEFAULT '',"
                              "plain_text TEXT NOT NULL DEFAULT '',"
                              "created_at TEXT NOT NULL,"
                              "updated_at TEXT NOT NULL,"
                              "deleted_at TEXT NULL)"),
                          "legacy notes table creates");
            ok &= execSql(query,
                          QStringLiteral(
                              "CREATE TABLE todos ("
                              "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                              "text TEXT NOT NULL,"
                              "done INTEGER NOT NULL DEFAULT 0,"
                              "due_at TEXT NULL,"
                              "sort_order INTEGER NOT NULL DEFAULT 0)"),
                          "legacy todos table creates");
            ok &= execSql(query,
                          QStringLiteral(
                              "CREATE TABLE settings ("
                              "key TEXT PRIMARY KEY, value TEXT NOT NULL DEFAULT '')"),
                          "legacy settings table creates");
            ok &= execSql(query,
                          QStringLiteral(
                              "INSERT INTO notes(title, html, plain_text, created_at, updated_at) "
                              "VALUES('旧笔记', '<p>旧正文</p>', '旧正文', "
                              "'2026-08-30T00:00:00.000Z', '2026-08-30T00:00:00.000Z')"),
                          "legacy note seeds");
            ok &= execSql(query,
                          QStringLiteral(
                              "INSERT INTO settings(key, value) "
                              "VALUES('quick_note', '迁移便签内容')"),
                          "legacy quick note seeds");
            database.close();
        }
    }
    QSqlDatabase::removeDatabase(connectionName);
    return ok;
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName(QStringLiteral("FeatherNoteTests"));
    QCoreApplication::setApplicationName(
        QStringLiteral("Persistence-%1")
            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));

    bool ok = true;
    const QString testDataDirectory = QStandardPaths::writableLocation(
        QStandardPaths::AppLocalDataLocation);
    ok &= seedLegacyDatabase(testDataDirectory);

    qint64 editedNoteId = 0;
    qint64 stickyId = 0;
    {
        Database database;
        QString error;
        ok &= check(database.open(&error), "legacy database migrates and opens");
        if (!ok) {
            std::cerr << error.toStdString() << '\n';
            return 1;
        }

        const QList<NoteSummary> migrated = database.listNoteSummaries(QString(), &error);
        ok &= check(migrated.size() == 2,
                    "legacy note and quick note migrate into summary list");
        ok &= check(database.listNoteSummaries(QStringLiteral("旧正文"), &error).size() == 1,
                    "summary search finds legacy body without returning bodies");

        const auto migratedSticky = database.stickyNote(&error);
        ok &= check(migratedSticky.has_value()
                        && migratedSticky->plainText == QStringLiteral("迁移便签内容")
                        && migratedSticky->kind == QStringLiteral("sticky"),
                    "legacy quick note becomes a first-class sticky note");
        stickyId = migratedSticky.has_value() ? migratedSticky->id : 0;

        const auto legacySearch = database.listNoteSummaries(QStringLiteral("旧笔记"), &error);
        ok &= check(legacySearch.size() == 1, "legacy title remains searchable");
        const auto legacyNote = legacySearch.isEmpty()
            ? std::optional<NoteRecord>()
            : database.note(legacySearch.first().id, &error);
        ok &= check(legacyNote.has_value()
                        && legacyNote->contentHash.size() == 32
                        && legacyNote->bodyRevision == 1
                        && legacyNote->excerpt.contains(QStringLiteral("旧正文")),
                    "migration backfills hash, revision and excerpt");

        editedNoteId = database.createNote(QStringLiteral("空白"),
                                           QStringLiteral("<p><br></p>"),
                                           QString(),
                                           &error);
        ok &= check(editedNoteId > 0,
                    "null QString is normalized before SQLite binding");

        ok &= check(database.updateNote(editedNoteId,
                                        QStringLiteral("游戏灵感"),
                                        QStringLiteral("<p><b>飞行城市原型</b></p>"),
                                        QStringLiteral("飞行城市原型"),
                                        &error),
                    "note body updates");
        const auto bodyUpdated = database.note(editedNoteId, &error);
        ok &= check(bodyUpdated.has_value()
                        && bodyUpdated->plainText == QStringLiteral("飞行城市原型")
                        && bodyUpdated->bodyRevision == 2
                        && bodyUpdated->contentHash.size() == 32,
                    "body update increments revision and refreshes hash");

        ok &= check(database.renameNote(editedNoteId,
                                        QStringLiteral("天空城构想"),
                                        &error),
                    "note renames without rewriting body");
        const auto renamed = database.note(editedNoteId, &error);
        ok &= check(renamed.has_value()
                        && renamed->title == QStringLiteral("天空城构想")
                        && renamed->bodyRevision == 2,
                    "metadata rename preserves body revision");
        ok &= check(database.listNoteSummaries(QStringLiteral("飞行"), &error).size() == 1,
                    "short Chinese search falls back and finds note");
        ok &= check(database.listNoteSummaries(QStringLiteral("飞行城市"), &error).size() == 1,
                    "trigram-capable search finds Chinese substring");

        const qint64 folderId = database.createFolder(QStringLiteral("游戏设计"), &error);
        ok &= check(folderId > 0, "folder creates");
        ok &= check(database.moveNoteToFolder(editedNoteId, folderId, &error),
                    "note moves to folder");
        const QList<NoteSummary> folderNotes = database.listNoteSummaries(
            QString(), &error, folderId);
        ok &= check(folderNotes.size() == 1 && folderNotes.first().id == editedNoteId,
                    "folder filter returns only assigned note");
        ok &= check(database.renameFolder(folderId, QStringLiteral("玩法构想"), &error),
                    "folder renames");
        const QList<FolderRecord> folders = database.listFolders(&error);
        ok &= check(folders.size() == 1
                        && folders.first().name == QStringLiteral("玩法构想"),
                    "renamed folder round-trips");
        ok &= check(database.deleteFolder(folderId, &error), "folder deletes");
        const auto unfiled = database.note(editedNoteId, &error);
        ok &= check(unfiled.has_value() && unfiled->folderId == 0,
                    "deleting folder safely unfiles note");

        const qint64 savedStickyId = database.saveStickyNote(
            QStringLiteral("迁移后的第二版便签"), &error);
        ok &= check(savedStickyId == stickyId, "sticky save reuses migrated note");
        ok &= check(database.quickNote(&error) == QStringLiteral("迁移后的第二版便签"),
                    "compatibility quick-note reader uses note table");
        ok &= check(database.saveQuickNote(QStringLiteral("兼容接口第三版"), &error),
                    "compatibility quick-note writer uses note table");

        const qint64 todoId = database.createTodo(QStringLiteral("画概念图"), &error);
        ok &= check(todoId > 0, "todo creates");
        ok &= check(database.updateTodoDone(todoId, true, &error), "todo completes");
        const auto todos = database.listTodos(&error);
        ok &= check(todos.size() == 1 && todos.first().done,
                    "todo state round-trips");

        ok &= check(database.softDeleteNote(editedNoteId, &error),
                    "note soft-deletes");
        ok &= check(!database.note(editedNoteId, &error).has_value(),
                    "deleted note is hidden");
    }

    {
        Database reopened;
        QString error;
        ok &= check(reopened.open(&error), "migrated database reopens idempotently");
        const auto sticky = reopened.stickyNote(&error);
        ok &= check(sticky.has_value()
                        && sticky->id == stickyId
                        && sticky->plainText == QStringLiteral("兼容接口第三版"),
                    "sticky note survives restart");
        ok &= check(reopened.listFolders(&error).isEmpty(),
                    "deleted folder stays deleted after restart");
        ok &= check(reopened.listTodos(&error).size() == 1,
                    "todo survives restart");
    }

    const QString normalizedTestPath = QDir::cleanPath(testDataDirectory);
    const QString normalizedTestRoot = QDir::cleanPath(
        QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation));
    if (!normalizedTestPath.isEmpty()
        && normalizedTestPath.contains(QStringLiteral("qttest"), Qt::CaseInsensitive)
        && normalizedTestPath.startsWith(normalizedTestRoot)) {
        QDir(testDataDirectory).removeRecursively();
    }

    if (!ok)
        return 1;
    std::cout << "PASS: persistence, migration, grouping and sticky smoke test\n";
    return 0;
}
