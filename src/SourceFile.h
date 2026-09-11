#pragma once
#include <QByteArray>
#include <QString>
#include <optional>

enum class TextEncoding
{
    Utf8,
    Utf8Bom,
    Utf16LE,
    Utf16BE,
    Gb18030
};
struct SourceSnapshot
{
    QString path;
    QString text;
    QByteArray bytes;
    QByteArray hash;
    TextEncoding encoding = TextEncoding::Utf8;
    QString newline = QStringLiteral("\n");
};
enum class SourceWriteStatus
{
    Saved,
    Unchanged,
    Conflict,
    Missing,
    Failed
};
struct SourceWriteResult
{
    SourceWriteStatus status = SourceWriteStatus::Failed;
    QString error;
    QByteArray bytes;
    QString journalPath;
    bool success() const
    {
        return status == SourceWriteStatus::Saved || status == SourceWriteStatus::Unchanged;
    }
};
namespace SourceFile
{
std::optional<SourceSnapshot> decode(const QByteArray& bytes, QString* error = nullptr);
std::optional<SourceSnapshot> read(const QString& path, QString* error = nullptr);
QByteArray encode(const QString& text, const SourceSnapshot& baseline, QString* error = nullptr);
SourceWriteResult write(
    const SourceSnapshot& baseline, const QString& text, const QString& recoveryDirectory);
bool finishWrite(const SourceWriteResult& result, QString* error = nullptr);
QString encodingName(TextEncoding encoding);
}
