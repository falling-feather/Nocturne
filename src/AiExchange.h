#pragma once
#include "WorkspaceStore.h"
#include <QJsonObject>
#include <QJsonArray>

struct HandoffResult
{
    QString manifestPath;
    int noteCount = 0;
    int imageCount = 0;
    QString error;
};
struct HandoffSelection
{
    qint64 noteId = 0;
    int start = 0;
    int end = 0;
    QString markdown;
    bool readOnly = false;
    QByteArray noteHash;
    QByteArray sourceHash;
    QString sourcePath;
};
namespace AiExchange
{
QString markdown(const NoteSnapshot& snapshot);
void replaceImageReference(QString& markdown, const QString& before, const QString& after);
HandoffResult create(Database& database, const QList<qint64>& ids, const QString& parentDirectory,
    bool includeImages, bool activeSharing, const std::optional<HandoffSelection>& selection = std::nullopt);
QJsonObject readSession(const QString& path, QString* error = nullptr);
bool setActive(const QString& path, bool active, QString* error = nullptr);
QString scopedPath(const QString& sessionFile, const QString& relative);
QJsonObject callTool(const QString& sessionFile, const QString& name, const QJsonObject& arguments);
QJsonArray tools();
int runMcp(int argc, char** argv);
}
