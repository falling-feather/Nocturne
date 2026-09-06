#pragma once
#include "BackupManager.h"
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QUrl>
#include <QCryptographicHash>
#include <QUuid>

// Keep daily data outside AppData: packaged launchers may virtualize that tree.
inline bool prepareDailyData(const QString& legacy, const QString& destination, QString* error)
{
    if (QFile::exists(destination + "/notebook.sqlite3")) return true;
    if (!QFile::exists(legacy + "/notebook.sqlite3")) return QDir().mkpath(destination);
    const auto backup = BackupManager(legacy).create(BackupKind::Manual);
    if (!backup.success) { *error = backup.error; return false; }
    const QString stage = destination + ".migration-" + QUuid::createUuid().toString(QUuid::Id128);
    if (!QDir().mkpath(stage)) { *error = QStringLiteral("无法建立资料迁移目录"); return false; }
    QDirIterator files(backup.directory, QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);
    while (files.hasNext()) {
        const QString source = files.next();
        const QString relative = QDir(backup.directory).relativeFilePath(source);
        const QString target = stage + "/" + relative;
        if (!QDir().mkpath(QFileInfo(target).absolutePath()) || !QFile::copy(source, target)) {
            *error = QStringLiteral("复制旧资料失败，原资料与备份保持不变：%1").arg(source); return false;
        }
    }
    const QString connection = QStringLiteral("profile-migration");
    bool success = false;
    {
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        db.setDatabaseName(stage + "/notebook.sqlite3");
        if (db.open() && db.transaction()) {
            bool hasHash = false;
            QSqlQuery columns(db); columns.exec("PRAGMA table_info(notes)");
            while (columns.next()) hasHash |= columns.value(1).toString() == "content_hash";
            QSqlQuery read(db); success = read.exec("SELECT id,html FROM notes");
            const QString oldPrefix = QUrl::fromLocalFile(legacy + "/attachments/").toString();
            const QString newPrefix = QUrl::fromLocalFile(destination + "/attachments/").toString();
            while (success && read.next()) {
                const QString html = read.value(1).toString();
                QString changed = html; changed.replace(oldPrefix, newPrefix);
                if (changed == html) continue;
                QSqlQuery update(db); update.prepare(hasHash ? "UPDATE notes SET html=?,content_hash=? WHERE id=?" : "UPDATE notes SET html=? WHERE id=?");
                update.addBindValue(changed);
                if (hasHash) update.addBindValue(QCryptographicHash::hash(changed.toUtf8(), QCryptographicHash::Sha256));
                update.addBindValue(read.value(0)); success = update.exec();
            }
            if (success) success = db.commit(); else db.rollback();
            QSqlQuery check(db); success = success && check.exec("PRAGMA quick_check") && check.next() && check.value(0).toString() == "ok";
        }
        if (!success) *error = QStringLiteral("迁移校验失败；原资料及快照保持不变。%1").arg(db.lastError().text());
        db.close();
    }
    QSqlDatabase::removeDatabase(connection);
    if (!success) return false;
    if (QDir(destination).exists() && !QDir().rmdir(destination)) { *error = QStringLiteral("目标目录非空，未覆盖任何资料：%1").arg(destination); return false; }
    if (!QDir().rename(stage, destination)) { *error = QStringLiteral("无法启用迁移后的资料目录：%1").arg(stage); return false; }
    return true;
}

