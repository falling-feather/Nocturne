#pragma once
#include <QThread>
#include <QByteArray>
#include <QStringList>
#include <QHash>

class DocumentImporter final : public QThread {
    Q_OBJECT
public:
    DocumentImporter(QStringList paths, qint64 parentFolder, QObject* parent = nullptr,
        bool linkSources = false, QString rootNameOverride = QString());
    void runNow() { run(); }
    static bool readDocument(const QString& sourcePath, QString* html,
        QString* plainText, QByteArray* sourceBytes = nullptr,
        QString* error = nullptr);
    static void renderDocument(const QString& text, const QString& kind, const QString& directory,
        QString* html, QString* plainText);
    void setRootParents(const QHash<QString,qint64>& parents) { m_rootParents = parents; }
    void setRefreshMode() { m_registerScanRoots = false; }
    static bool linkRecentProjectDocs(const QString& workspaceRoot, int days,
        QString* report = nullptr);
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
    bool m_linkSources = false;
    bool m_registerScanRoots = true;
    QString m_rootNameOverride;
    QHash<QString,qint64> m_rootParents;
};
