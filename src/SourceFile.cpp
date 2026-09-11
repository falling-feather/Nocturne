#include "SourceFile.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStringDecoder>
#include <QDateTime>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace
{
constexpr qint64 maximumBytes = 8 * 1024 * 1024;
QByteArray digest(const QByteArray& bytes)
{
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
}
QString normalized(QString text)
{
    return text.replace("\r\n", "\n").replace('\r', '\n');
}
void fail(QString* error, const QString& message)
{
    if (error)
        *error = message;
}
#ifdef Q_OS_WIN
struct FileHandle
{
    HANDLE value = INVALID_HANDLE_VALUE;
    ~FileHandle()
    {
        if (value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
};
bool writeBytes(HANDLE handle, const QByteArray& bytes)
{
    LARGE_INTEGER zero {};
    if (!SetFilePointerEx(handle, zero, nullptr, FILE_BEGIN))
        return false;
    qsizetype offset = 0;
    while (offset < bytes.size())
    {
        DWORD written = 0;
        if (!WriteFile(handle, bytes.constData() + offset, DWORD(bytes.size() - offset), &written, nullptr)
            || written == 0)
            return false;
        offset += written;
    }
    return SetEndOfFile(handle) && FlushFileBuffers(handle);
}
#endif
}

namespace SourceFile
{
std::optional<SourceSnapshot> decode(const QByteArray& bytes, QString* error)
{
    if (error)
        error->clear();
    if (bytes.size() > maximumBytes)
    {
        fail(error, QStringLiteral("文档超过 8 MiB 上限。"));
        return {};
    }
    SourceSnapshot result;
    result.bytes = bytes;
    result.hash = digest(bytes);
    QByteArray content = bytes;
    QString text;
    if (bytes.startsWith(QByteArray::fromHex("fffe")) || bytes.startsWith(QByteArray::fromHex("feff")))
    {
        const bool little = bytes.startsWith(QByteArray::fromHex("fffe"));
        result.encoding = little ? TextEncoding::Utf16LE : TextEncoding::Utf16BE;
        QStringDecoder decoder(little ? QStringDecoder::Utf16LE : QStringDecoder::Utf16BE);
        text = decoder(bytes.mid(2));
        if (decoder.hasError())
        {
            fail(error, QStringLiteral("UTF-16 编码不完整。"));
            return {};
        }
    }
    else
    {
        if (bytes.startsWith(QByteArray::fromHex("efbbbf")))
        {
            result.encoding = TextEncoding::Utf8Bom;
            content = bytes.mid(3);
        }
        QStringDecoder decoder(QStringDecoder::Utf8);
        text = decoder(content);
        if (decoder.hasError())
        {
#ifdef Q_OS_WIN
            const int count = MultiByteToWideChar(
                54936, MB_ERR_INVALID_CHARS, bytes.constData(), int(bytes.size()), nullptr, 0);
            if (count <= 0)
            {
                fail(error, QStringLiteral("无法识别文档编码。"));
                return {};
            }
            text.resize(count);
            MultiByteToWideChar(54936, MB_ERR_INVALID_CHARS, bytes.constData(), int(bytes.size()),
                reinterpret_cast<wchar_t*>(text.data()), count);
            result.encoding = TextEncoding::Gb18030;
#else
            fail(error, QStringLiteral("无法识别文档编码。"));
            return {};
#endif
        }
    }
    if (text.contains(QChar::Null))
    {
        fail(error, QStringLiteral("文件包含二进制内容。"));
        return {};
    }
    if (text.contains("\r\n"))
        result.newline = "\r\n";
    else if (text.contains('\r'))
        result.newline = "\r";
    result.text = normalized(text);
    return result;
}
std::optional<SourceSnapshot> read(const QString& path, QString* error)
{
    if (error)
        error->clear();
    const QFileInfo info(path);
    if (!info.isFile())
    {
        fail(error, QStringLiteral("源文件不存在：%1").arg(path));
        return {};
    }
    if (info.size() > maximumBytes)
    {
        fail(error, QStringLiteral("文档超过 8 MiB 上限。"));
        return {};
    }
    QFile file(info.canonicalFilePath());
    if (!file.open(QIODevice::ReadOnly))
    {
        fail(error, file.errorString());
        return {};
    }
    const auto bytes = file.read(maximumBytes + 1);
    if (file.error() != QFile::NoError)
    {
        fail(error, file.errorString());
        return {};
    }
    auto result = decode(bytes, error);
    if (result)
        result->path = info.canonicalFilePath();
    return result;
}
QByteArray encode(const QString& text, const SourceSnapshot& baseline, QString* error)
{
    if (error)
        error->clear();
    if (normalized(text) == baseline.text)
        return baseline.bytes;
    QString converted = normalized(text);
    converted.replace("\n", baseline.newline);
    QByteArray bytes;
    switch (baseline.encoding)
    {
    case TextEncoding::Utf8:
        bytes = converted.toUtf8();
        break;
    case TextEncoding::Utf8Bom:
        bytes = QByteArray::fromHex("efbbbf") + converted.toUtf8();
        break;
    case TextEncoding::Utf16LE:
    case TextEncoding::Utf16BE:
    {
        const bool little = baseline.encoding == TextEncoding::Utf16LE;
        bytes = QByteArray::fromHex(little ? "fffe" : "feff");
        for (QChar character : converted)
        {
            const ushort unit = character.unicode();
            bytes.append(char(little ? unit & 255 : unit >> 8));
            bytes.append(char(little ? unit >> 8 : unit & 255));
        }
        break;
    }
    case TextEncoding::Gb18030:
#ifdef Q_OS_WIN
    {
        const int count = WideCharToMultiByte(54936, 0, reinterpret_cast<const wchar_t*>(converted.utf16()),
            int(converted.size()), nullptr, 0, nullptr, nullptr);
        if (count <= 0 && !converted.isEmpty())
        {
            fail(error, QStringLiteral("无法按原 GB18030 编码保存。"));
            return {};
        }
        bytes.resize(count);
        WideCharToMultiByte(54936, 0, reinterpret_cast<const wchar_t*>(converted.utf16()),
            int(converted.size()), bytes.data(), count, nullptr, nullptr);
    }
#else
        fail(error, QStringLiteral("当前平台不支持写入 GB18030。"));
        return {};
#endif
    break;
    }
    if (bytes.size() > maximumBytes)
    {
        fail(error, QStringLiteral("保存内容超过 8 MiB 上限。"));
        return {};
    }
    return bytes;
}
SourceWriteResult write(const SourceSnapshot& baseline, const QString& text, const QString& recoveryDirectory)
{
    SourceWriteResult result;
    result.bytes = encode(text, baseline, &result.error);
    if (!result.error.isEmpty())
        return result;
    if (!QFileInfo::exists(baseline.path))
    {
        result.status = SourceWriteStatus::Missing;
        result.error = QStringLiteral("源文件已被移动或删除；编辑内容仍保留在夜航。");
        return result;
    }
    QByteArray current;
#ifdef Q_OS_WIN
    // Exclusive access prevents writes, replacement, and reads of partial data.
    FileHandle handle;
    handle.value = CreateFileW(reinterpret_cast<const wchar_t*>(baseline.path.utf16()),
        GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle.value == INVALID_HANDLE_VALUE)
    {
        result.error = QStringLiteral("源文件只读、被占用或无法写入（Windows %1）。").arg(GetLastError());
        return result;
    }
    LARGE_INTEGER size {};
    if (!GetFileSizeEx(handle.value, &size) || size.QuadPart > maximumBytes)
    {
        result.error = QStringLiteral("无法安全读取当前源文件。");
        return result;
    }
    current.resize(size.QuadPart);
    DWORD count = 0;
    if (!ReadFile(handle.value, current.data(), DWORD(current.size()), &count, nullptr)
        || count != current.size())
    {
        result.error = QStringLiteral("源文件读取不完整，未进行写入。");
        return result;
    }
#else
    QString error;
    const auto source = read(baseline.path, &error);
    if (!source)
    {
        result.error = error;
        return result;
    }
    current = source->bytes;
#endif
    if (current != result.bytes && digest(current) != baseline.hash)
    {
        result.status = SourceWriteStatus::Conflict;
        result.error = QStringLiteral("源文件已被其他编辑器修改。请比较双方版本后再保存；没有覆盖外部内容。");
        return result;
    }
    const QString journalName = QString::fromLatin1(digest(baseline.path.toUtf8()).toHex()) + ".json";
    const QString journalPath = QDir(recoveryDirectory).filePath(journalName);
    if (current == result.bytes)
    {
        result.status = SourceWriteStatus::Unchanged;
        if (QFileInfo::exists(journalPath))
            result.journalPath = journalPath;
        return result;
    }
    if (!QDir().mkpath(recoveryDirectory))
    {
        result.error = QStringLiteral("无法建立写入恢复记录，未修改源文件。");
        return result;
    }
    const QJsonObject journal { { "format", 1 }, { "path", baseline.path },
        { "createdAt", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs) },
        { "before", QString::fromLatin1(qCompress(current).toBase64()) },
        { "after", QString::fromLatin1(qCompress(result.bytes).toBase64()) },
        { "afterHash", QString::fromLatin1(digest(result.bytes).toHex()) } };
    QSaveFile recovery(journalPath);
    const auto payload = QJsonDocument(journal).toJson(QJsonDocument::Compact);
    if (!recovery.open(QIODevice::WriteOnly) || recovery.write(payload) != payload.size()
        || !recovery.commit())
    {
        result.error = QStringLiteral("无法保存写入恢复记录，未修改源文件。");
        return result;
    }
    result.journalPath = journalPath;
#ifdef Q_OS_WIN
    if (!writeBytes(handle.value, result.bytes))
    {
        const bool rolledBack = writeBytes(handle.value, current);
        result.error = rolledBack
            ? QStringLiteral("源文件写入失败，已还原原内容，双方内容保留在恢复记录中。")
            : QStringLiteral("源文件写入失败，原文与新稿已保留在恢复记录中，请从恢复中心处理。");
        return result;
    }
#else
    QSaveFile file(baseline.path);
    if (!file.open(QIODevice::WriteOnly) || file.write(result.bytes) != result.bytes.size() || !file.commit())
    {
        result.error = file.errorString();
        return result;
    }
#endif
    result.status = SourceWriteStatus::Saved;
    return result;
}
bool finishWrite(const SourceWriteResult& result, QString* error)
{
    if (error)
        error->clear();
    if (result.journalPath.isEmpty())
        return true;
    QFile file(result.journalPath);
    if (!file.exists())
        return true;
    if (!file.open(QIODevice::ReadOnly))
    {
        fail(error, file.errorString());
        return false;
    }
    const auto object = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    if (object.value("afterHash").toString() != QString::fromLatin1(digest(result.bytes).toHex()))
    {
        fail(error, QStringLiteral("写入恢复记录已变化，保留以供检查。"));
        return false;
    }
    if (!file.remove())
    {
        fail(error, file.errorString());
        return false;
    }
    return true;
}
QString encodingName(TextEncoding encoding)
{
    switch (encoding)
    {
    case TextEncoding::Utf8:
        return "UTF-8";
    case TextEncoding::Utf8Bom:
        return "UTF-8 BOM";
    case TextEncoding::Utf16LE:
        return "UTF-16 LE";
    case TextEncoding::Utf16BE:
        return "UTF-16 BE";
    case TextEncoding::Gb18030:
        return "GB18030";
    }
    return {};
}
}
