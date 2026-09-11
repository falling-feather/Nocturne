#include "DocumentImporter.h"
#include "Database.h"
#include "WorkspaceStore.h"
#include "BackupManager.h"
#include "NocturneStyle.h"
#include "MathSupport.h"
#include "SourceFile.h"
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
#include <algorithm>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
QString decodeText(const QByteArray& data, bool* ok)
{
    const auto source = SourceFile::decode(data);
    *ok = source.has_value(); return source ? source->text : QString();
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

void resolveImages(QTextDocument& document, const QString& sourceDirectory)
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
        const QUrl url(image.format.name());
        if (!url.isRelative() && !url.isLocalFile()) continue;
        const QString source = url.isRelative()
            ? QDir(sourceDirectory).absoluteFilePath(url.toString()) : url.toLocalFile();
        if (!QFileInfo::exists(source)) continue;
        image.format.setName(QUrl::fromLocalFile(QFileInfo(source).absoluteFilePath()).toString());
        QTextCursor cursor(&document); cursor.setPosition(image.start);
        cursor.setPosition(image.start + image.length, QTextCursor::KeepAnchor);
        cursor.setCharFormat(image.format);
    }
}

QStringList recentDocumentationDirectories(const QString& root, int days)
{
    const QString canonicalRoot = QFileInfo(root).canonicalFilePath();
    if (canonicalRoot.isEmpty()) return {};
    const QDateTime cutoff = QDateTime::currentDateTime().addDays(-qMax(1, days));
    QStringList found;
    const auto projects = QDir(canonicalRoot).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Name);
    for (const QFileInfo& project : projects) {
        if (project.fileName().compare(QStringLiteral("笔记本"), Qt::CaseInsensitive) == 0)
            continue;
        const bool projectRecent = project.lastModified() >= cutoff;
        for (const QString& name : {QStringLiteral("doc"), QStringLiteral("docs")}) {
            const QFileInfo documentation(project.absoluteFilePath() + QDir::separator() + name);
            if (!documentation.isDir() || documentation.isSymLink()) continue;
            const auto entries = QDir(documentation.absoluteFilePath()).entryInfoList(
                QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Name);
            if (!entries.isEmpty() && (projectRecent || documentation.lastModified() >= cutoff))
                found.append(documentation.canonicalFilePath());
        }
    }
    found.removeDuplicates();
    std::sort(found.begin(), found.end(), [](const QString& a, const QString& b) { return a.compare(b, Qt::CaseInsensitive) < 0; });
    return found;
}

}
DocumentImporter::DocumentImporter(QStringList paths, qint64 parentFolder, QObject* parent,
    bool linkSources, QString rootNameOverride)
    : QThread(parent), m_paths(std::move(paths)), m_parentFolder(parentFolder),
      m_linkSources(linkSources), m_rootNameOverride(std::move(rootNameOverride)) {}

bool DocumentImporter::readDocument(const QString& sourcePath, QString* html,
    QString* plainText, QByteArray* sourceBytes, QString* error)
{
    if (html) html->clear();
    if (plainText) plainText->clear();
    if (sourceBytes) sourceBytes->clear();
    if (error) error->clear();
    const QFileInfo info(sourcePath);
    if (!info.isFile()) { if (error) *error = QStringLiteral("源文件不存在：%1").arg(sourcePath); return false; }
    if (info.size() > 8 * 1024 * 1024) { if (error) *error = QStringLiteral("超过单文件 8 MiB 上限"); return false; }
    QFile file(info.canonicalFilePath());
    if (!file.open(QIODevice::ReadOnly)) { if (error) *error = file.errorString(); return false; }
    const QByteArray bytes = file.readAll();
    if (file.error() != QFile::NoError) { if (error) *error = file.errorString(); return false; }
    bool decoded = false;
    const QString text = decodeText(bytes, &decoded);
    if (!decoded || text.contains(QChar::Null)) { if (error) *error = QStringLiteral("无法识别文本编码"); return false; }
    renderDocument(text, info.suffix().toLower(), info.absolutePath(), html, plainText);
    if (sourceBytes) *sourceBytes = bytes;
    return true;
}

void DocumentImporter::renderDocument(const QString& text, const QString& kind, const QString& directory,
                                      QString* html, QString* plainText)
{
    QTextDocument document;
    document.setDefaultFont(QFont(NocturneUi::sansFamily(), 12));
    if ((kind == "md" || kind == "markdown") && text.size() <= 512*1024) MathSupport::setMarkdown(document,text);
    else if (kind == "html" || kind == "htm") document.setHtml(MathSupport::normalizeHtml(text));
    else document.setPlainText(text);
    resolveImages(document,directory);
    if (html) *html=document.toHtml();
    if (plainText) *plainText=MathSupport::plainText(document);
}

void DocumentImporter::run()
{
    Database database; QString error;
    if (!database.open(&error)) { errors.append(error); return; }
    struct Pending { QString path; qint64 parent; int depth; QString scope; };
    QList<Pending> pending;
    for (const auto& path : m_paths) pending.append({path, m_rootParents.value(path,qMax<qint64>(0, m_parentFolder)), 0, QFileInfo(path).canonicalFilePath()});
    QSet<QString> visited;
    const QSet<QString> ignored = {".git", ".svn", "node_modules", ".venv", ".idea", ".vs",
        "vendor", "third_party", "resources", "assets", "images", "attachments",
        "backups", "release", "releases", "build", "dist"};
    while (!pending.isEmpty() && !isInterruptionRequested()) {
        const Pending task = pending.takeLast();
        const QFileInfo info(task.path);
        const QString canonical = info.canonicalFilePath();
        if (canonical.isEmpty() || info.isSymLink()) {
            if (task.depth == 0) errors.append(info.fileName() + QStringLiteral("：目录不存在或是符号链接，未刷新。"));
            ++skipped; continue;
        }
        if (canonical.compare(task.scope, Qt::CaseInsensitive) != 0
            && !canonical.startsWith(task.scope.endsWith('/') ? task.scope : task.scope + '/', Qt::CaseInsensitive)) {
            errors.append(info.fileName() + QStringLiteral("：已超出指定目录，未导入。")); continue;
        }
        if (visited.contains(canonical.toCaseFolded())) { ++skipped; continue; }
        visited.insert(canonical.toCaseFolded());
        if (info.isDir()) {
            if (task.depth > 48) { errors.append(info.fileName() + QStringLiteral("：目录层级超过 48 层")); continue; }
            if (ignored.contains(info.fileName())) { ++skipped; continue; }
            const QString folderName = task.depth == 0 && !m_rootNameOverride.isEmpty()
                ? m_rootNameOverride
                : (info.fileName().isEmpty() ? QStringLiteral("导入文档") : info.fileName());
            const qint64 folder = m_linkSources
                ? database.ensureLinkedFolder(canonical, folderName, task.parent, &error)
                : database.ensureImportedFolder(canonical, folderName, task.parent, &error);
            if (!folder) { errors.append(info.fileName() + "：" + error); continue; }
            if (m_linkSources && m_registerScanRoots && task.depth == 0
                && !WorkspaceStore(database).setRefreshRoot(canonical, true, &error)) {
                errors.append(error); continue;
            }
            const auto children = QDir(canonical).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
            for (auto it = children.crbegin(); it != children.crend(); ++it) {
                if (it->isDir() && ignored.contains(it->fileName())) continue;
                pending.append({it->absoluteFilePath(), folder, task.depth + 1, task.scope});
            }
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
        if ((extension == "md" || extension == "markdown")
            && !(m_linkSources && bytes.size() > 512 * 1024))
            MathSupport::setMarkdown(document, text);
        else if (extension == "html" || extension == "htm") document.setHtml(MathSupport::normalizeHtml(text));
        else document.setPlainText(text);
        if (m_linkSources)
            resolveImages(document, info.absolutePath());
        else
            copyImages(document, info.absolutePath(), QDir(database.dataDirectory()).filePath("attachments"));
        bool duplicate = false;
        const QByteArray hash = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
        const qint64 id = m_linkSources
            ? database.linkNote(canonical, hash, extension, info.completeBaseName(), document.toHtml(), MathSupport::plainText(document), task.parent, &duplicate, &error,&bytes)
            : database.importNote(canonical, hash, info.completeBaseName(), document.toHtml(), MathSupport::plainText(document), task.parent, &duplicate, &error);
        if (!id) errors.append(info.fileName() + "：" + error);
        else if (duplicate) ++skipped;
        else { ++imported; lastNoteId = id; }
        emit progress(imported, skipped, info.fileName());
    }
}

bool DocumentImporter::linkRecentProjectDocs(const QString& workspaceRoot, int days, QString* report)
{
    Database database; QString error;
    if (!database.open(&error)) { if (report) *report = error; return false; }
    const BackupResult backup = BackupManager(database.dataDirectory()).create(BackupKind::Manual);
    if (!backup.success) {
        if (report) *report = QStringLiteral("对接前备份失败：") + backup.error;
        return false;
    }
    const QString canonicalRoot = QFileInfo(workspaceRoot).canonicalFilePath();
    const QStringList documentation = recentDocumentationDirectories(canonicalRoot, days);
    qint64 rootFolder = database.ensureLinkedFolder(canonicalRoot, QStringLiteral("代码玩具测试"), 0, &error);
    if (!rootFolder) { if (report) *report = error; return false; }
    int imported = 0, skipped = 0;
    QStringList errors;
    for (const QString& directory : documentation) {
        const QString relative = QDir(canonicalRoot).relativeFilePath(directory);
        const QStringList parts = relative.split(QRegularExpression(QStringLiteral("[/\\\\]")), Qt::SkipEmptyParts);
        const QString project = parts.isEmpty() ? QStringLiteral("根目录") : parts.first();
        const QString projectPath = QDir(canonicalRoot).filePath(project);
        const qint64 projectFolder = database.ensureLinkedFolder(
            QFileInfo(projectPath).canonicalFilePath(), project, rootFolder, &error);
        if (!projectFolder) { errors.append(project + QStringLiteral("：") + error); continue; }
        DocumentImporter importer({directory}, projectFolder, nullptr, true, QStringLiteral("详细md文档"));
        importer.runNow();
        imported += importer.imported; skipped += importer.skipped; errors.append(importer.errors);
    }
    if (report) {
        *report = QStringLiteral("发现 %1 个文档目录，新增或更新 %2 篇，跳过 %3 篇。")
            .arg(documentation.size()).arg(imported).arg(skipped);
        if (!errors.isEmpty()) *report += QStringLiteral("\n") + errors.join(QStringLiteral("\n"));
    }
    return errors.isEmpty();
}
