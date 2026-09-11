#include "MathSupport.h"

#include <QCache>
#include <QDirIterator>
#include <QPainter>
#include <QRegularExpression>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextFragment>
#include <QTextImageFormat>
#include <QUuid>
#include <algorithm>
#include <memory>
#include "latex.h"
#include "core/formula.h"
#include "platform/qt/graphic_qt.h"

static void initializeMathFonts() { Q_INIT_RESOURCE(MathFonts); }

namespace {
struct Span { int start; int length; MathSupport::Formula formula; };
bool escaped(const QString& text, int position)
{
    int count = 0;
    while (position > 0 && text.at(--position) == '\\') ++count;
    return count % 2 != 0;
}

QList<Span> spans(const QString& text)
{
    QList<Span> result;
    for (int i = 0; i < text.size();) {
        // Markdown code spans and fenced blocks own their contents, including $.
        if ((text[i] == '`' || (text[i] == '~' && (i == 0 || text[i-1] == '\n'))) && !escaped(text, i)) {
            const QChar character = text[i];
            int count = 1;
            while (i + count < text.size() && text[i + count] == character) ++count;
            if (character == '`' || count >= 3) {
                const int end = text.indexOf(QString(count, character), i + count);
                i = end < 0 ? text.size() : end + count;
                continue;
            }
        }
        QString opening, closing;
        bool display = false;
        if (!escaped(text, i)) {
            if (text.mid(i, 2) == "$$") { opening = closing = "$$"; display = true; }
            else if (text.mid(i, 2) == "\\[") { opening = "\\["; closing = "\\]"; display = true; }
            else if (text.mid(i, 2) == "\\(") { opening = "\\("; closing = "\\)"; }
            else if (text[i] == '$' && i + 1 < text.size() && !text[i+1].isSpace()) opening = closing = "$";
        }
        if (opening.isEmpty()) { ++i; continue; }
        int end = i + opening.size();
        while ((end = text.indexOf(closing, end)) >= 0) {
            if (!escaped(text, end) && (opening != "$"
                || (end > i + 1 && !text[end-1].isSpace()
                    && (end + 1 == text.size() || !text[end+1].isDigit())))) break;
            end += closing.size();
        }
        if (end < 0 || (!display && text.mid(i, end-i).contains('\n'))) { i += opening.size(); continue; }
        const QString source = text.mid(i + opening.size(), end - i - opening.size());
        if (!source.trimmed().isEmpty()) result.append({i, int(end + closing.size() - i), {source, display}});
        i = end + closing.size();
    }
    return result;
}

bool codeAt(const QTextDocument& document, int start, int end)
{
    for (auto block = document.findBlock(start); block.isValid() && block.position() < end; block = block.next()) {
        if (block.blockFormat().hasProperty(QTextFormat::BlockCodeFence)
            || block.blockFormat().hasProperty(QTextFormat::BlockCodeLanguage)) return true;
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const auto f = it.fragment();
            if (f.position() >= end || f.position() + f.length() <= start) continue;
            if (f.charFormat().isImageFormat() || f.charFormat().fontFixedPitch()
                || f.charFormat().fontFamilies().toStringList().contains(QStringLiteral("Consolas"))) return true;
        }
    }
    return false;
}

QList<Span> formulaObjects(const QTextDocument& document)
{
    QList<Span> result;
    for (auto block = document.begin(); block.isValid(); block = block.next())
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const auto f = it.fragment();
            if (f.charFormat().isImageFormat() && MathSupport::isFormula(f.charFormat().toImageFormat().name()))
                for (int i = 0; i < f.length(); ++i)
                    result.append({f.position() + i, 1, MathSupport::formula(f.charFormat().toImageFormat().name())});
        }
    return result;
}
QCache<QString, MathSupport::Rendered>& cache()
{
    static QCache<QString, MathSupport::Rendered> values(8 * 1024); // KiB, bounded across notes.
    return values;
}
}

namespace MathSupport {
void initialize()
{
    // Adding application fonts invalidates Qt's font engines. Finish registration
    // before QTextDocument enters layout/painting, never from drawObject().
    static const bool initialized = [] {
        initializeMathFonts(); tex::LaTeX::init(":/nocturne-math");
        QDirIterator fonts(QStringLiteral(":/nocturne-math/fonts"), {QStringLiteral("*.ttf")}, QDir::Files, QDirIterator::Subdirectories);
        while (fonts.hasNext()) { tex::Font_qt font(fonts.next().toStdString(), 1.0f); }
        return true;
    }();
    Q_UNUSED(initialized)
}
QString imageName(const Formula& value)
{
    return QStringLiteral("nocturne-math:%1:").arg(value.display ? "block" : "inline")
        + QString::fromLatin1(value.source.toUtf8().toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}
bool isFormula(const QString& name)
{
    return name.startsWith("nocturne-math:inline:") || name.startsWith("nocturne-math:block:");
}
Formula formula(const QString& name)
{
    if (!isFormula(name)) return {};
    return {QString::fromUtf8(QByteArray::fromBase64(name.section(':', 2).toLatin1(), QByteArray::Base64UrlEncoding)),
            name.startsWith("nocturne-math:block:")};
}
QString delimited(const Formula& value)
{
    return value.display ? "$$\n" + value.source.trimmed() + "\n$$" : "$" + value.source + "$";
}
QString protectMarkdown(const QString& markdown)
{
    QString output = markdown;
    const auto matches = spans(markdown);
    for (auto it = matches.crbegin(); it != matches.crend(); ++it) {
        const QString image = QStringLiteral("![公式](%1)").arg(imageName(it->formula));
        output.replace(it->start, it->length, it->formula.display ? "\n\n" + image + "\n\n" : image);
    }
    return output;
}
QString normalizeHtml(const QString& html)
{
    // Browsers copy both visual glyphs and the accessible MathML representation.
    // Replace the outer formula container once, using its authoritative TeX annotation.
    static const QRegularExpression tags(QStringLiteral("<(/?)([\\w:-]+)\\b[^>]*>"));
    static const QRegularExpression annotation(QStringLiteral("<annotation\\b[^>]*encoding\\s*=\\s*['\"]application/x-tex['\"][^>]*>([\\s\\S]*?)</annotation>"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression katex(QStringLiteral("class\\s*=\\s*['\"][^'\"]*\\bkatex(?:-display)?\\b[^'\"]*['\"]"));
    QString output = html;
    QList<QPair<int, QString>> replacements;
    QList<int> lengths;
    auto matches = tags.globalMatch(html);
    int start = -1, depth = 0;
    QString name;
    while (matches.hasNext()) {
        const auto tag = matches.next();
        const QString type = tag.captured(2).toLower();
        const bool closing = !tag.captured(1).isEmpty();
        if (start < 0 && !closing && (type == "math" || type == "mjx-container" || (type == "span" && katex.match(tag.captured()).hasMatch()))) {
            start = tag.capturedStart(); name = type; depth = 1; continue;
        }
        if (start < 0 || type != name) continue;
        depth += closing ? -1 : 1;
        if (depth != 0) continue;
        const QString container = html.mid(start, tag.capturedEnd() - start);
        const auto source = annotation.match(container);
        if (source.hasMatch()) {
            QTextDocument decoded; decoded.setHtml(source.captured(1));
            const bool display = container.contains("katex-display") || container.contains("display=\"block\"") || container.contains("display='block'");
            const QString image = QStringLiteral("<img src=\"%1\" alt=\"公式\" />").arg(imageName({decoded.toPlainText(), display}));
            replacements.append({start, display ? "<p align=\"center\">" + image + "</p>" : image});
            lengths.append(tag.capturedEnd() - start);
        }
        start = -1;
    }
    for (int i = replacements.size() - 1; i >= 0; --i) output.replace(replacements[i].first, lengths[i], replacements[i].second);
    return output;
}
void renderDelimitedMath(QTextDocument& document)
{
    const auto matches = spans(document.toPlainText());
    QTextCursor edit(&document); edit.beginEditBlock();
    for (auto it = matches.crbegin(); it != matches.crend(); ++it) {
        if (codeAt(document, it->start, it->start + it->length)) continue;
        QTextCursor cursor(&document); cursor.setPosition(it->start);
        cursor.setPosition(it->start + it->length, QTextCursor::KeepAnchor);
        QTextImageFormat image; image.setName(imageName(it->formula));
        image.setProperty(QTextFormat::ImageAltText, delimited(it->formula));
        image.setVerticalAlignment(QTextCharFormat::AlignMiddle);
        cursor.insertImage(image);
        if (it->formula.display && cursor.block().text().trimmed() == QString(QChar::ObjectReplacementCharacter)) {
            auto format = cursor.blockFormat(); format.setAlignment(Qt::AlignHCenter);
            format.setLineHeight(0, QTextBlockFormat::SingleHeight); cursor.setBlockFormat(format);
        }
    }
    edit.endEditBlock();
}
void setMarkdown(QTextDocument& document, const QString& markdown)
{
    document.setMarkdown(protectMarkdown(markdown), QTextDocument::MarkdownDialectGitHub);
    // A standalone block formula follows paragraph alignment and stays editable.
    for (auto block = document.begin(); block.isValid(); block = block.next()) {
        if (block.text().trimmed() != QString(QChar::ObjectReplacementCharacter)) continue;
        const auto f = block.begin().fragment().charFormat();
        if (f.isImageFormat() && isFormula(f.toImageFormat().name()) && formula(f.toImageFormat().name()).display) {
            QTextCursor cursor(block); auto format = block.blockFormat();
            format.setAlignment(Qt::AlignHCenter); format.setLineHeight(0, QTextBlockFormat::SingleHeight);
            cursor.setBlockFormat(format);
        }
    }
}
QString plainText(const QTextDocument& document)
{
    QString text = document.toPlainText();
    const auto objects = formulaObjects(document);
    for (auto it = objects.crbegin(); it != objects.crend(); ++it)
        text.replace(it->start, it->length, delimited(it->formula));
    return text;
}
QString markdown(const QTextDocument& document)
{
    std::unique_ptr<QTextDocument> copy(document.clone());
    const auto objects = formulaObjects(document);
    const QString prefix = "NOCTURNEMATH" + QUuid::createUuid().toString(QUuid::Id128);
    QList<QPair<QString, QString>> tokens;
    for (auto it = objects.crbegin(); it != objects.crend(); ++it) {
        const QString token = prefix + QString::number(it->start);
        QTextCursor cursor(copy.get()); cursor.setPosition(it->start);
        cursor.setPosition(it->start + 1, QTextCursor::KeepAnchor);
        cursor.insertText(token, QTextCharFormat());
        tokens.append({token, delimited(it->formula)});
    }
    QString text = copy->toMarkdown(QTextDocument::MarkdownDialectGitHub);
    for (const auto& token : tokens) text.replace(token.first, token.second);
    return text;
}
Rendered render(const Formula& value, const QColor& color, qreal fontPixels)
{
    const QString key = imageName(value) + color.name(QColor::HexArgb) + QString::number(fontPixels);
    if (const auto* saved = cache().object(key)) return *saved;
    Rendered result;
    // This is a bounded math renderer, not a general TeX interpreter.
    static const QRegularExpression definitions(QStringLiteral("\\\\(?:newcommand|renewcommand|newenvironment|renewenvironment|def|gdef|edef|xdef|input|include|includegraphics|write|loop|catcode|csname)\\b"));
    int depth = 0, maxDepth = 0;
    for (int i = 0; i < value.source.size(); ++i) {
        if (escaped(value.source, i)) continue;
        if (value.source[i] == '{') maxDepth = std::max(maxDepth, ++depth);
        else if (value.source[i] == '}') --depth;
    }
    if (value.source.size() > 4096 || maxDepth > 48 || definitions.match(value.source).hasMatch())
        result.error = QStringLiteral("公式过长、嵌套过深或包含不支持的宏，请编辑源码。");
    else {
        try {
            initialize();
            tex::Formula parsed;
            tex::TeXParser parser(false, ("$" + value.source + "$").toStdWString(), &parsed);
            parser.parse();
            tex::TeXRenderBuilder builder;
            std::unique_ptr<tex::TeXRender> layout(builder.setStyle(value.display ? tex::TexStyle::display : tex::TexStyle::text)
                .setTextSize(float(fontPixels)).setForeground(color.rgba()).build(parsed));
            const QSize logical(layout->getWidth() + 8, layout->getHeight() + 6);
            if (logical.width() <= 0 || logical.height() <= 0 || logical.width() > 4096 || logical.height() > 2048)
                result.error = QStringLiteral("公式超出显示范围，请分成较短的公式。");
            else {
                result.image = QImage(logical * 2, QImage::Format_ARGB32_Premultiplied);
                result.image.setDevicePixelRatio(2); result.image.fill(Qt::transparent);
                QPainter painter(&result.image);
                painter.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
                tex::Graphics2D_qt graphics(&painter); layout->draw(graphics, 4, 3);
            }
        } catch (const std::exception& error) {
            result.error = QStringLiteral("无法解析公式：%1").arg(QString::fromUtf8(error.what()));
        }
    }
    if (result.image.isNull()) {
        result.image = QImage(960, 128, QImage::Format_ARGB32_Premultiplied);
        result.image.setDevicePixelRatio(2); result.image.fill(Qt::transparent);
        QPainter painter(&result.image); painter.setPen(color);
        painter.drawText(QRect(4, 2, 472, 60), Qt::TextWordWrap, value.source.left(160) + QStringLiteral("  [右键编辑公式]"));
    }
    cache().insert(key, new Rendered(result), std::max<qint64>(1, result.image.sizeInBytes() / 1024));
    return result;
}
void clearCache() { cache().clear(); }
}
