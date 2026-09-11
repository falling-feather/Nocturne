#pragma once
#include "Database.h"
#include <QJsonObject>
#include <QSqlDatabase>

struct NoteSnapshot
{
    NoteRecord note;
    QString sourcePath;
    QByteArray sourceBytes;
    QJsonObject toJson() const;
    static NoteSnapshot fromJson(const QJsonObject& object);
};
struct NoteVersion
{
    qint64 id = 0;
    qint64 noteId = 0;
    QDateTime createdAt;
    QDateTime updatedAt;
    QString reason;
    QString label;
    bool pinned = false;
    qint64 bytes = 0;
};
struct NoteActivity
{
    qint64 noteId = 0;
    bool pinned = false;
    int cursor = 0;
    int scroll = 0;
    QDateTime openedAt;
    bool sourceMode = false;
};
struct NoteTemplate
{
    qint64 id = 0;
    QString name;
    NoteSnapshot content;
};
struct CaptureSource
{
    qint64 noteId = 0;
    qint64 sourceNoteId = 0;
    int position = 0;
    QString text;
};

class WorkspaceStore
{
public:
    explicit WorkspaceStore(Database& database)
        : m_database(database)
    {
    }
    static bool initialize(QSqlDatabase& database, QString* error);
    bool capture(qint64 noteId, const QString& reason, QString* error = nullptr, bool pinned = false,
        const QString& label = {});
    bool captureSnapshot(const NoteSnapshot& snapshot, const QString& reason, QString* error = nullptr,
        bool pinned = false, const QString& label = {});
    QList<NoteVersion> versions(qint64 noteId, QString* error = nullptr) const;
    std::optional<NoteSnapshot> version(qint64 versionId, QString* error = nullptr) const;
    std::optional<NoteSnapshot> snapshot(
        qint64 noteId, bool includeDeleted = false, QString* error = nullptr) const;
    bool setVersionPinned(qint64 id, bool pinned, QString* error = nullptr);
    bool prune(QString* error = nullptr);
    qint64 historyBytes() const;
    int historyKeep() const;
    int historyLimitMiB() const;
    bool setHistoryLimits(int keep, int limitMiB, QString* error = nullptr);
    QByteArray cachedSource(qint64 noteId) const;
    bool cacheSource(qint64 noteId, const QByteArray& bytes, QString* error = nullptr);
    QList<NoteSummary> deletedNotes(QString* error = nullptr) const;
    bool restoreDeleted(qint64 noteId, QString* error = nullptr);
    bool saveDraft(qint64 noteId, const NoteSnapshot& snapshot, QString* error = nullptr);
    QList<NoteSnapshot> drafts(QString* error = nullptr) const;
    bool clearDraft(qint64 noteId, QString* error = nullptr);
    NoteActivity activity(qint64 noteId) const;
    bool recordPosition(qint64 noteId, int cursor, int scroll, bool opened, QString* error = nullptr,
        bool sourceMode = false);
    bool setPinned(qint64 noteId, bool pinned, QString* error = nullptr);
    QList<NoteActivity> activities() const;
    QList<NoteTemplate> templates(QString* error = nullptr) const;
    qint64 saveTemplate(const QString& name, const NoteSnapshot& snapshot, QString* error = nullptr);
    bool deleteTemplate(qint64 id, QString* error = nullptr);
    bool setCaptureSource(
        qint64 noteId, qint64 sourceNoteId, int position, const QString& text, QString* error = nullptr);
    std::optional<CaptureSource> captureSource(qint64 noteId) const;
    QStringList refreshRootPaths(QString* error = nullptr) const;
    bool setRefreshRoot(const QString& path, bool enabled, QString* error = nullptr);
    QList<QPair<qint64, QString>> linkedFolderRoots(qint64 folderId = -1, QString* error = nullptr) const;
    QString linkedFolderPath(qint64 folderId) const;
    bool relinkFolder(
        qint64 folderId, const QString& directory, QString* error = nullptr, int* linkedFiles = nullptr);

private:
    QSqlDatabase connection() const;
    Database& m_database;
};
