#include "NoteEditor.h"
#include "NocturneStyle.h"
#include "NocturneDialogs.h"
#include <QAbstractTextDocumentLayout>
#include <QPainter>
#include <QTextTable>
#include <QFormLayout>
#include <QSpinBox>
#include <QDialogButtonBox>
#include <QResizeEvent>
#include <QTimer>
#include <QScrollArea>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

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
#include <QTextDocumentFragment>
#include <QTextList>
#include <QKeyEvent>
#include <QMenu>
#include <QContextMenuEvent>
#include <QRegularExpression>
#include <QMouseEvent>
#include <QApplication>

#include <algorithm>
#include <memory>

namespace {
// Presentation-only contrast correction. QTextLayout overlays leave the saved
// rich text, explicit author colors, cursor, and undo stack untouched.
class ContrastHighlighter final : public QSyntaxHighlighter
{
public:
    explicit ContrastHighlighter(QTextDocument* document) : QSyntaxHighlighter(document) {}
    QHash<QString, bool> todoStates;
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
            const QString href = format.anchorHref();
            if (href.startsWith(QStringLiteral("nocturne-todo:"))
                && todoStates.contains(href.mid(14))) {
                QTextCharFormat marker;
                marker.setForeground(QColor("#AEBAC7"));
                marker.setBackground(QColor("#101924"));
                marker.setFontUnderline(true);
                marker.setFontStrikeOut(todoStates.value(href.mid(14)));
                setFormat(fragment.position() - block.position(), fragment.length(), marker);
                continue;
            }
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
    setMouseTracking(true);
    viewport()->setMouseTracking(true);
    setPlaceholderText(QStringLiteral("让此刻的念头，在这里靠岸…"));
    document()->setDocumentMargin(0);
    document()->setDefaultStyleSheet(QStringLiteral(
        "p { margin-top: 10px; margin-bottom: 12px; line-height: 160%; }"
        "h1, h2, h3 { margin-top: 24px; margin-bottom: 14px; }"
        "li { margin-top: 6px; margin-bottom: 6px; line-height: 150%; }"));
    m_contrastHighlighter = new ContrastHighlighter(document());
    document()->documentLayout()->registerHandler(QTextFormat::ImageObject, this);
    m_imageClickTimer = new QTimer(this);
    m_imageClickTimer->setSingleShot(true);
    connect(m_imageClickTimer, &QTimer::timeout, this, [this] {
        if (isReadOnly() || m_pendingImage.isNull() || !m_pendingImage.charFormat().isImageFormat()) return;
        bool accepted = false;
        const QString caption = NocturneDialogs::getText(this, QStringLiteral("图片描述"),
            QStringLiteral("显示在图片下方，可留空："), QLineEdit::Normal,
            m_pendingImage.charFormat().stringProperty(QTextFormat::ImageAltText), &accepted);
        if (accepted) setImageCaption(m_pendingImage.selectionStart(), caption);
    });

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
            const QFileInfo info(url.toLocalFile());
            if (url.isLocalFile() && (isImageFile(url.toLocalFile()) || info.isDir()
                || QStringList{"md", "markdown", "txt", "html", "htm"}.contains(info.suffix().toLower())))
                return true;
        }
    }
    return QTextEdit::canInsertFromMimeData(source);
}

void NoteEditor::insertFromMimeData(const QMimeData* source)
{
    if (source->hasUrls()) {
        QStringList documents;
        for (const auto& url : source->urls()) {
            const QFileInfo info(url.toLocalFile());
            if (url.isLocalFile() && (info.isDir() || QStringList{"md", "markdown", "txt", "html", "htm"}.contains(info.suffix().toLower())))
                documents.append(info.absoluteFilePath());
        }
        if (!documents.isEmpty()) { emit filesImportRequested(documents); return; }
    }
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

    if (source->hasText() && !source->hasHtml()
        && (QRegularExpression(QStringLiteral("(?m)^(#{1,6} |```|> |[-*] |[0-9]+\\. )")).match(source->text()).hasMatch()
            || QRegularExpression(QStringLiteral("(?m)^\\s*\\|?\\s*:?-{3,}:?\\s*\\|.*$")).match(source->text()).hasMatch())) {
        insertMarkdownText(source->text());
        return;
    }
    QTextEdit::insertFromMimeData(source);
    repairImageBlocks();
}

void NoteEditor::applyHeadingLevel(int level)
{
    if (isReadOnly()) return;
    level = std::clamp(level, 0, 6);
    QTextCursor selection = textCursor();
    const int end = selection.hasSelection() ? selection.selectionEnd() - 1 : selection.position();
    QTextCursor cursor(document());
    cursor.setPosition(selection.selectionStart());
    cursor.beginEditBlock();
    const int sizes[] = {12, 25, 21, 18, 16, 14, 12};
    do {
        QTextBlockFormat block = cursor.blockFormat();
        block.setHeadingLevel(level);
        block.clearProperty(QTextFormat::BlockQuoteLevel);
        block.clearProperty(QTextFormat::BlockCodeLanguage);
        block.clearProperty(QTextFormat::BlockCodeFence);
        block.setNonBreakableLines(false);
        block.setLeftMargin(0);
        cursor.setBlockFormat(block);
        QTextCursor line(cursor); line.select(QTextCursor::BlockUnderCursor);
        QTextCharFormat format;
        format.setFontPointSize(sizes[level]);
        format.setFontWeight(level ? QFont::DemiBold : QFont::Normal);
        format.setFontFamilies({level ? NocturneUi::serifFamily() : NocturneUi::sansFamily()});
        line.mergeCharFormat(format);
        if (cursor.block().position() + cursor.block().length() > end) break;
    } while (cursor.movePosition(QTextCursor::NextBlock));
    cursor.endEditBlock();
    setTextCursor(selection); setFocus();
}
void NoteEditor::applyQuote()
{
    if (isReadOnly()) return;
    auto cursor = textCursor(); cursor.beginEditBlock();
    auto block = cursor.blockFormat();
    block.setHeadingLevel(0);
    block.clearProperty(QTextFormat::BlockCodeLanguage);
    block.clearProperty(QTextFormat::BlockCodeFence);
    block.setProperty(QTextFormat::BlockQuoteLevel, 1);
    block.setLeftMargin(24);
    cursor.mergeBlockFormat(block); cursor.endEditBlock();
    setTextCursor(cursor); setFocus();
}
void NoteEditor::applyCodeBlock()
{
    if (isReadOnly()) return;
    auto cursor = textCursor(); cursor.beginEditBlock();
    auto block = cursor.blockFormat();
    block.setHeadingLevel(0);
    block.setProperty(QTextFormat::BlockCodeLanguage, QStringLiteral("text"));
    block.clearProperty(QTextFormat::BlockQuoteLevel);
    block.setProperty(QTextFormat::BlockCodeFence, QStringLiteral("```"));
    block.setNonBreakableLines(true);
    cursor.mergeBlockFormat(block);
    QTextCharFormat format; format.setFontFamilies({QStringLiteral("Consolas")});
    format.setFontPointSize(11); cursor.mergeCharFormat(format);
    cursor.endEditBlock(); setTextCursor(cursor); setFocus();
}
void NoteEditor::insertMarkdownText(const QString& markdown)
{
    if (isReadOnly()) return;
    auto cursor = textCursor();
    cursor.beginEditBlock();
    cursor.insertFragment(QTextDocumentFragment::fromMarkdown(markdown, QTextDocument::MarkdownDialectGitHub));
    repairImageBlocks();
    cursor.endEditBlock(); setTextCursor(cursor);
}
QString NoteEditor::markSelectionTodo(const QString& anchor)
{
    auto cursor = textCursor();
    if (isReadOnly() || !cursor.hasSelection()) return {};
    const QString selected = cursor.selectedText().replace(QChar::ParagraphSeparator, QChar('\n'));
    cursor.beginEditBlock();
    QTextCharFormat format;
    format.setAnchor(true); format.setAnchorHref(QStringLiteral("nocturne-todo:") + anchor);
    format.setFontUnderline(false);
    cursor.mergeCharFormat(format);
    cursor.endEditBlock();
    setTextCursor(cursor);
    return selected;
}
QString NoteEditor::markdownForExport() const
{
    std::unique_ptr<QTextDocument> copy(document()->clone());
    QList<QPair<int, int>> ranges;
    for (auto block = copy->begin(); block.isValid(); block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const auto fragment = it.fragment();
            if (fragment.isValid() && (fragment.charFormat().anchorHref().startsWith(QStringLiteral("nocturne-todo:"))
                || fragment.charFormat().anchorHref() == QStringLiteral("nocturne-caption:")))
                ranges.append({fragment.position(), fragment.length()});
        }
    }
    for (const auto& range : ranges) {
        QTextCursor cursor(copy.get()); cursor.setPosition(range.first);
        cursor.setPosition(range.first + range.second, QTextCursor::KeepAnchor);
        QTextCharFormat clear; clear.setAnchor(false); clear.setAnchorHref(QString()); clear.setFontUnderline(false);
        cursor.mergeCharFormat(clear);
    }
    return copy->toMarkdown(QTextDocument::MarkdownDialectGitHub);
}
void NoteEditor::setTodoStates(const QHash<QString, bool>& states)
{
    auto* highlighter = static_cast<ContrastHighlighter*>(m_contrastHighlighter);
    if (highlighter->todoStates == states) return;
    highlighter->todoStates = states;
    highlighter->rehighlight();
}
bool NoteEditor::locateTodo(const QString& anchor)
{
    const QString href = QStringLiteral("nocturne-todo:") + anchor;
    int start = -1, end = -1;
    for (QTextBlock block = document()->begin(); block.isValid(); block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const auto fragment = it.fragment();
            if (fragment.isValid() && fragment.charFormat().anchorHref() == href) {
                if (start < 0) start = fragment.position();
                end = fragment.position() + fragment.length();
            }
        }
    }
    if (start < 0) return false;
    QTextCursor cursor(document()); cursor.setPosition(start); cursor.setPosition(end, QTextCursor::KeepAnchor);
    setTextCursor(cursor); ensureCursorVisible(); setFocus();
    return true;
}
void NoteEditor::clearSelectionTodo()
{
    if (isReadOnly()) return;
    const QString href = textCursor().charFormat().anchorHref();
    if (!href.startsWith(QStringLiteral("nocturne-todo:")) || !locateTodo(href.mid(14))) return;
    auto cursor = textCursor(); cursor.beginEditBlock();
    QTextCharFormat clear; clear.setAnchor(false); clear.setAnchorHref(QString()); clear.setFontUnderline(false);
    cursor.mergeCharFormat(clear); cursor.endEditBlock(); setTextCursor(cursor);
}
void NoteEditor::contextMenuEvent(QContextMenuEvent* event)
{
    QMenu* menu = createStandardContextMenu();
    if (!isReadOnly()) {
        menu->addSeparator();
        auto* todo = menu->addAction(QStringLiteral("设置待办"));
        todo->setObjectName(QStringLiteral("selectionTodoAction"));
        todo->setEnabled(textCursor().hasSelection() && !textCursor().selectedText().trimmed().isEmpty());
        connect(todo, &QAction::triggered, this, &NoteEditor::selectionTodoRequested);
        if (textCursor().charFormat().anchorHref().startsWith(QStringLiteral("nocturne-todo:")))
            menu->addAction(QStringLiteral("取消这段待办标记"), this, &NoteEditor::clearSelectionTodo);
        auto* headings = menu->addMenu(QStringLiteral("分级标题"));
        for (int level = 0; level <= 6; ++level) {
            headings->addAction(level ? QStringLiteral("标题 %1").arg(level) : QStringLiteral("正文"),
                               this, [this, level] { applyHeadingLevel(level); });
        }
        menu->addAction(QStringLiteral("插入表格…"), this, &NoteEditor::showTableDialog);
        auto* convert = menu->addAction(QStringLiteral("选区转为表格"), this, &NoteEditor::showSelectionToTable);
        convert->setEnabled(textCursor().hasSelection() || textCursor().currentTable());
        if (auto* table = textCursor().currentTable()) {
            const auto cell = table->cellAt(textCursor());
            menu->addAction(QStringLiteral("在下方添加行"), this, [table, cell] { table->insertRows(cell.row() + 1, 1); });
            menu->addAction(QStringLiteral("在右侧添加列"), this, [table, cell] { table->insertColumns(cell.column() + 1, 1); });
            menu->addAction(QStringLiteral("删除当前行"), this, [table, cell] { table->removeRows(cell.row(), 1); });
            menu->addAction(QStringLiteral("删除当前列"), this, [table, cell] { table->removeColumns(cell.column(), 1); });
        }
    }
    menu->exec(event->globalPos()); delete menu;
}

void NoteEditor::mousePressEvent(QMouseEvent* event)
{
    m_imageClickTimer->stop();
    m_pressedTodo.clear();
    m_pressedImage = -1;
    if (event->button() == Qt::LeftButton && event->modifiers() == Qt::NoModifier) {
        m_pressedImage = imageAt(event->position().toPoint());
        m_linkPressPosition = event->position().toPoint();
        const QString href = anchorAt(event->position().toPoint());
        if (href.startsWith(QStringLiteral("nocturne-todo:"))
            && static_cast<ContrastHighlighter*>(m_contrastHighlighter)->todoStates.contains(href.mid(14))) {
            m_pressedTodo = href.mid(14);
            m_linkPressPosition = event->position().toPoint();
        }
    }
    QTextEdit::mousePressEvent(event);
}

void NoteEditor::mouseReleaseEvent(QMouseEvent* event)
{
    QTextEdit::mouseReleaseEvent(event);
    const QString pressed = m_pressedTodo;
    m_pressedTodo.clear();
    const int pressedImage = m_pressedImage;
    m_pressedImage = -1;
    if (!isReadOnly() && event->button() == Qt::LeftButton && pressedImage >= 0
        && !textCursor().hasSelection()
        && (event->position().toPoint() - m_linkPressPosition).manhattanLength() < QApplication::startDragDistance()
        && imageAt(event->position().toPoint()) == pressedImage) {
        QTextCursor cursor(document()); cursor.setPosition(pressedImage); cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
        m_pendingImage = cursor;
        m_imageClickTimer->start(QApplication::doubleClickInterval());
        return;
    }
    if (event->button() == Qt::LeftButton && !pressed.isEmpty() && !textCursor().hasSelection()
        && (event->position().toPoint() - m_linkPressPosition).manhattanLength() < QApplication::startDragDistance()
        && anchorAt(event->position().toPoint()) == QStringLiteral("nocturne-todo:") + pressed)
        emit todoActivated(pressed);
}

void NoteEditor::mouseMoveEvent(QMouseEvent* event)
{
    QTextEdit::mouseMoveEvent(event);
    if (event->buttons() != Qt::NoButton) return;
    const QString href = anchorAt(event->position().toPoint());
    const bool active = href.startsWith(QStringLiteral("nocturne-todo:"))
        && static_cast<ContrastHighlighter*>(m_contrastHighlighter)->todoStates.contains(href.mid(14));
    const bool image = imageAt(event->position().toPoint()) >= 0;
    viewport()->setCursor(active || image ? Qt::PointingHandCursor : Qt::IBeamCursor);
    viewport()->setToolTip(image ? QStringLiteral("单击编辑描述 · 双击放大查看") : active ? QStringLiteral("点击查看待办明细；拖动可选中文字") : QString());
}
void NoteEditor::keyPressEvent(QKeyEvent* event)
{
    if (!isReadOnly() && event->key() == Qt::Key_Space && !textCursor().hasSelection()
        && textCursor().atBlockEnd()) {
        const QString prefix = textCursor().block().text();
        const auto heading = QRegularExpression(QStringLiteral("^(#{1,6})$")).match(prefix);
        const bool bullet = prefix == "-" || prefix == "*" || prefix == "+";
        const bool numbered = QRegularExpression(QStringLiteral("^[0-9]+\\.$")).match(prefix).hasMatch();
        if (heading.hasMatch() || prefix == ">" || bullet || numbered) {
            auto cursor = textCursor(); cursor.beginEditBlock();
            cursor.select(QTextCursor::BlockUnderCursor); cursor.removeSelectedText();
            setTextCursor(cursor);
            if (heading.hasMatch()) applyHeadingLevel(heading.captured(1).size());
            else if (prefix == ">") applyQuote();
            else { QTextListFormat list; list.setStyle(numbered ? QTextListFormat::ListDecimal : QTextListFormat::ListDisc);
                   list.setIndent(1); cursor.createList(list); setTextCursor(cursor); }
            cursor.endEditBlock();
            return;
        }
    }
    if (!isReadOnly() && (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
        && !textCursor().hasSelection()) {
        const QString line = textCursor().block().text();
        if (line.startsWith(QStringLiteral("```")) && textCursor().atBlockEnd()) {
            auto cursor = textCursor(); cursor.beginEditBlock();
            const bool wasCode = cursor.blockFormat().hasProperty(QTextFormat::BlockCodeFence);
            cursor.select(QTextCursor::BlockUnderCursor); cursor.removeSelectedText(); setTextCursor(cursor);
            if (wasCode) applyHeadingLevel(0);
            else {
                applyCodeBlock();
                auto languageCursor = textCursor();
                auto format = languageCursor.blockFormat();
                format.setProperty(QTextFormat::BlockCodeLanguage, line.mid(3).trimmed());
                languageCursor.setBlockFormat(format);
            }
            cursor.endEditBlock(); return;
        }
        if (line.isEmpty() && textCursor().blockFormat().intProperty(QTextFormat::BlockQuoteLevel) > 0) {
            applyHeadingLevel(0); return;
        }
        const bool leaveTodo = textCursor().atBlockEnd()
            && textCursor().charFormat().anchorHref().startsWith(QStringLiteral("nocturne-todo:"));
        const bool heading = textCursor().blockFormat().headingLevel() > 0 && textCursor().atBlockEnd();
        QTextEdit::keyPressEvent(event);
        if (heading) applyHeadingLevel(0);
        if (leaveTodo) {
            QTextCharFormat plain; plain.setAnchor(false); plain.setAnchorHref(QString()); plain.setFontUnderline(false);
            mergeCurrentCharFormat(plain);
        }
        return;
    }
    QTextEdit::keyPressEvent(event);
    if (isReadOnly() || !QStringLiteral("*_`~").contains(event->text()) || event->text().isEmpty()) return;
    auto cursor = textCursor();
    const QString before = cursor.block().text().left(cursor.positionInBlock());
    const QRegularExpression patterns[] = {
        QRegularExpression(QStringLiteral("(\\*\\*|__)([^\\n]+?)\\1$")),
        QRegularExpression(QStringLiteral("(?<!\\*)\\*([^*\\n]+)\\*$")),
        QRegularExpression(QStringLiteral("`([^`\\n]+)`$")),
        QRegularExpression(QStringLiteral("~~([^~\\n]+)~~$"))
    };
    for (int type = 0; type < 4; ++type) {
        const auto match = patterns[type].match(before);
        if (!match.hasMatch()) continue;
        QTextCharFormat original = cursor.charFormat(), styled = original;
        if (type == 0) styled.setFontWeight(QFont::Bold);
        if (type == 1) styled.setFontItalic(true);
        if (type == 2) styled.setFontFamilies({QStringLiteral("Consolas")});
        if (type == 3) styled.setFontStrikeOut(true);
        cursor.beginEditBlock();
        cursor.setPosition(cursor.block().position() + match.capturedStart());
        cursor.setPosition(cursor.position() + match.capturedLength(), QTextCursor::KeepAnchor);
        cursor.insertText(match.captured(type == 0 ? 2 : 1), styled);
        cursor.setCharFormat(original);
        cursor.endEditBlock(); setTextCursor(cursor); break;
    }
}

QImage NoteEditor::imageResource(const QTextFormat& format) const
{
    const auto value = document()->resource(QTextDocument::ImageResource, QUrl(format.toImageFormat().name()));
    if (value.canConvert<QImage>()) return qvariant_cast<QImage>(value);
    if (value.canConvert<QPixmap>()) return qvariant_cast<QPixmap>(value).toImage();
    return {};
}
QSizeF NoteEditor::intrinsicSize(QTextDocument*, int, const QTextFormat& format)
{
    const QImage image = imageResource(format);
    if (image.isNull()) return QSizeF(48, 32);
    const auto img = format.toImageFormat();
    const qreal available = std::max(40, viewport()->width() - 24);
    const qreal width = std::min(available, img.width() > 0 ? img.width() : qreal(image.width()));
    return QSizeF(width, width * image.height() / image.width());
}
void NoteEditor::drawObject(QPainter* painter, const QRectF& rect, QTextDocument*, int, const QTextFormat& format)
{
    const auto image = imageResource(format);
    painter->save();
    painter->setRenderHint(QPainter::SmoothPixmapTransform);
    if (!image.isNull()) painter->drawImage(rect, image);
    else { painter->setPen(NocturneUi::theme().muted); painter->drawText(rect, Qt::AlignCenter, QStringLiteral("图片")); }
    painter->restore();
}
void NoteEditor::resizeEvent(QResizeEvent* event)
{
    QTextEdit::resizeEvent(event);
    document()->markContentsDirty(0, document()->characterCount());
}
void NoteEditor::setHtml(const QString& html)
{
    m_imageClickTimer->stop(); m_pendingImage = QTextCursor();
    QTextEdit::setHtml(html);
    repairImageBlocks();
    document()->clearUndoRedoStacks();
    document()->setModified(false);
}
void NoteEditor::repairImageBlocks()
{
    QTextCursor edit(document()); edit.beginEditBlock();
    for (auto block = document()->begin(); block.isValid(); block = block.next()) {
        bool hasImage = false;
        for (auto it = block.begin(); !it.atEnd(); ++it)
            hasImage |= it.fragment().charFormat().isImageFormat();
        if (!hasImage) continue;
        QTextCursor cursor(block); auto format = block.blockFormat();
        if (format.lineHeightType() != QTextBlockFormat::SingleHeight) {
            format.setLineHeight(0, QTextBlockFormat::SingleHeight);
            cursor.setBlockFormat(format);
        }
    }
    edit.endEditBlock();
}
int NoteEditor::imageAt(const QPoint& point) const
{
    const auto cursor = cursorForPosition(point);
    for (const int position : {cursor.position(), cursor.position() - 1}) {
        if (position < 0 || position >= document()->characterCount() - 1) continue;
        QTextCursor image(document()); image.setPosition(position); image.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
        if (!image.charFormat().isImageFormat()) continue;
        QTextCursor start(document()); start.setPosition(position);
        const auto size = const_cast<NoteEditor*>(this)->intrinsicSize(document(), position, image.charFormat());
        const auto caret = cursorRect(start);
        const QRectF bounds(caret.left(), caret.bottom() - size.height(), size.width(), size.height());
        if (bounds.adjusted(-2, -2, 2, 2).contains(point)) return position;
    }
    return -1;
}
void NoteEditor::setImageCaption(int position, const QString& caption)
{
    if (isReadOnly() || position < 0) return;
    QTextCursor cursor(document()); cursor.setPosition(position);
    cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
    if (!cursor.charFormat().isImageFormat()) return;
    cursor.beginEditBlock();
    auto image = cursor.charFormat(); image.setProperty(QTextFormat::ImageAltText, caption);
    cursor.setCharFormat(image);
    // Caption blocks use an HTML-persistent anchor, never a transient custom property.
    auto next = cursor.block().next();
    const bool existing = next.isValid() && next.begin().fragment().charFormat().anchorHref() == QStringLiteral("nocturne-caption:");
    if (existing) {
        QTextCursor old(next); old.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor); old.removeSelectedText();
        if (caption.isEmpty()) {
            old.setBlockFormat(QTextBlockFormat()); old.setCharFormat(QTextCharFormat());
            if (old.block().next().isValid()) old.deleteChar();
            cursor.endEditBlock(); return;
        }
        cursor = old;
    } else {
        if (caption.isEmpty()) { cursor.endEditBlock(); return; }
        cursor.clearSelection(); cursor.movePosition(QTextCursor::EndOfBlock);
        cursor.insertBlock();
    }
    QTextBlockFormat block; block.setAlignment(Qt::AlignHCenter); block.setTopMargin(6); block.setBottomMargin(12);
    cursor.setBlockFormat(block);
    QTextCharFormat text; text.setFontPointSize(10); text.setFontItalic(true);
    text.setAnchor(true); text.setAnchorHref(QStringLiteral("nocturne-caption:"));
    cursor.insertText(caption, text);
    if (!cursor.block().next().isValid()) cursor.insertBlock(QTextBlockFormat(), QTextCharFormat());
    cursor.endEditBlock(); setTextCursor(cursor); setFocus();
}
void NoteEditor::createTable(int rows, int columns)
{
    if (isReadOnly()) return;
    auto cursor = textCursor(); cursor.beginEditBlock();
    QTextTableFormat table;
    table.setBorder(1); table.setBorderStyle(QTextFrameFormat::BorderStyle_Solid);
    table.setBorderBrush(NocturneUi::theme().muted);
    table.setCellPadding(8); table.setCellSpacing(0);
    table.setWidth(QTextLength(QTextLength::PercentageLength, 100)); table.setHeaderRowCount(1);
    QVector<QTextLength> widths(std::clamp(columns, 1, 12), QTextLength(QTextLength::PercentageLength, 100.0 / std::clamp(columns, 1, 12)));
    table.setColumnWidthConstraints(widths);
    auto* inserted = cursor.insertTable(std::clamp(rows, 1, 100), std::clamp(columns, 1, 12), table);
    cursor = inserted->cellAt(0, 0).firstCursorPosition();
    cursor.endEditBlock(); setTextCursor(cursor); setFocus();
}
void NoteEditor::showTableDialog()
{
    if (isReadOnly()) return;
    NocturneDialog dialog(this); dialog.setWindowTitle(QStringLiteral("插入表格"));
    auto* form = new QFormLayout(dialog.body());
    auto* rows = new QSpinBox(&dialog); rows->setRange(1, 100); rows->setValue(3); rows->setAccessibleName(QStringLiteral("行数"));
    auto* columns = new QSpinBox(&dialog); columns->setRange(1, 12); columns->setValue(3); columns->setAccessibleName(QStringLiteral("列数"));
    form->addRow(QStringLiteral("行数（含表头）"), rows); form->addRow(QStringLiteral("列数"), columns);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() == QDialog::Accepted) createTable(rows->value(), columns->value());
}

void NoteEditor::mouseDoubleClickEvent(QMouseEvent* event)
{
    const int position = imageAt(event->position().toPoint());
    if (event->button() != Qt::LeftButton || position < 0) { QTextEdit::mouseDoubleClickEvent(event); return; }
    m_imageClickTimer->stop(); m_pressedImage = -1;
    QTextCursor cursor(document()); cursor.setPosition(position); cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
    const auto format = cursor.charFormat();
    QImage full;
    const QUrl url(format.toImageFormat().name());
    if (url.isLocalFile()) { QImageReader reader(url.toLocalFile()); reader.setAutoTransform(true); full = reader.read(); }
    if (full.isNull()) full = imageResource(format);
    if (full.isNull()) return;
    NocturneDialog dialog(this); dialog.setWindowTitle(QStringLiteral("图片预览")); dialog.resize(960, 700);
    auto* layout = new QVBoxLayout(dialog.body());
    auto* controls = new QHBoxLayout;
    auto* fit = new QPushButton(QStringLiteral("适应窗口"), &dialog);
    auto* actual = new QPushButton(QStringLiteral("原始尺寸"), &dialog);
    auto* minus = new QPushButton(QStringLiteral("缩小 −"), &dialog);
    auto* plus = new QPushButton(QStringLiteral("放大 +"), &dialog);
    controls->addWidget(fit); controls->addWidget(actual); controls->addWidget(minus); controls->addWidget(plus); controls->addStretch();
    layout->addLayout(controls);
    auto* scroll = new QScrollArea(&dialog); scroll->setAlignment(Qt::AlignCenter);
    auto* image = new QLabel; image->setAlignment(Qt::AlignCenter); scroll->setWidget(image); layout->addWidget(scroll, 1);
    auto* description = new QLabel(format.stringProperty(QTextFormat::ImageAltText), &dialog);
    description->setTextFormat(Qt::PlainText); description->setWordWrap(true); description->setAlignment(Qt::AlignCenter);
    layout->addWidget(description); description->setVisible(!description->text().isEmpty());
    qreal zoom = 1;
    auto render = [&] { const auto size = full.size() * zoom; image->setPixmap(QPixmap::fromImage(full.scaled(size, Qt::KeepAspectRatio, Qt::SmoothTransformation))); image->resize(size); };
    auto fitImage = [&] { zoom = std::min(1.0, std::min(qreal(std::max(40, scroll->viewport()->width() - 16)) / full.width(), qreal(std::max(40, scroll->viewport()->height() - 16)) / full.height())); render(); };
    connect(fit, &QPushButton::clicked, &dialog, fitImage);
    connect(actual, &QPushButton::clicked, &dialog, [&] { zoom = 1; render(); });
    connect(minus, &QPushButton::clicked, &dialog, [&] { zoom = std::max(0.02, zoom / 1.25); render(); });
    connect(plus, &QPushButton::clicked, &dialog, [&] { zoom = std::min(2.0, zoom * 1.25); render(); });
    QTimer::singleShot(0, &dialog, fitImage);
    dialog.exec(); event->accept();
}

bool NoteEditor::selectionToTable(QString* error)
{
    auto fail = [&](const QString& message) { if (error) *error = message; return false; };
    if (isReadOnly()) return fail(QStringLiteral("当前内容只读。"));
    auto selection = textCursor();
    QList<QTextTable*> tables;
    auto collect = [&](auto&& self, QTextFrame* frame) -> void {
        for (auto* child : frame->childFrames()) {
            if (auto* table = qobject_cast<QTextTable*>(child)) {
                if ((selection.hasSelection() && selection.selectionStart() <= table->lastPosition()
                     && selection.selectionEnd() > table->firstPosition()) || selection.currentTable() == table)
                    tables.append(table);
            }
            self(self, child);
        }
    };
    collect(collect, document()->rootFrame());
    auto style = [&](QTextTable* table) {
        auto format = table->format();
        format.setBorder(1); format.setBorderStyle(QTextFrameFormat::BorderStyle_Solid);
        format.setBorderBrush(NocturneUi::theme().muted);
        format.setBorderCollapse(true); format.setCellSpacing(0); format.setCellPadding(8);
        format.setWidth(QTextLength(QTextLength::PercentageLength, 100));
        format.setHeaderRowCount(1);
        format.setColumnWidthConstraints(QVector<QTextLength>(table->columns(), QTextLength(QTextLength::PercentageLength, 100.0/table->columns())));
        table->setFormat(format);
        for (int row = 0; row < table->rows(); ++row) {
            for (int col = 0; col < table->columns(); ++col) {
                auto cell = table->cellAt(row, col);
                if (cell.row() != row || cell.column() != col) continue;
                auto cursor = cell.firstCursorPosition();
                const int end = cell.lastCursorPosition().position();
                do {
                    auto block = cursor.blockFormat();
                    block.setTopMargin(4); block.setBottomMargin(4);
                    block.setLineHeight(120, QTextBlockFormat::ProportionalHeight);
                    cursor.setBlockFormat(block);
                } while (cursor.movePosition(QTextCursor::NextBlock) && cursor.position() <= end);
            }
        }
    };
    if (!tables.isEmpty()) {
        selection.beginEditBlock();
        for (auto* table : tables) style(table);
        selection.endEditBlock(); setTextCursor(selection); setFocus(); return true;
    }
    if (!selection.hasSelection()) return fail(QStringLiteral("请先选中需要转换的内容。"));
    const QString text = selection.selectedText().replace(QChar::ParagraphSeparator, '\n').replace(QChar::LineSeparator, '\n');
    if (text.contains(QChar::ObjectReplacementCharacter))
        return fail(QStringLiteral("请只选中表格文字，不包含图片。"));
    QStringList lines = text.split('\n');
    while (!lines.isEmpty() && lines.last().trimmed().isEmpty()) lines.removeLast();
    while (!lines.isEmpty() && lines.first().trimmed().isEmpty()) lines.removeFirst();
    if (lines.size() < 2 || lines.size() > 101)
        return fail(QStringLiteral("请选择 2—100 行内容，使用制表符、竖线或连续空格分隔列。"));
    const bool tabs = lines.first().contains('\t');
    const bool pipes = !tabs && lines.first().contains('|');
    const QRegularExpression separator(tabs ? QStringLiteral("\\t") : pipes ? QStringLiteral("(?<!\\\\)\\|") : QStringLiteral(" {2,}|　+"));
    QList<QStringList> rows;
    int columns = 0;
    for (auto line : lines) {
        if (pipes) {
            line = line.trimmed();
            if (line.startsWith('|')) line.remove(0, 1);
            if (line.endsWith('|') && !line.endsWith("\\|")) line.chop(1);
        }
        auto cells = line.split(separator, Qt::KeepEmptyParts);
        for (auto& cell : cells) cell = cell.trimmed();
        if (!columns) columns = cells.size();
        if (columns < 2 || columns > 12 || cells.size() != columns)
            return fail(QStringLiteral("无法可靠识别行列：每行请使用相同数量的制表符、竖线或连续两个以上空格分隔列。原文未修改。"));
        bool divider = pipes && rows.size() == 1;
        for (const auto& cell : cells) divider &= QRegularExpression(QStringLiteral("^:?-{3,}:?$")).match(cell).hasMatch();
        if (!divider) rows.append(cells);
    }
    if (rows.size() < 2 || rows.size() > 100) return fail(QStringLiteral("需要表头和至少一行数据，最多 100 行。"));
    // Construct first; only replace the selection after all rows have validated.
    QTextDocument converted;
    QTextCursor output(&converted);
    auto* table = output.insertTable(rows.size(), columns);
    for (int row = 0; row < rows.size(); ++row)
        for (int col = 0; col < columns; ++col) {
            auto cell = table->cellAt(row, col).firstCursorPosition();
            if (pipes) cell.insertFragment(QTextDocumentFragment::fromMarkdown(rows[row][col], QTextDocument::MarkdownDialectGitHub));
            else cell.insertText(rows[row][col]);
        }
    style(table);
    selection.beginEditBlock(); selection.insertFragment(QTextDocumentFragment(&converted)); selection.endEditBlock();
    setTextCursor(selection); setFocus(); return true;
}
void NoteEditor::showSelectionToTable()
{
    QString error;
    if (!selectionToTable(&error)) NocturneDialogs::information(this, QStringLiteral("选区转为表格"), error);
}
