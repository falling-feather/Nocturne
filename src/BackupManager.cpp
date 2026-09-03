#include "BackupManager.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

#include <algorithm>
#include <utility>

namespace {
constexpr auto kDatabaseFile = "notebook.sqlite3";
constexpr auto kAttachmentsDirectory = "attachments";
constexpr auto kManifestFile = "backup.json";

QString kindName(BackupKind kind)
{
    return kind == BackupKind::Automatic ? QStringLiteral("auto")
                                         : QStringLiteral("manual");
}

QString sqlStringLiteral(QString value)
{
    value.replace(QLatin1Char('\''), QStringLiteral("''"));
    return value;
}

bool copyAttachmentTree(const QString& source,
                        const QString& destination,
                        int* fileCount,
                        qint64* totalBytes,
                        QString* error)
{
    const QDir sourceDirectory(source);
    if (!sourceDirectory.exists())
        return true;
    if (!QDir().mkpath(destination)) {
        if (error)
            *error = QStringLiteral("无法创建附件备份目录：%1").arg(destination);
        return false;
    }

    QDirIterator iterator(source,
                          QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot,
                          QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const QString sourcePath = iterator.next();
        const QFileInfo info = iterator.fileInfo();
        if (info.isSymLink())
            continue;

        const QString relativePath = sourceDirectory.relativeFilePath(sourcePath);
        const QString destinationPath = QDir(destination).filePath(relativePath);
        if (info.isDir()) {
            if (!QDir().mkpath(destinationPath)) {
                if (error)
                    *error = QStringLiteral("无法创建附件子目录：%1").arg(relativePath);
                return false;
            }
            continue;
        }

        if (!QDir().mkpath(QFileInfo(destinationPath).absolutePath())
            || !QFile::copy(sourcePath, destinationPath)) {
            if (error)
                *error = QStringLiteral("无法备份附件：%1").arg(relativePath);
            return false;
        }
        if (fileCount)
            ++(*fileCount);
        if (totalBytes)
            *totalBytes += info.size();
    }
    return true;
}

bool createDatabaseSnapshot(const QString& sourcePath,
                            const QString& destinationPath,
                            QString* error)
{
    const QString connectionName = QStringLiteral("nocturne-backup-source-%1")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    bool success = false;
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                           connectionName);
        database.setDatabaseName(sourcePath);
        if (!database.open()) {
            if (error)
                *error = QStringLiteral("无法打开源数据库：%1")
                             .arg(database.lastError().text());
        } else {
            QSqlQuery setup(database);
            setup.exec(QStringLiteral("PRAGMA busy_timeout = 5000"));
            setup.finish();
            QSqlQuery snapshot(database);
            success = snapshot.exec(
                QStringLiteral("VACUUM INTO '%1'")
                    .arg(sqlStringLiteral(QDir::toNativeSeparators(destinationPath))));
            if (!success && error) {
                *error = QStringLiteral("无法生成一致性数据库快照：%1")
                             .arg(snapshot.lastError().text());
            }
            database.close();
        }
    }
    QSqlDatabase::removeDatabase(connectionName);
    return success;
}

bool validateDatabaseSnapshot(const QString& databasePath,
                              int* schemaVersion,
                              QString* error)
{
    const QString connectionName = QStringLiteral("nocturne-backup-check-%1")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    bool success = false;
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                           connectionName);
        database.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
        database.setDatabaseName(databasePath);
        if (!database.open()) {
            if (error)
                *error = QStringLiteral("无法复核数据库快照：%1")
                             .arg(database.lastError().text());
        } else {
            QSqlQuery quickCheck(database);
            const bool checkExecuted = quickCheck.exec(QStringLiteral("PRAGMA quick_check"));
            const bool checkOk = checkExecuted && quickCheck.next()
                && quickCheck.value(0).toString() == QStringLiteral("ok");

            QSqlQuery versionQuery(database);
            const bool versionOk = versionQuery.exec(QStringLiteral("PRAGMA user_version"))
                && versionQuery.next();
            if (schemaVersion && versionOk)
                *schemaVersion = versionQuery.value(0).toInt();

            success = checkOk && versionOk;
            if (!success && error) {
                *error = checkExecuted
                    ? QStringLiteral("数据库快照完整性校验未通过")
                    : QStringLiteral("无法执行数据库快照校验：%1")
                          .arg(quickCheck.lastError().text());
            }
            database.close();
        }
    }
    QSqlDatabase::removeDatabase(connectionName);
    return success;
}

bool writeManifest(const QString& directory,
                   BackupKind kind,
                   const QDateTime& createdAt,
                   int schemaVersion,
                   int attachmentFileCount,
                   qint64 attachmentBytes,
                   qint64 databaseBytes,
                   QString* error)
{
    QJsonObject manifest;
    manifest.insert(QStringLiteral("formatVersion"), 1);
    manifest.insert(QStringLiteral("application"), QStringLiteral("Nocturne"));
    manifest.insert(QStringLiteral("applicationVersion"),
                    QCoreApplication::applicationVersion());
    manifest.insert(QStringLiteral("kind"), kindName(kind));
    manifest.insert(QStringLiteral("createdAtUtc"),
                    createdAt.toUTC().toString(Qt::ISODateWithMs));
    manifest.insert(QStringLiteral("databaseFile"), QString::fromLatin1(kDatabaseFile));
    manifest.insert(QStringLiteral("schemaVersion"), schemaVersion);
    manifest.insert(QStringLiteral("databaseBytes"), QString::number(databaseBytes));
    manifest.insert(QStringLiteral("attachmentsDirectory"),
                    QString::fromLatin1(kAttachmentsDirectory));
    manifest.insert(QStringLiteral("attachmentFileCount"), attachmentFileCount);
    manifest.insert(QStringLiteral("attachmentBytes"), QString::number(attachmentBytes));

    QSaveFile file(QDir(directory).filePath(QString::fromLatin1(kManifestFile)));
    if (!file.open(QIODevice::WriteOnly)
        || file.write(QJsonDocument(manifest).toJson(QJsonDocument::Indented)) < 0
        || !file.commit()) {
        if (error)
            *error = QStringLiteral("无法写入备份清单：%1").arg(file.errorString());
        return false;
    }
    return true;
}

QRegularExpression backupDirectoryPattern()
{
    return QRegularExpression(
        QStringLiteral("^NocturneBackup-(auto|manual)-[0-9]{8}-[0-9]{6}-[0-9]{3}-[0-9a-f]{8}$"));
}

bool hasRecognizedManifest(const QString& directory, BackupKind kind)
{
    QFile file(QDir(directory).filePath(QString::fromLatin1(kManifestFile)));
    if (!file.open(QIODevice::ReadOnly))
        return false;
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return false;
    const QJsonObject manifest = document.object();
    return manifest.value(QStringLiteral("formatVersion")).toInt() == 1
        && manifest.value(QStringLiteral("application")).toString()
            == QStringLiteral("Nocturne")
        && manifest.value(QStringLiteral("kind")).toString() == kindName(kind)
        && manifest.value(QStringLiteral("databaseFile")).toString()
            == QString::fromLatin1(kDatabaseFile);
}

int pruneBackups(const QString& root, BackupKind kind, int retentionLimit)
{
    if (retentionLimit < 1)
        return 0;

    const QString expectedKind = kindName(kind);
    const QRegularExpression pattern = backupDirectoryPattern();
    QList<QFileInfo> candidates;
    const QFileInfoList entries = QDir(root).entryInfoList(
        QDir::Dirs | QDir::NoDotAndDotDot,
        QDir::Name);
    for (const QFileInfo& entry : entries) {
        if (entry.isSymLink())
            continue;
        const QRegularExpressionMatch match = pattern.match(entry.fileName());
        if (!match.hasMatch() || match.captured(1) != expectedKind)
            continue;
        if (!hasRecognizedManifest(entry.absoluteFilePath(), kind)) {
            continue;
        }
        candidates.append(entry);
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const QFileInfo& left, const QFileInfo& right) {
                  return left.fileName() < right.fileName();
              });

    int removed = 0;
    while (candidates.size() > retentionLimit) {
        const QFileInfo oldest = candidates.takeFirst();
        if (QDir(oldest.absoluteFilePath()).removeRecursively())
            ++removed;
    }
    return removed;
}
} // namespace

BackupManager::BackupManager(QString dataDirectory)
    : m_dataDirectory(QDir::cleanPath(std::move(dataDirectory)))
{
}

QString BackupManager::backupRoot() const
{
    return QDir(m_dataDirectory).filePath(QStringLiteral("backups"));
}

bool BackupManager::hasAutomaticBackupForDate(const QDate& localDate) const
{
    if (!localDate.isValid())
        return false;
    const QString prefix = QStringLiteral("NocturneBackup-auto-%1-")
        .arg(localDate.toString(QStringLiteral("yyyyMMdd")));
    const QFileInfoList entries = QDir(backupRoot()).entryInfoList(
        QStringList{prefix + QStringLiteral("*")},
        QDir::Dirs | QDir::NoDotAndDotDot,
        QDir::Name);
    const QRegularExpression pattern = backupDirectoryPattern();
    for (const QFileInfo& entry : entries) {
        if (!entry.isSymLink()
            && pattern.match(entry.fileName()).hasMatch()
            && hasRecognizedManifest(entry.absoluteFilePath(),
                                     BackupKind::Automatic)) {
            return true;
        }
    }
    return false;
}

BackupResult BackupManager::create(BackupKind kind,
                                   const QDateTime& now,
                                   int retentionLimit) const
{
    BackupResult result;
    const QDateTime createdAt = now.isValid() ? now : QDateTime::currentDateTime();
    const QDateTime localCreatedAt = createdAt.toLocalTime();
    const QString root = backupRoot();
    if (!QDir().mkpath(root)) {
        result.error = QStringLiteral("无法创建备份根目录：%1").arg(root);
        return result;
    }

    if (kind == BackupKind::Automatic
        && hasAutomaticBackupForDate(localCreatedAt.date())) {
        result.success = true;
        return result;
    }

    const QString sourceDatabase = QDir(m_dataDirectory).filePath(
        QString::fromLatin1(kDatabaseFile));
    if (!QFileInfo::exists(sourceDatabase)) {
        result.error = QStringLiteral("找不到本地数据库：%1").arg(sourceDatabase);
        return result;
    }

    const QString uniqueSuffix = QUuid::createUuid()
        .toString(QUuid::WithoutBraces)
        .remove(QLatin1Char('-'))
        .left(8);
    const QString directoryName = QStringLiteral("NocturneBackup-%1-%2-%3")
        .arg(kindName(kind),
             localCreatedAt.toString(QStringLiteral("yyyyMMdd-HHmmss-zzz")),
             uniqueSuffix);
    const QString finalDirectory = QDir(root).filePath(directoryName);
    const QString partialDirectory = finalDirectory + QStringLiteral(".partial");
    if (!QDir().mkpath(partialDirectory)) {
        result.error = QStringLiteral("无法创建临时备份目录：%1").arg(partialDirectory);
        return result;
    }

    const auto fail = [&](const QString& message) {
        QDir(partialDirectory).removeRecursively();
        result.error = message;
        return result;
    };

    const QString snapshotPath = QDir(partialDirectory).filePath(
        QString::fromLatin1(kDatabaseFile));
    QString error;
    if (!createDatabaseSnapshot(sourceDatabase, snapshotPath, &error))
        return fail(error);

    int attachmentFileCount = 0;
    qint64 attachmentBytes = 0;
    const QString sourceAttachments = QDir(m_dataDirectory).filePath(
        QString::fromLatin1(kAttachmentsDirectory));
    const QString destinationAttachments = QDir(partialDirectory).filePath(
        QString::fromLatin1(kAttachmentsDirectory));
    if (!copyAttachmentTree(sourceAttachments,
                            destinationAttachments,
                            &attachmentFileCount,
                            &attachmentBytes,
                            &error)) {
        return fail(error);
    }

    int schemaVersion = 0;
    if (!validateDatabaseSnapshot(snapshotPath, &schemaVersion, &error))
        return fail(error);

    const qint64 databaseBytes = QFileInfo(snapshotPath).size();
    if (!writeManifest(partialDirectory,
                       kind,
                       createdAt,
                       schemaVersion,
                       attachmentFileCount,
                       attachmentBytes,
                       databaseBytes,
                       &error)) {
        return fail(error);
    }

    if (!QDir().rename(partialDirectory, finalDirectory))
        return fail(QStringLiteral("无法完成备份目录切换"));

    const int limit = retentionLimit > 0
        ? retentionLimit
        : (kind == BackupKind::Automatic ? AutomaticRetention : ManualRetention);
    result.success = true;
    result.created = true;
    result.directory = finalDirectory;
    result.removedOldBackups = pruneBackups(root, kind, limit);
    return result;
}
