#pragma once

#include <QDateTime>
#include <QString>

enum class BackupKind {
    Automatic,
    Manual
};

struct BackupResult {
    bool success = false;
    bool created = false;
    QString directory;
    QString error;
    int removedOldBackups = 0;
};

class BackupManager final
{
public:
    static constexpr int AutomaticRetention = 7;
    static constexpr int ManualRetention = 10;

    explicit BackupManager(QString dataDirectory);

    QString backupRoot() const;
    bool hasAutomaticBackupForDate(const QDate& localDate) const;
    BackupResult create(BackupKind kind,
                        const QDateTime& now = QDateTime(),
                        int retentionLimit = -1) const;

private:
    QString m_dataDirectory;
};
