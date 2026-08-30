#include "NoteEditor.h"

#include <QFileInfo>
#include <QMimeData>
#include <QPixmap>
#include <QUrl>
#include <QVariant>

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
