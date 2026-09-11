#include "Database.h"
#include "WorkspaceStore.h"
#include "SourceFile.h"
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QUuid>
#include <QStandardPaths>
#include <iostream>

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QStandardPaths::setTestModeEnabled(true);
    app.setOrganizationName("NocturneTests");
    app.setApplicationName("Workspace-" + QUuid::createUuid().toString(QUuid::Id128));
    QTemporaryDir profile;
    app.setProperty("nocturneDataDirectory", profile.path());
    bool ok = true;
    auto check = [&](bool condition, const char* message)
    {
        if (!condition)
        {
            ok = false;
            std::cerr << "FAIL: " << message << '\n';
        }
    };
    QString error;
    Database database;
    check(database.open(&error), "schema opens");
    WorkspaceStore store(database);
    const auto note = database.createNote("history", "<p>one</p>", "one", &error);
    check(database.updateNote(note, "history", "<p>two</p>", "two", &error), "first edit saves with history");
    check(database.updateNote(note, "history", "<p>three</p>", "three", &error), "second edit saves");
    const auto versions = store.versions(note);
    check(versions.size() == 2, "continuous edits coalesce while retaining the initial content");
    check(!versions.isEmpty() && store.version(versions.last().id)->note.plainText == "one",
        "initial version reconstructs independently");
    check(store.capture(note, "checkpoint", &error, true, "交接前"), "manual checkpoint persists");
    check(database.updateNote(note, "history", "<p>four</p>", "four", &error), "edits after checkpoint save");
    bool protectedVersion = false;
    for (const auto& version : store.versions(note))
        if (version.pinned && version.label == "交接前")
            protectedVersion = true;
    check(protectedVersion && store.prune(&error), "checkpoint is retained by pruning");
    check(database.softDeleteNote(note, &error) && !database.note(note),
        "deleted note disappears from normal views");
    check(store.deletedNotes().size() == 1 && store.restoreDeleted(note, &error),
        "recycle bin restores exactly one note");
    check(database.note(note)->plainText == "four", "recycle bin preserves latest body");
    check(store.setPinned(note, true, &error) && store.recordPosition(note, 4, 12, true, &error),
        "reading metadata persists");
    check(store.activity(note).pinned && store.activity(note).cursor == 4,
        "reading metadata retains pinned state");
    const auto templateId = store.saveTemplate("项目模板", *store.snapshot(note), &error);
    check(templateId > 0 && store.templates().size() == 1, "templates persist independent content");
    check(store.deleteTemplate(templateId, &error) && store.templates().isEmpty(),
        "template deletion does not delete notes");
    const auto unchangedHash = database.note(note)->contentHash;
    check(!database.updateNote(
              note, "history", "wrong", "wrong", &error, QStringLiteral("编辑保存"), QByteArray("stale")),
        "stale database version is rejected");
    check(database.note(note)->contentHash == unchangedHash, "stale save leaves note unchanged");
    QTemporaryDir files;
    const QString path = files.filePath("源文档.md");
    const QByteArray original = QByteArray::fromHex("efbbbf") + "# Heading\r\n\r\n![local](images/a.png)\r\n";
    QFile file(path);
    check(file.open(QIODevice::WriteOnly), "fixture opens");
    file.write(original);
    file.close();
    auto source = SourceFile::read(path, &error);
    check(source.has_value(), "source reads with format metadata");
    if (source)
    {
        check(SourceFile::encode(source->text, *source) == original, "unchanged source retains every byte");
        auto saved = SourceFile::write(*source, source->text + "more\n", files.filePath("recovery"));
        check(saved.success(), "checked source write succeeds");
        check(saved.bytes.startsWith(QByteArray::fromHex("efbbbf")) && saved.bytes.endsWith("more\r\n"),
            "encoding and CRLF persist");
        check(SourceFile::finishWrite(saved, &error), "completed write clears its recovery journal");
        const auto conflict = SourceFile::write(*source, "stale text", files.filePath("recovery"));
        check(
            conflict.status == SourceWriteStatus::Conflict, "stale editor cannot overwrite external changes");
        auto latest = SourceFile::read(path);
        check(latest && latest->bytes == saved.bytes, "conflict leaves external file unchanged");
    }
    const auto utf16 = SourceFile::decode(QByteArray::fromHex("fffe2d4e87650d000a00"));
    check(utf16
            && SourceFile::encode(utf16->text + QStringLiteral("新行\n"), *utf16)
                .startsWith(QByteArray::fromHex("fffe")),
        "UTF16 remains UTF16");
    {
        QTemporaryDir moved, collision;
        const auto fresh = SourceFile::read(path);
        const qint64 folder = database.ensureLinkedFolder(files.path(), "linked folder", 0, &error);
        bool skipped = false;
        const auto linked = database.linkNote(fresh->path, fresh->hash, "md", "linked", "<p>original</p>",
            "original", folder, &skipped, &error, &fresh->bytes);
        const QString collisionPath = collision.filePath(QFileInfo(path).fileName());
        check(QFile::copy(path, collisionPath), "collision target copies");
        const auto other = SourceFile::read(collisionPath);
        database.linkNote(other->path, other->hash, "md", "other", "<p>other</p>", "other", 0, &skipped,
            &error, &other->bytes);
        const auto baseline = database.note(linked);
        check(!database.updateNote(linked, "changed", "<p>changed</p>", "changed", &error,
                  QStringLiteral("重新关联"), baseline->contentHash, &other->bytes, other->path, "md"),
            "relink collision rolls back body and mapping together");
        check(database.note(linked)->html == baseline->html
                && database.linkedSource(linked)->sourcePath == fresh->path,
            "failed relink preserves prior content and source");
        const QString newPath = moved.filePath(QFileInfo(path).fileName());
        check(QFile::copy(path, newPath), "moved source fixture copies");
        int count = 0;
        check(store.relinkFolder(folder, moved.path(), &error, &count) && count == 1,
            "directory remap preserves note identity");
        check(database.linkedSource(linked)->sourcePath == QFileInfo(newPath).canonicalFilePath()
                && QFileInfo::exists(path),
            "directory remap changes references without moving files");
        check(database.softDeleteNote(linked, &error), "linked note can be removed from notebook");
        skipped = false;
        const auto repeat = database.linkNote(QFileInfo(newPath).canonicalFilePath(), QByteArray("new hash"),
            "md", "linked", "<p>new</p>", "new", folder, &skipped, &error);
        check(repeat == linked && skipped && !database.note(linked),
            "refresh never resurrects a removed linked note");
    }
    std::cout << (ok ? "PASS" : "FAIL") << ": workspace history, recovery and safe source writes\n";
    return ok ? 0 : 1;
}
