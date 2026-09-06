#pragma once
#include <QThread>
#include <QStringList>

class DocumentImporter final : public QThread {
    Q_OBJECT
public:
    DocumentImporter(QStringList paths, qint64 parentFolder, QObject* parent = nullptr);
    int imported = 0;
    int skipped = 0;
    qint64 lastNoteId = 0;
    QStringList errors;
signals:
    void progress(int imported, int skipped, const QString& currentFile);
protected:
    void run() override;
private:
    QStringList m_paths;
    qint64 m_parentFolder;
};
