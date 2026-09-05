#include "NoteEditor.h"
#include "NocturneStyle.h"

#include <QFileInfo>
#include <QImageReader>
#include <QMimeData>
#include <QPixmap>
#include <QTextDocument>
#include <QUrl>
#include <QVariant>
#include <QSyntaxHighlighter>
#include <QTextBlock>
#include <QTextFragment>

#include <algorithm>

namespace {
// Presentation-only contrast correction. QTextLayout overlays leave the saved
// rich text, explicit author colors, cursor, and undo stack untouched.
class ContrastHighlighter final : public QSyntaxHighlighter
{
public:
    explicit ContrastHighlighter(QTextDocument* document) : QSyntaxHighlighter(document) {}
protected:
    void highlightBlock(const QString&) override
    {
        const auto& theme = NocturneUi::theme();
        const bool dark = theme.paper.lightnessF() < 0.5;
        const QTextBlock block = currentBlock();
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (!fragment.isValid()) continue;
            const auto format = fragment.charFormat();
            // Explicit foreground/background pairs retain their authored colors.
            if (format.background().style() != Qt::NoBrush) continue;
            const QColor color = format.foreground().color();
            if (format.foreground().style() != Qt::NoBrush
                && ((dark && color.lightnessF() < 0.40)
                    || (!dark && color.lightnessF() > 0.72))) {
                setFormat(fragment.position() - block.position(), fragment.length(), theme.text);
            }
        }
    }
};

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
    setPlaceholderText(QStringLiteral("让此刻的念头，在这里靠岸…"));
    document()->setDocumentMargin(0);
    document()->setDefaultStyleSheet(QStringLiteral(
        "p { margin-top: 10px; margin-bottom: 12px; line-height: 160%; }"
        "h1, h2, h3 { margin-top: 24px; margin-bottom: 14px; }"
        "li { margin-top: 6px; margin-bottom: 6px; line-height: 150%; }"));
    m_contrastHighlighter = new ContrastHighlighter(document());

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

void NoteEditor::refreshTheme()
{
    QPalette colors = palette();
    colors.setColor(QPalette::Text, NocturneUi::theme().text);
    colors.setColor(QPalette::Base, NocturneUi::theme().paper);
    colors.setColor(QPalette::PlaceholderText, NocturneUi::theme().muted);
    setPalette(colors);
    m_contrastHighlighter->rehighlight();
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
