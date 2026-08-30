#include "Database.h"

#include <QCoreApplication>
#include <QDir>
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
}

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName(QStringLiteral("FeatherNoteTests"));
    QCoreApplication::setApplicationName(
        QStringLiteral("Persistence-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));

    bool ok = true;
    QString testDataDirectory;
    {
        Database database;
        QString error;
        ok &= check(database.open(&error), "database opens");
        if (!ok) {
            std::cerr << error.toStdString() << '\n';
            return 1;
        }
        testDataDirectory = database.dataDirectory();

        const qint64 emptyId = database.createNote(QStringLiteral("空白"),
                                                   QStringLiteral("<p><br></p>"),
                                                   QString(),
                                                   &error);
        ok &= check(emptyId > 0, "null QString is normalized before SQLite binding");

        ok &= check(database.updateNote(emptyId,
                                        QStringLiteral("游戏灵感"),
                                        QStringLiteral("<p><b>飞行城市</b></p>"),
                                        QStringLiteral("飞行城市"),
                                        &error),
                    "note updates");
        const auto loaded = database.note(emptyId, &error);
        ok &= check(loaded.has_value() && loaded->plainText == QStringLiteral("飞行城市"),
                    "note round-trips as UTF-8/SQLite text");
        ok &= check(database.listNotes(QStringLiteral("飞行"), &error).size() == 1,
                    "plain-text search finds note");

        const qint64 todoId = database.createTodo(QStringLiteral("画概念图"), &error);
        ok &= check(todoId > 0, "todo creates");
        ok &= check(database.updateTodoDone(todoId, true, &error), "todo completes");
        const auto todos = database.listTodos(&error);
        ok &= check(todos.size() == 1 && todos.first().done, "todo state round-trips");

        ok &= check(database.saveQuickNote(QStringLiteral("临时灵感"), &error),
                    "quick note saves");
        ok &= check(database.quickNote(&error) == QStringLiteral("临时灵感"),
                    "quick note round-trips");

        ok &= check(database.softDeleteNote(emptyId, &error), "note soft-deletes");
        ok &= check(!database.note(emptyId, &error).has_value(), "deleted note is hidden");
    }

    if (!testDataDirectory.isEmpty())
        QDir(testDataDirectory).removeRecursively();

    if (!ok)
        return 1;
    std::cout << "PASS: persistence smoke test\n";
    return 0;
}
