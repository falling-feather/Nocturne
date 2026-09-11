#include "WorkspaceStore.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>
#include <QFileInfo>
#include <QSet>
#include <QDir>
#include <QUuid>
#include <algorithm>

namespace
{
void errorText(QString* error, const QString& text)
{
    if (error)
        *error = text;
}
bool execute(QSqlQuery& query, QString* error)
{
    if (query.exec())
        return true;
    errorText(error, query.lastError().text());
    return false;
}
QString now()
{
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}
QDateTime date(const QVariant& value)
{
    return QDateTime::fromString(value.toString(), Qt::ISODateWithMs);
}
QByteArray packed(const NoteSnapshot& value)
{
    return qCompress(QJsonDocument(value.toJson()).toJson(QJsonDocument::Compact), 6);
}
std::optional<NoteSnapshot> unpacked(const QByteArray& value, QString* error)
{
    if (value.size() < 4
        || (quint64(quint8(value[0])) << 24 | quint64(quint8(value[1])) << 16 | quint64(quint8(value[2])) << 8
               | quint8(value[3]))
            > 64 * 1024 * 1024)
    {
        errorText(error, QStringLiteral("历史快照大小无效。"));
        return {};
    }
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(qUncompress(value), &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject())
    {
        errorText(error, QStringLiteral("历史快照无法读取。"));
        return {};
    }
    return NoteSnapshot::fromJson(document.object());
}
int setting(QSqlDatabase db, const QString& key, int fallback)
{
    QSqlQuery query(db);
    query.prepare("SELECT value FROM settings WHERE key=?");
    query.addBindValue(key);
    return query.exec() && query.next() ? query.value(0).toInt() : fallback;
}
}

QJsonObject NoteSnapshot::toJson() const
{
    return { { "id", QString::number(note.id) }, { "folder", QString::number(note.folderId) },
        { "kind", note.kind }, { "title", note.title }, { "html", note.html }, { "text", note.plainText },
        { "sourcePath", sourcePath }, { "sourceBytes", QString::fromLatin1(sourceBytes.toBase64()) } };
}
NoteSnapshot NoteSnapshot::fromJson(const QJsonObject& object)
{
    NoteSnapshot value;
    value.note.id = object.value("id").toString().toLongLong();
    value.note.folderId = object.value("folder").toString().toLongLong();
    value.note.kind = object.value("kind").toString("note");
    value.note.title = object.value("title").toString();
    value.note.html = object.value("html").toString();
    value.note.plainText = object.value("text").toString();
    value.note.contentHash = QCryptographicHash::hash(value.note.html.toUtf8(), QCryptographicHash::Sha256);
    value.sourcePath = object.value("sourcePath").toString();
    value.sourceBytes = QByteArray::fromBase64(object.value("sourceBytes").toString().toLatin1());
    return value;
}
QSqlDatabase WorkspaceStore::connection() const
{
    return QSqlDatabase::database(m_database.connectionName_, false);
}
bool WorkspaceStore::initialize(QSqlDatabase& database, QString* error)
{
    const QStringList schema {
        "CREATE TABLE IF NOT EXISTS note_versions(id INTEGER PRIMARY KEY AUTOINCREMENT,note_id INTEGER NOT "
        "NULL REFERENCES notes(id),created_at TEXT NOT NULL,updated_at TEXT NOT NULL,reason TEXT NOT "
        "NULL,label TEXT NOT NULL DEFAULT '',pinned INTEGER NOT NULL DEFAULT 0,fingerprint BLOB NOT "
        "NULL,payload BLOB NOT NULL)",
        "CREATE INDEX IF NOT EXISTS note_versions_note_time ON note_versions(note_id,id DESC)",
        "CREATE TABLE IF NOT EXISTS source_cache(note_id INTEGER PRIMARY KEY REFERENCES notes(id),bytes BLOB "
        "NOT NULL)",
        "CREATE TABLE IF NOT EXISTS note_activity(note_id INTEGER PRIMARY KEY REFERENCES notes(id),pinned "
        "INTEGER NOT NULL DEFAULT 0,cursor_position INTEGER NOT NULL DEFAULT 0,scroll_position INTEGER NOT "
        "NULL DEFAULT 0,opened_at TEXT NOT NULL DEFAULT '',source_mode INTEGER NOT NULL DEFAULT 1)",
        "CREATE TABLE IF NOT EXISTS note_templates(id INTEGER PRIMARY KEY AUTOINCREMENT,name TEXT NOT "
        "NULL,payload BLOB NOT NULL,created_at TEXT NOT NULL)",
        "CREATE TABLE IF NOT EXISTS capture_sources(note_id INTEGER PRIMARY KEY REFERENCES "
        "notes(id),source_note_id INTEGER NOT NULL REFERENCES notes(id),position INTEGER NOT "
        "NULL,source_text TEXT NOT NULL)",
        "CREATE TABLE IF NOT EXISTS recovery_drafts(note_id INTEGER PRIMARY KEY REFERENCES notes(id),payload "
        "BLOB NOT NULL,updated_at TEXT NOT NULL)"
    };
    for (const auto& statement : schema)
    {
        QSqlQuery query(database);
        if (!query.exec(statement))
        {
            errorText(error, query.lastError().text());
            return false;
        }
    }
    return true;
}
std::optional<NoteSnapshot> WorkspaceStore::snapshot(qint64 noteId, bool includeDeleted, QString* error) const
{
    if (error)
        error->clear();
    QSqlQuery query(connection());
    query.prepare(
        "SELECT id,title,html,plain_text,folder_id,kind,body_revision,content_hash,created_at,updated_at "
        "FROM notes WHERE id=?"
        + QString(includeDeleted ? "" : " AND deleted_at IS NULL"));
    query.addBindValue(noteId);
    if (!execute(query, error) || !query.next())
    {
        if (error && error->isEmpty())
            *error = QStringLiteral("笔记不存在。");
        return {};
    }
    NoteSnapshot value;
    auto& note = value.note;
    note.id = query.value(0).toLongLong();
    note.title = query.value(1).toString();
    note.html = query.value(2).toString();
    note.plainText = query.value(3).toString();
    note.folderId = query.value(4).toLongLong();
    note.kind = query.value(5).toString();
    note.bodyRevision = query.value(6).toInt();
    note.contentHash = query.value(7).toByteArray();
    note.createdAt = date(query.value(8));
    note.updatedAt = date(query.value(9));
    value.sourceBytes = cachedSource(noteId);
    if (auto source = m_database.linkedSource(noteId))
        value.sourcePath = source->sourcePath;
    return value;
}
bool WorkspaceStore::capture(
    qint64 noteId, const QString& reason, QString* error, bool pinned, const QString& label)
{
    const auto value = snapshot(noteId, false, error);
    if (!value)
        return false;
    return captureSnapshot(*value, reason, error, pinned, label);
}
bool WorkspaceStore::captureSnapshot(
    const NoteSnapshot& value, const QString& reason, QString* error, bool pinned, const QString& label)
{
    const qint64 noteId = value.note.id;
    const QByteArray payload = packed(value);
    const QByteArray hash = QCryptographicHash::hash(
        QJsonDocument(value.toJson()).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256);
    QSqlQuery latest(connection());
    latest.prepare("SELECT id,created_at,reason,pinned,fingerprint FROM note_versions WHERE note_id=? ORDER "
                   "BY id DESC LIMIT 1");
    latest.addBindValue(noteId);
    if (!execute(latest, error))
        return false;
    qint64 replace = 0;
    if (latest.next())
    {
        if (!pinned && latest.value(4).toByteArray() == hash)
            return true;
        if (!pinned && reason == QStringLiteral("编辑保存") && latest.value(2).toString() == reason
            && !latest.value(3).toBool()
            && date(latest.value(1)).secsTo(QDateTime::currentDateTimeUtc()) < 300)
            replace = latest.value(0).toLongLong();
    }
    latest.finish();
    QSqlQuery query(connection());
    const QString stamp = now();
    if (replace)
    {
        query.prepare("UPDATE note_versions SET updated_at=?,fingerprint=?,payload=? WHERE id=?");
        query.addBindValue(stamp);
        query.addBindValue(hash);
        query.addBindValue(payload);
        query.addBindValue(replace);
    }
    else
    {
        query.prepare("INSERT INTO "
                      "note_versions(note_id,created_at,updated_at,reason,label,pinned,fingerprint,payload) "
                      "VALUES(?,?,?,?,?,?,?,?)");
        query.addBindValue(noteId);
        query.addBindValue(stamp);
        query.addBindValue(stamp);
        query.addBindValue(reason);
        query.addBindValue(label.isNull() ? QStringLiteral("") : label);
        query.addBindValue(pinned);
        query.addBindValue(hash);
        query.addBindValue(payload);
    }
    return execute(query, error) && prune(error);
}
QList<NoteVersion> WorkspaceStore::versions(qint64 noteId, QString* error) const
{
    QList<NoteVersion> result;
    QSqlQuery query(connection());
    query.prepare("SELECT id,note_id,created_at,updated_at,reason,label,pinned,length(payload) FROM "
                  "note_versions WHERE note_id=? ORDER BY id DESC");
    query.addBindValue(noteId);
    if (!execute(query, error))
        return result;
    while (query.next())
        result.append({ query.value(0).toLongLong(), query.value(1).toLongLong(), date(query.value(2)),
            date(query.value(3)), query.value(4).toString(), query.value(5).toString(),
            query.value(6).toBool(), query.value(7).toLongLong() });
    return result;
}
std::optional<NoteSnapshot> WorkspaceStore::version(qint64 versionId, QString* error) const
{
    QSqlQuery query(connection());
    query.prepare("SELECT payload FROM note_versions WHERE id=?");
    query.addBindValue(versionId);
    if (!execute(query, error) || !query.next())
    {
        if (error && error->isEmpty())
            *error = QStringLiteral("该历史版本已不存在。");
        return {};
    }
    return unpacked(query.value(0).toByteArray(), error);
}
bool WorkspaceStore::setVersionPinned(qint64 id, bool pinned, QString* error)
{
    QSqlQuery query(connection());
    query.prepare("UPDATE note_versions SET pinned=? WHERE id=?");
    query.addBindValue(pinned);
    query.addBindValue(id);
    return execute(query, error);
}
qint64 WorkspaceStore::historyBytes() const
{
    QSqlQuery query(connection());
    return query.exec("SELECT COALESCE(SUM(length(payload)),0) FROM note_versions") && query.next()
        ? query.value(0).toLongLong()
        : 0;
}
int WorkspaceStore::historyKeep() const
{
    return std::clamp(setting(connection(), "history/keep", 50), 10, 200);
}
int WorkspaceStore::historyLimitMiB() const
{
    return std::clamp(setting(connection(), "history/limitMiB", 128), 32, 512);
}
bool WorkspaceStore::setHistoryLimits(int keep, int limitMiB, QString* error)
{
    QSqlQuery query(connection());
    query.prepare(
        "INSERT INTO settings(key,value) VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value");
    for (const auto& pair : QList<QPair<QString, int>> { { "history/keep", std::clamp(keep, 10, 200) },
             { "history/limitMiB", std::clamp(limitMiB, 32, 512) } })
    {
        query.bindValue(0, pair.first);
        query.bindValue(1, QString::number(pair.second));
        if (!execute(query, error))
            return false;
    }
    return prune(error);
}
bool WorkspaceStore::prune(QString* error)
{
    QSqlQuery trim(connection());
    trim.prepare(
        "DELETE FROM note_versions WHERE id IN (SELECT id FROM (SELECT id,pinned,ROW_NUMBER() OVER(PARTITION "
        "BY note_id ORDER BY id DESC) AS rank FROM note_versions) WHERE rank>? AND pinned=0)");
    trim.addBindValue(historyKeep());
    if (!execute(trim, error))
        return false;
    qint64 excess = historyBytes() - qint64(historyLimitMiB()) * 1024 * 1024;
    if (excess <= 0)
        return true;
    QSqlQuery candidates(connection());
    if (!candidates.exec("SELECT id,length(payload) FROM note_versions v WHERE pinned=0 AND id NOT IN "
                         "(SELECT id FROM (SELECT id,ROW_NUMBER() OVER(PARTITION BY note_id ORDER BY id "
                         "DESC) AS rank FROM note_versions) WHERE rank<=2) ORDER BY id"))
    {
        errorText(error, candidates.lastError().text());
        return false;
    }
    QList<qint64> ids;
    while (excess > 0 && candidates.next())
    {
        ids.append(candidates.value(0).toLongLong());
        excess -= candidates.value(1).toLongLong();
    }
    candidates.finish();
    QSqlQuery remove(connection());
    remove.prepare("DELETE FROM note_versions WHERE id=? AND pinned=0");
    for (qint64 id : ids)
    {
        remove.bindValue(0, id);
        if (!execute(remove, error))
            return false;
    }
    return true; // Explicit checkpoints and the latest two states are never silently discarded.
}
QByteArray WorkspaceStore::cachedSource(qint64 noteId) const
{
    QSqlQuery query(connection());
    query.prepare("SELECT bytes FROM source_cache WHERE note_id=?");
    query.addBindValue(noteId);
    return query.exec() && query.next() ? qUncompress(query.value(0).toByteArray()) : QByteArray();
}
bool WorkspaceStore::cacheSource(qint64 noteId, const QByteArray& bytes, QString* error)
{
    QSqlQuery query(connection());
    query.prepare("INSERT INTO source_cache(note_id,bytes) VALUES(?,?) ON CONFLICT(note_id) DO UPDATE SET "
                  "bytes=excluded.bytes");
    query.addBindValue(noteId);
    query.addBindValue(qCompress(bytes));
    return execute(query, error);
}
QList<NoteSummary> WorkspaceStore::deletedNotes(QString* error) const
{
    QList<NoteSummary> result;
    QSqlQuery query(connection());
    if (!query.exec("SELECT id,title,folder_id,kind,deleted_at FROM notes WHERE deleted_at IS NOT NULL ORDER "
                    "BY deleted_at DESC"))
    {
        errorText(error, query.lastError().text());
        return result;
    }
    while (query.next())
    {
        NoteSummary value;
        value.id = query.value(0).toLongLong();
        value.title = query.value(1).toString();
        value.folderId = query.value(2).toLongLong();
        value.kind = query.value(3).toString();
        value.updatedAt = date(query.value(4));
        result.append(value);
    }
    return result;
}
bool WorkspaceStore::restoreDeleted(qint64 noteId, QString* error)
{
    QSqlQuery query(connection());
    query.prepare("UPDATE notes SET deleted_at=NULL,updated_at=? WHERE id=? AND deleted_at IS NOT NULL");
    query.addBindValue(now());
    query.addBindValue(noteId);
    if (!execute(query, error))
        return false;
    if (query.numRowsAffected() != 1)
    {
        errorText(error, QStringLiteral("该笔记不在回收站中。"));
        return false;
    }
    return true;
}
bool WorkspaceStore::saveDraft(qint64 noteId, const NoteSnapshot& value, QString* error)
{
    QSqlQuery query(connection());
    query.prepare("INSERT INTO recovery_drafts(note_id,payload,updated_at) VALUES(?,?,?) ON "
                  "CONFLICT(note_id) DO UPDATE SET payload=excluded.payload,updated_at=excluded.updated_at");
    query.addBindValue(noteId);
    query.addBindValue(packed(value));
    query.addBindValue(now());
    return execute(query, error);
}
QList<NoteSnapshot> WorkspaceStore::drafts(QString* error) const
{
    QList<NoteSnapshot> result;
    QSqlQuery query(connection());
    if (!query.exec("SELECT payload FROM recovery_drafts ORDER BY updated_at DESC"))
    {
        errorText(error, query.lastError().text());
        return result;
    }
    while (query.next())
        if (auto value = unpacked(query.value(0).toByteArray(), error))
            result.append(*value);
    return result;
}
bool WorkspaceStore::clearDraft(qint64 noteId, QString* error)
{
    QSqlQuery query(connection());
    query.prepare("DELETE FROM recovery_drafts WHERE note_id=?");
    query.addBindValue(noteId);
    return execute(query, error);
}
NoteActivity WorkspaceStore::activity(qint64 noteId) const
{
    QSqlQuery query(connection());
    query.prepare("SELECT pinned,cursor_position,scroll_position,opened_at,source_mode FROM note_activity "
                  "WHERE note_id=?");
    query.addBindValue(noteId);
    if (!query.exec() || !query.next())
        return { noteId };
    return { noteId, query.value(0).toBool(), query.value(1).toInt(), query.value(2).toInt(),
        date(query.value(3)), query.value(4).toBool() };
}
bool WorkspaceStore::recordPosition(
    qint64 noteId, int cursor, int scroll, bool opened, QString* error, bool sourceMode)
{
    QSqlQuery query(connection());
    query.prepare("INSERT INTO note_activity(note_id,cursor_position,scroll_position,opened_at,source_mode) "
                  "VALUES(?,?,?,?,?) ON CONFLICT(note_id) DO UPDATE SET "
                  "cursor_position=excluded.cursor_position,scroll_position=excluded.scroll_position,source_"
                  "mode=excluded.source_mode,opened_at=CASE WHEN ? THEN excluded.opened_at ELSE "
                  "note_activity.opened_at END");
    query.addBindValue(noteId);
    query.addBindValue(std::max(0, cursor));
    query.addBindValue(std::max(0, scroll));
    query.addBindValue(now());
    query.addBindValue(sourceMode);
    query.addBindValue(opened);
    return execute(query, error);
}
bool WorkspaceStore::setPinned(qint64 noteId, bool pinned, QString* error)
{
    QSqlQuery query(connection());
    query.prepare("INSERT INTO note_activity(note_id,pinned) VALUES(?,?) ON CONFLICT(note_id) DO UPDATE SET "
                  "pinned=excluded.pinned");
    query.addBindValue(noteId);
    query.addBindValue(pinned);
    return execute(query, error);
}
QList<NoteActivity> WorkspaceStore::activities() const
{
    QList<NoteActivity> result;
    QSqlQuery query(connection());
    if (query.exec("SELECT note_id,pinned,cursor_position,scroll_position,opened_at,source_mode FROM "
                   "note_activity ORDER BY opened_at DESC"))
        while (query.next())
            result.append({ query.value(0).toLongLong(), query.value(1).toBool(), query.value(2).toInt(),
                query.value(3).toInt(), date(query.value(4)), query.value(5).toBool() });
    return result;
}
QList<NoteTemplate> WorkspaceStore::templates(QString* error) const
{
    QList<NoteTemplate> result;
    QSqlQuery query(connection());
    if (!query.exec("SELECT id,name,payload FROM note_templates ORDER BY id DESC"))
    {
        errorText(error, query.lastError().text());
        return result;
    }
    while (query.next())
        if (auto content = unpacked(query.value(2).toByteArray(), error))
            result.append({ query.value(0).toLongLong(), query.value(1).toString(), *content });
    return result;
}
qint64 WorkspaceStore::saveTemplate(const QString& name, const NoteSnapshot& value, QString* error)
{
    if (name.trimmed().isEmpty())
    {
        errorText(error, QStringLiteral("请填写模板名称。"));
        return 0;
    }
    QSqlQuery query(connection());
    query.prepare("INSERT INTO note_templates(name,payload,created_at) VALUES(?,?,?)");
    query.addBindValue(name.trimmed());
    query.addBindValue(packed(value));
    query.addBindValue(now());
    return execute(query, error) ? query.lastInsertId().toLongLong() : 0;
}
bool WorkspaceStore::deleteTemplate(qint64 id, QString* error)
{
    QSqlQuery query(connection());
    query.prepare("DELETE FROM note_templates WHERE id=?");
    query.addBindValue(id);
    return execute(query, error);
}
bool WorkspaceStore::setCaptureSource(
    qint64 noteId, qint64 sourceNoteId, int position, const QString& text, QString* error)
{
    QSqlQuery query(connection());
    query.prepare(
        "INSERT INTO capture_sources(note_id,source_note_id,position,source_text) VALUES(?,?,?,?) ON "
        "CONFLICT(note_id) DO UPDATE SET "
        "source_note_id=excluded.source_note_id,position=excluded.position,source_text=excluded.source_text");
    query.addBindValue(noteId);
    query.addBindValue(sourceNoteId);
    query.addBindValue(position);
    query.addBindValue(text);
    return execute(query, error);
}
std::optional<CaptureSource> WorkspaceStore::captureSource(qint64 noteId) const
{
    QSqlQuery query(connection());
    query.prepare("SELECT source_note_id,position,source_text FROM capture_sources WHERE note_id=?");
    query.addBindValue(noteId);
    if (!query.exec() || !query.next())
        return {};
    return CaptureSource { noteId, query.value(0).toLongLong(), query.value(1).toInt(),
        query.value(2).toString() };
}
QList<QPair<qint64, QString>> WorkspaceStore::linkedFolderRoots(qint64 folderId) const
{
    QHash<qint64, qint64> parents;
    for (const auto& folder : m_database.listFolders())
        parents.insert(folder.id, folder.parentId);
    QSqlQuery query(connection());
    QHash<qint64, QString> linked;
    if (query.exec("SELECT folder_id,source_path FROM linked_folders"))
        while (query.next())
            linked.insert(query.value(0).toLongLong(), query.value(1).toString());
    QList<QPair<qint64, QString>> result;
    for (auto it = linked.cbegin(); it != linked.cend(); ++it)
    {
        bool inScope = folderId < 0 || it.key() == folderId;
        bool hasLinkedAncestor = false;
        QSet<qint64> seen;
        for (qint64 parent = parents.value(it.key()); parent > 0 && !seen.contains(parent);
            parent = parents.value(parent))
        {
            seen.insert(parent);
            if (parent == folderId)
                inScope = true;
            if (linked.contains(parent) && (folderId < 0 || parent == folderId || !inScope))
                hasLinkedAncestor = true;
        }
        if (inScope && !hasLinkedAncestor)
            result.append({ it.key(), it.value() });
    }
    return result;
}

QString WorkspaceStore::linkedFolderPath(qint64 folderId) const
{
    QSqlQuery query(connection());
    query.prepare("SELECT source_path FROM linked_folders WHERE folder_id=?");
    query.addBindValue(folderId);
    return query.exec() && query.next() ? query.value(0).toString() : QString();
}

bool WorkspaceStore::relinkFolder(qint64 folderId, const QString& directory, QString* error, int* linkedFiles)
{
    if (error)
        error->clear();
    if (linkedFiles)
        *linkedFiles = 0;
    const QString previous = QDir::cleanPath(linkedFolderPath(folderId));
    const QString target = QFileInfo(directory).canonicalFilePath();
    if (previous.isEmpty() || previous == "." || target.isEmpty() || !QFileInfo(target).isDir())
    {
        errorText(error, QStringLiteral("请选择一个已对接目录及存在的新目录。"));
        return false;
    }
    if (previous.compare(target, Qt::CaseInsensitive) == 0)
        return true;
    auto database = connection();
    if (!database.transaction())
    {
        errorText(error, database.lastError().text());
        return false;
    }
    struct Mapping
    {
        qint64 id;
        QString before;
        QString after;
    };
    QList<Mapping> folders, notes;
    for (const auto& table : QStringList { "linked_folders", "linked_sources" })
    {
        QSqlQuery query(database);
        const QString key = table == "linked_folders" ? "folder_id" : "note_id";
        if (!query.exec("SELECT " + key + ",source_path FROM " + table))
        {
            errorText(error, query.lastError().text());
            database.rollback();
            return false;
        }
        while (query.next())
        {
            const QString path = QDir::cleanPath(query.value(1).toString());
            if (path.compare(previous, Qt::CaseInsensitive) != 0
                && !path.startsWith(previous + "/", Qt::CaseInsensitive))
                continue;
            Mapping mapping { query.value(0).toLongLong(), path,
                QDir::cleanPath(target + path.mid(previous.size())) };
            (table == "linked_folders" ? folders : notes).append(mapping);
        }
    }
    for (const auto& mapping : notes)
    {
        auto value = snapshot(mapping.id, true, error);
        if (!value || !captureSnapshot(*value, QStringLiteral("目录重新关联前"), error))
        {
            database.rollback();
            return false;
        }
    }
    const QString temporary = "nocturne-relink:" + QUuid::createUuid().toString(QUuid::Id128) + ":";
    for (const auto& table : QStringList { "linked_folders", "linked_sources" })
    {
        const auto& mappings = table == "linked_folders" ? folders : notes;
        const QString key = table == "linked_folders" ? "folder_id" : "note_id";
        QSqlQuery query(database);
        query.prepare("UPDATE " + table + " SET source_path=? WHERE " + key + "=?");
        for (int phase = 0; phase < 2; ++phase)
            for (const auto& mapping : mappings)
            {
                query.bindValue(0, phase == 0 ? temporary + QString::number(mapping.id) : mapping.after);
                query.bindValue(1, mapping.id);
                if (!execute(query, error))
                {
                    database.rollback();
                    return false;
                }
            }
    }
    if (!database.commit())
    {
        errorText(error, database.lastError().text());
        database.rollback();
        return false;
    }
    if (linkedFiles)
        *linkedFiles = notes.size();
    return true;
}
