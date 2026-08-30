#pragma once

#include <QDateTime>
#include <QList>
#include <QString>

#include <optional>

struct NoteRecord {
    qint64 id = 0;
    QString title;
    QString html;
    QString plainText;
    QDateTime createdAt;
    QDateTime updatedAt;
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
    Database();
    ~Database();

    Database(const Database &) = delete;
    Database &operator=(const Database &) = delete;

    bool open(QString *error = nullptr);
    QString dataDirectory() const;

    QList<NoteRecord> listNotes(const QString &filter = QString(),
                                QString *error = nullptr) const;
    std::optional<NoteRecord> note(qint64 id, QString *error = nullptr) const;
    qint64 createNote(const QString &title,
                      const QString &html,
                      const QString &plainText,
                      QString *error = nullptr);
    bool updateNote(qint64 id,
                    const QString &title,
                    const QString &html,
                    const QString &plainText,
                    QString *error = nullptr);
    bool softDeleteNote(qint64 id, QString *error = nullptr);

    QList<TodoRecord> listTodos(QString *error = nullptr) const;
    qint64 createTodo(const QString &text, QString *error = nullptr);
    bool updateTodoDone(qint64 id, bool done, QString *error = nullptr);
    bool deleteCompletedTodos(QString *error = nullptr);

    QString quickNote(QString *error = nullptr) const;
    bool saveQuickNote(const QString &text, QString *error = nullptr);

private:
    QString connectionName_;
    QString dataDirectory_;
};
