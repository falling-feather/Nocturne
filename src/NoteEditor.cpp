#include "NoteEditor.h"

#include <QFileInfo>
#include <QImageReader>
#include <QMimeData>
#include <QPixmap>
#include <QTextDocument>
#include <QUrl>
#include <QVariant>

#include <algorithm>

namespace {
bool isImageFile(const QString& path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    return suffix == QStringLiteral("png") || suffix == QStringLiteral("jpg")
        || suffix == QStringLiteral("jpeg") || suffix == QStringLiteral("bmp")
        || suffix == QStringLiteral("gif") || suffix == QStringLiteral("webp");
}
}

NoteEditor::NoteEditor(QWidget* parent)
    : QTextEdit(parent)
{
    setAcceptRichText(true);
    setAcceptDrops(true);
    setUndoRedoEnabled(true);
    setPlaceholderText(QStringLiteral("记录想法，或把概念图拖到这里…"));

    document()->setResourceProvider([this](const QUrl& url) -> QVariant {
        if (!url.isLocalFile())
            return {};

        QImageReader reader(url.toLocalFile());
        reader.setAutoTransform(true);
        const QSize sourceSize = reader.size();
        const int targetWidth = std::max(320, viewport()->width() - 48);
        const QSize decodeBounds(targetWidth,
                                 std::max(targetWidth, viewport()->height() * 2));
        if (sourceSize.isValid()
            && (sourceSize.width() > decodeBounds.width()
                || sourceSize.height() > decodeBounds.height())) {
            reader.setScaledSize(sourceSize.scaled(decodeBounds, Qt::KeepAspectRatio));
        }
        const QImage image = reader.read();
        return image.isNull() ? QVariant() : QVariant::fromValue(image);
    });
}

bool NoteEditor::canInsertFromMimeData(const QMimeData* source) const
{
    if (source->hasImage())
        return true;

    if (source->hasUrls()) {
        for (const QUrl& url : source->urls()) {
            if (url.isLocalFile() && isImageFile(url.toLocalFile()))
                return true;
        }
    }
    return QTextEdit::canInsertFromMimeData(source);
}

void NoteEditor::insertFromMimeData(const QMimeData* source)
{
    if (source->hasImage()) {
        QImage image;
        const QVariant payload = source->imageData();
        if (payload.canConvert<QImage>())
            image = qvariant_cast<QImage>(payload);
        else if (payload.canConvert<QPixmap>())
            image = qvariant_cast<QPixmap>(payload).toImage();

        if (!image.isNull()) {
            emit imagePasted(image);
            return;
        }
    }

    if (source->hasUrls()) {
        QStringList images;
        for (const QUrl& url : source->urls()) {
            if (url.isLocalFile() && isImageFile(url.toLocalFile()))
                images.push_back(url.toLocalFile());
        }
        if (!images.isEmpty()) {
            emit imageFilesDropped(images);
            return;
        }
    }

    QTextEdit::insertFromMimeData(source);
}
