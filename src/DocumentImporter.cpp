#include "DocumentImporter.h"
#include "Database.h"
#include "NocturneStyle.h"
#include <QFont>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QStringDecoder>
#include <QTextDocument>
#include <QTextBlock>
#include <QTextFragment>
#include <QTextCursor>
#include <QTextImageFormat>
#include <QUrl>
#include <QSet>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
QString decodeText(const QByteArray& data, bool* ok)
{
    *ok = true;
    if (data.startsWith("\xff\xfe") || data.startsWith("\xfe\xff")) {
        QStringDecoder decoder(QStringDecoder::Utf16);
        const QString text = decoder(data);
        *ok = !decoder.hasError(); return text;
    }
    QStringDecoder decoder(QStringDecoder::Utf8);
    QString text = decoder(data);
    if (!decoder.hasError()) return text;
#ifdef Q_OS_WIN
    const int length = MultiByteToWideChar(54936, MB_ERR_INVALID_CHARS, data.constData(), data.size(), nullptr, 0);
    if (length > 0) {
        text.resize(length);
        MultiByteToWideChar(54936, MB_ERR_INVALID_CHARS, data.constData(), data.size(),
                            reinterpret_cast<wchar_t*>(text.data()), length);
        return text;
    }
#endif
    *ok = false; return {};
}

void copyImages(QTextDocument& document, const QString& sourceDirectory, const QString& attachments)
{
    struct ImageRun { int start; int length; QTextImageFormat format; };
    QList<ImageRun> images;
    for (auto block = document.begin(); block.isValid(); block = block.next())
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const auto fragment = it.fragment();
            if (fragment.isValid() && fragment.charFormat().isImageFormat())
                images.append({fragment.position(), fragment.length(), fragment.charFormat().toImageFormat()});
        }
    for (auto& image : images) {
        QUrl url(image.format.name());
        if (!url.isRelative() && !url.isLocalFile()) continue;
        const QString source = url.isRelative() ? QDir(sourceDirectory).absoluteFilePath(url.toString()) : url.toLocalFile();
        const QFileInfo info(source);
        if (!info.isFile() || info.size() > 20 * 1024 * 1024) continue;
        QFile file(source);
        if (!file.open(QIODevice::ReadOnly)) continue;
        const auto bytes = file.readAll();
        const QString hash = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
        const QString destination = QDir(attachments).filePath(hash + "." + info.suffix().toLower());
        if (!QFileInfo::exists(destination)) {
            QDir().mkpath(attachments);
            if (!QFile::copy(source, destination)) continue;
        }
        image.format.setName(QUrl::fromLocalFile(destination).toString());
        QTextCursor cursor(&document); cursor.setPosition(image.start);
        cursor.setPosition(image.start + image.length, QTextCursor::KeepAnchor);
        cursor.setCharFormat(image.format);
    }
}
}
DocumentImporter::DocumentImporter(QStringList paths, qint64 parentFolder, QObject* parent)
    : QThread(parent), m_paths(std::move(paths)), m_parentFolder(parentFolder) {}

void DocumentImporter::run()
{
    Database database; QString error;
    if (!database.open(&error)) { errors.append(error); return; }
    struct Pending { QString path; qint64 parent; int depth; };
    QList<Pending> pending;
    for (const auto& path : m_paths) pending.append({path, qMax<qint64>(0, m_parentFolder), 0});
    QSet<QString> visited;
    const QSet<QString> ignored = {".git", ".svn", "node_modules", ".venv", ".idea", ".vs"};
    while (!pending.isEmpty() && !isInterruptionRequested()) {
        const Pending task = pending.takeLast();
        const QFileInfo info(task.path);
        const QString canonical = info.canonicalFilePath();
        if (canonical.isEmpty() || info.isSymLink()) { ++skipped; continue; }
        if (visited.contains(canonical.toCaseFolded())) { ++skipped; continue; }
        visited.insert(canonical.toCaseFolded());
        if (info.isDir()) {
            if (task.depth > 48) { errors.append(info.fileName() + QStringLiteral("：目录层级超过 48 层")); continue; }
            if (ignored.contains(info.fileName())) { ++skipped; continue; }
            const qint64 folder = database.ensureImportedFolder(canonical,
                info.fileName().isEmpty() ? QStringLiteral("导入文档") : info.fileName(), task.parent, &error);
            if (!folder) { errors.append(info.fileName() + "：" + error); continue; }
            const auto children = QDir(canonical).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
            for (auto it = children.crbegin(); it != children.crend(); ++it)
                pending.append({it->absoluteFilePath(), folder, task.depth + 1});
            emit progress(imported, skipped, info.fileName());
            continue;
        }
        const QString extension = info.suffix().toLower();
        if (!QStringList{"md", "markdown", "txt", "html", "htm"}.contains(extension)) { ++skipped; continue; }
        if (info.size() > 8 * 1024 * 1024) { errors.append(info.fileName() + QStringLiteral("：超过单文件 8 MiB 上限")); continue; }
        QFile file(canonical);
        if (!file.open(QIODevice::ReadOnly)) { errors.append(info.fileName() + "：" + file.errorString()); continue; }
        const QByteArray bytes = file.readAll();
        if (file.error() != QFile::NoError) { errors.append(info.fileName() + "：" + file.errorString()); continue; }
        bool decoded = false; const QString text = decodeText(bytes, &decoded);
        if (!decoded || text.contains(QChar::Null)) { errors.append(info.fileName() + QStringLiteral("：无法识别文本编码")); continue; }
        QTextDocument document;
        document.setDefaultFont(QFont(NocturneUi::sansFamily(), 12));
        if (extension == "md" || extension == "markdown")
            document.setMarkdown(text, QTextDocument::MarkdownDialectGitHub);
        else if (extension == "html" || extension == "htm") document.setHtml(text);
        else document.setPlainText(text);
        copyImages(document, info.absolutePath(), QDir(database.dataDirectory()).filePath("attachments"));
        bool duplicate = false;
        const qint64 id = database.importNote(canonical, QCryptographicHash::hash(bytes, QCryptographicHash::Sha256),
            info.completeBaseName(), document.toHtml(), document.toPlainText(), task.parent, &duplicate, &error);
        if (!id) errors.append(info.fileName() + "：" + error);
        else if (duplicate) ++skipped;
        else { ++imported; lastNoteId = id; }
        emit progress(imported, skipped, info.fileName());
    }
}
