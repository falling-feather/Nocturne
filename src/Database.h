#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QString>

#include <optional>

struct NoteSummary {
    qint64 id = 0;
    qint64 folderId = 0;
    QString title;
    QString excerpt;
    QString kind = QStringLiteral("note");
    int bodyRevision = 1;
    QByteArray contentHash;
    QDateTime createdAt;
    QDateTime updatedAt;
};

struct NoteRecord : NoteSummary {
    QString html;
    QString plainText;
};

struct FolderRecord {
    qint64 id = 0;
    qint64 parentId = 0;
    QString name;
    int sortOrder = 0;
};

struct TodoRecord {
    qint64 id = 0;
    QString text;
    bool done = false;
    QDateTime dueAt;
    int sortOrder = 0;
};

class Database {
public:
    static constexpr qint64 AllFolders = -1;
    static constexpr qint64 UnfiledFolder = 0;

    Database();
    ~Database();

    Database(const Database &) = delete;
    Database &operator=(const Database &) = delete;

    bool open(QString *error = nullptr);
    QString dataDirectory() const;

    QList<NoteSummary> listNoteSummaries(const QString &filter = QString(),
                                         QString *error = nullptr,
                                         qint64 folderFilter = AllFolders) const;
    std::optional<NoteRecord> note(qint64 id, QString *error = nullptr) const;
    qint64 createNote(const QString &title,
                      const QString &html,
                      const QString &plainText,
                      QString *error = nullptr,
                      qint64 folderId = UnfiledFolder,
                      const QString &kind = QStringLiteral("note"));
    bool updateNote(qint64 id,
                    const QString &title,
                    const QString &html,
                    const QString &plainText,
                    QString *error = nullptr);
    bool renameNote(qint64 id,
                    const QString &title,
                    QString *error = nullptr);
    bool moveNoteToFolder(qint64 id,
                          qint64 folderId,
                          QString *error = nullptr);
    bool softDeleteNote(qint64 id, QString *error = nullptr);

    QList<FolderRecord> listFolders(QString *error = nullptr) const;
    qint64 createFolder(const QString &name,
                        QString *error = nullptr,
                        qint64 parentId = 0);
    bool renameFolder(qint64 id,
                      const QString &name,
                      QString *error = nullptr);
    bool deleteFolder(qint64 id, QString *error = nullptr);

    std::optional<NoteRecord> stickyNote(QString *error = nullptr) const;
    qint64 saveStickyNote(const QString &text, QString *error = nullptr);
    qint64 saveStickyNote(qint64 id,
                          const QString &text,
                          QString *error = nullptr);

    QList<TodoRecord> listTodos(QString *error = nullptr) const;
    qint64 createTodo(const QString &text, QString *error = nullptr);
    bool updateTodoDone(qint64 id, bool done, QString *error = nullptr);
    bool deleteCompletedTodos(QString *error = nullptr);

    // Compatibility wrappers for the 0.1.0 quick-note API.
    QString quickNote(QString *error = nullptr) const;
    bool saveQuickNote(const QString &text, QString *error = nullptr);

private:
    QString connectionName_;
    QString dataDirectory_;
    bool ftsEnabled_ = false;
};
