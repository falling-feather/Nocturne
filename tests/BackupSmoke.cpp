#include "BackupManager.h"
#include "Database.h"

#include <QCoreApplication>
#include <QDate>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTime>
#include <QTimeZone>
#include <QUuid>

#include <iostream>

namespace {
bool check(bool condition, const char* message)
{
    if (!condition)
        std::cerr << "FAIL: " << message << '\n';
    return condition;
}

bool snapshotContains(const QString& databasePath, const QString& title)
{
    const QString connectionName = QStringLiteral("backup-test-%1")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    bool found = false;
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                           connectionName);
        database.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
        database.setDatabaseName(databasePath);
        if (database.open()) {
            QSqlQuery query(database);
            query.prepare(QStringLiteral(
                "SELECT COUNT(*) FROM notes WHERE title = ? AND deleted_at IS NULL"));
            query.addBindValue(title);
            found = query.exec() && query.next() && query.value(0).toInt() == 1;
            database.close();
        }
    }
    QSqlDatabase::removeDatabase(connectionName);
    return found;
}
} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName(QStringLiteral("FeatherNoteTests"));
    QCoreApplication::setApplicationName(
        QStringLiteral("Backup-%1")
            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    QCoreApplication::setApplicationVersion(QStringLiteral(NOCTURNE_VERSION));

    bool ok = true;
    QString dataDirectory;
    {
        Database database;
        QString error;
        ok &= check(database.open(&error), "source database opens");
        if (!ok) {
            std::cerr << error.toStdString() << '\n';
            return 1;
        }
        dataDirectory = database.dataDirectory();

        const qint64 noteId = database.createNote(
            QStringLiteral("可恢复的潮汐草稿"),
            QStringLiteral("<p>潮汐、灯塔与航线。</p>"),
            QStringLiteral("潮汐、灯塔与航线。"),
            &error);
        ok &= check(noteId > 0, "source note creates");

        const QString attachmentDirectory = QDir(dataDirectory).filePath(
            QStringLiteral("attachments/concepts"));
        ok &= check(QDir().mkpath(attachmentDirectory),
                    "source attachment directory creates");
        QFile attachment(QDir(attachmentDirectory).filePath(
            QStringLiteral("lighthouse.txt")));
        ok &= check(attachment.open(QIODevice::WriteOnly)
                        && attachment.write("nocturne-attachment") > 0,
                    "source attachment writes");
        attachment.close();

        BackupManager manager(dataDirectory);
        const QDateTime manualTime(QDate(2026, 9, 1),
                                   QTime(7, 30),
                                   QTimeZone::systemTimeZone());
        const BackupResult manual = manager.create(BackupKind::Manual,
                                                   manualTime,
                                                   2);
        if (!manual.success)
            std::cerr << "BACKUP ERROR: " << manual.error.toStdString() << '\n';
        ok &= check(manual.success && manual.created,
                    "manual backup succeeds and creates a snapshot");
        const QString snapshotPath = QDir(manual.directory).filePath(
            QStringLiteral("notebook.sqlite3"));
        const QString copiedAttachment = QDir(manual.directory).filePath(
            QStringLiteral("attachments/concepts/lighthouse.txt"));
        const QString manifestPath = QDir(manual.directory).filePath(
            QStringLiteral("backup.json"));
        ok &= check(QFileInfo::exists(snapshotPath),
                    "backup database exists");
        ok &= check(QFileInfo::exists(copiedAttachment),
                    "nested attachment is copied");
        ok &= check(snapshotContains(snapshotPath,
                                     QStringLiteral("可恢复的潮汐草稿")),
                    "backup database opens independently and contains the note");

        QFile manifestFile(manifestPath);
        ok &= check(manifestFile.open(QIODevice::ReadOnly),
                    "backup manifest exists");
        const QJsonObject manifest = QJsonDocument::fromJson(
            manifestFile.readAll()).object();
        ok &= check(manifest.value(QStringLiteral("application")).toString()
                            == QStringLiteral("Nocturne")
                        && manifest.value(QStringLiteral("kind")).toString()
                            == QStringLiteral("manual")
                        && manifest.value(QStringLiteral("attachmentFileCount")).toInt()
                            == 1,
                    "backup manifest describes the verified payload");

        const QString unrelatedDirectory = QDir(manager.backupRoot()).filePath(
            QStringLiteral("keep-me"));
        ok &= check(QDir().mkpath(unrelatedDirectory),
                    "unrelated directory creates beside backups");
        const QString invalidLookalike = QDir(manager.backupRoot()).filePath(
            QStringLiteral("NocturneBackup-auto-20200101-000000-000-deadbeef"));
        ok &= check(QDir().mkpath(invalidLookalike),
                    "lookalike backup directory creates");
        QFile invalidManifest(QDir(invalidLookalike).filePath(
            QStringLiteral("backup.json")));
        ok &= check(invalidManifest.open(QIODevice::WriteOnly)
                        && invalidManifest.write("not a nocturne manifest") > 0,
                    "invalid lookalike manifest writes");
        invalidManifest.close();

        const QTimeZone localZone = QTimeZone::systemTimeZone();
        const QDateTime firstDay(QDate(2026, 9, 1), QTime(8, 0), localZone);
        const QDateTime secondDay(QDate(2026, 9, 2), QTime(8, 0), localZone);
        const QDateTime thirdDay(QDate(2026, 9, 3), QTime(8, 0), localZone);
        const BackupResult automaticFirst = manager.create(BackupKind::Automatic,
                                                           firstDay,
                                                           2);
        const BackupResult automaticDuplicate = manager.create(
            BackupKind::Automatic, firstDay.addSecs(3600), 2);
        const BackupResult automaticSecond = manager.create(BackupKind::Automatic,
                                                            secondDay,
                                                            2);
        const BackupResult automaticThird = manager.create(BackupKind::Automatic,
                                                           thirdDay,
                                                           2);
        for (const BackupResult* result : {&automaticFirst,
                                          &automaticDuplicate,
                                          &automaticSecond,
                                          &automaticThird}) {
            if (!result->success)
                std::cerr << "BACKUP ERROR: " << result->error.toStdString() << '\n';
        }
        ok &= check(automaticFirst.success && automaticFirst.created,
                    "first daily automatic backup creates");
        ok &= check(automaticDuplicate.success && !automaticDuplicate.created,
                    "second automatic backup on the same local date is skipped");
        ok &= check(automaticSecond.success && automaticSecond.created
                        && automaticThird.success && automaticThird.created,
                    "automatic backups create on later dates");
        ok &= check(!QFileInfo::exists(automaticFirst.directory)
                        && QFileInfo::exists(automaticSecond.directory)
                        && QFileInfo::exists(automaticThird.directory),
                    "automatic retention removes only the oldest recognized backup");
        ok &= check(QFileInfo::exists(unrelatedDirectory)
                        && QFileInfo::exists(invalidLookalike)
                        && QFileInfo::exists(manual.directory),
                    "retention preserves unrelated, invalid and manual directories");
        ok &= check(QDir(manager.backupRoot())
                            .entryList(QStringList{QStringLiteral("*.partial")},
                                       QDir::Dirs | QDir::NoDotAndDotDot)
                            .isEmpty(),
                    "successful backups leave no partial directory");
    }

    if (!dataDirectory.isEmpty())
        QDir(dataDirectory).removeRecursively();

    if (ok)
        std::cout << "PASS: consistent database snapshot, attachments, daily limit and retention\n";
    return ok ? 0 : 1;
}
