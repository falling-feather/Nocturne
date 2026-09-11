#include "MathSupport.h"

#include <QCache>
#include <QBitArray>
#include <QDirIterator>
#include <QFontInfo>
#include <QPainter>
#include <QRegularExpression>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextFragment>
#include <QTextImageFormat>
#include <QTextTable>
#include <QUuid>
#include <algorithm>
#include <memory>
#include "latex.h"
#include "core/formula.h"
#include "platform/qt/graphic_qt.h"

static void initializeMathFonts() { Q_INIT_RESOURCE(MathFonts); }

namespace {
struct Span { int start; int length; MathSupport::Formula formula; bool delimited = true; QString error; };
bool escaped(const QString& text, int position)
{
    int count = 0;
    while (position > 0 && text.at(--position) == '\\') ++count;
    return count % 2 != 0;
}

void reserve(QBitArray& mask, int begin, int end)
{
    for (int i = std::max(0, begin); i < std::min(int(mask.size()), end); ++i) mask.setBit(i);
}

QBitArray literalMask(const QString& text)
{
    QBitArray mask(text.size());
    static const QRegularExpression fence(QStringLiteral("^ {0,3}(`{3,}|~{3,})(.*)$"));
    QChar fenceCharacter;
    int fenceLength = 0;
    bool frontMatter = text.startsWith("---\n");
    for (int start = 0; start < text.size();) {
        int end = text.indexOf('\n', start);
        if (end < 0) end = text.size();
        const QString line = text.mid(start, end - start);
        const auto marker = fence.match(line);
        if (frontMatter) {
            reserve(mask, start, end + 1);
            if (start > 0 && (line == "---" || line == "...")) frontMatter = false;
        } else if (fenceLength) {
            reserve(mask, start, end + 1);
            if (marker.hasMatch() && marker.captured(1).front() == fenceCharacter
                && marker.capturedLength(1) >= fenceLength && marker.captured(2).trimmed().isEmpty())
                fenceLength = 0;
        } else if (marker.hasMatch()) {
            fenceCharacter = marker.captured(1).front(); fenceLength = marker.capturedLength(1);
            reserve(mask, start, end + 1);
        } else if (line.startsWith("    ") || line.startsWith('\t')) {
            reserve(mask, start, end + 1);
        }
        start = end + 1;
    }
    for (int i = 0; i < text.size();) {
        if (mask.testBit(i) || text[i] != '`' || escaped(text, i)) { ++i; continue; }
        int count = 1;
        while (i + count < text.size() && text[i + count] == '`') ++count;
        int end = i + count;
        while ((end = text.indexOf(QString(count, '`'), end)) >= 0) {
            if ((end == 0 || text[end - 1] != '`')
                && (end + count == text.size() || text[end + count] != '`')) break;
            end += count;
        }
        if (end < 0) { i += count; continue; }
        reserve(mask, i, end + count); i = end + count;
    }
    static const QRegularExpression paths(QStringLiteral("(?:\\b[A-Za-z]:[\\\\/]|\\b[A-Za-z][A-Za-z0-9+.-]*://)[^\\s]+"));
    static const QRegularExpression links(QStringLiteral("!?\\[[^\\]\\n]*\\]\\((?:\\\\.|[^)\\n])*\\)"));
    static const QRegularExpression htmlCode(QStringLiteral("<(code|pre)\\b[^>]*>[\\s\\S]*?</\\1\\s*>"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression uncPaths(QStringLiteral("(?:^|(?<=\\s))\\\\\\\\[A-Za-z0-9_.-]+\\\\[^\\s]+"), QRegularExpression::MultilineOption);
    for (const auto& expression : {paths, links, htmlCode, uncPaths}) {
        auto matches = expression.globalMatch(text);
        while (matches.hasNext()) {
            const auto match = matches.next(); reserve(mask, match.capturedStart(), match.capturedEnd());
        }
    }
    return mask;
}

QList<Span> spans(const QString& text, QBitArray* reserved = nullptr)
{
    QList<Span> result;
    QBitArray mask = literalMask(text);
    for (int i = 0; i < text.size();) {
        if (mask.testBit(i)) { ++i; continue; }
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
        if (end < 0 || (!display && text.mid(i, end-i).contains('\n'))) {
            if (opening == "$" && text[i + 1].isDigit()) { ++i; continue; }
            int stop = display ? text.size() : text.indexOf('\n', i);
            if (stop < 0) stop = text.size();
            if (reserved) result.append({i, stop - i,
                {text.mid(i + opening.size(), stop - i - opening.size()), display}, true,
                QStringLiteral("公式分隔符未闭合")});
            reserve(mask, i, stop); i = stop; continue;
        }
        const QString source = text.mid(i + opening.size(), end - i - opening.size());
        bool literal = false;
        for (int position = i; position < end + closing.size() && !literal; ++position)
            literal = mask.testBit(position);
        if (!literal && !source.trimmed().isEmpty()) result.append({i, int(end + closing.size() - i), {source, display}});
        reserve(mask, i, end + closing.size());
        i = end + closing.size();
    }
    if (reserved) *reserved = mask;
    return result;
}

bool latin(QChar c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

int environmentEnd(const QString& text, int start)
{
    static const QRegularExpression tags(QStringLiteral("\\\\(begin|end)\\{([A-Za-z*]+)\\}"));
    auto matches = tags.globalMatch(text, start);
    QStringList stack;
    while (matches.hasNext()) {
        const auto match = matches.next();
        if (stack.isEmpty() && match.capturedStart() != start) return -1;
        if (match.captured(1) == "begin") stack.append(match.captured(2));
        else {
            if (stack.isEmpty() || stack.takeLast() != match.captured(2)) return -1;
            if (stack.isEmpty()) return match.capturedEnd();
        }
    }
    return -1;
}

QList<Span> allSpans(const QString& text)
{
    QBitArray reserved;
    auto result = spans(text, &reserved);
    static const QRegularExpression command(QStringLiteral("\\\\[A-Za-z]+"));
    static const QRegularExpression script(QStringLiteral("[A-Za-z][_^](?:[A-Za-z0-9]|\\{)"));
    const QStringList functions = {"sin", "cos", "tan", "log", "ln", "lim", "max", "min", "exp", "det", "sup", "inf", "mod"};
    int start = -1, end = 0, parentheses = 0;
    bool environment = false;
    auto finish = [&] {
        if (start < 0) return;
        while (start < end && text[start].isSpace()) ++start;
        while (end > start && text[end - 1].isSpace()) --end;
        const QString source = text.mid(start, end - start);
        if (!source.isEmpty() && (command.match(source).hasMatch()
                || (source.contains('=') && script.match(source).hasMatch()))) {
            const int line = start > 0 ? text.lastIndexOf('\n', start - 1) + 1 : 0;
            int lineEnd = text.indexOf('\n', end);
            if (lineEnd < 0) lineEnd = text.size();
            const bool standalone = text.mid(line, start - line).trimmed().isEmpty()
                && text.mid(end, lineEnd - end).trimmed().isEmpty();
            result.append({start, end - start, {source, environment || standalone}, false});
        }
        start = -1; parentheses = 0; environment = false;
    };
    for (int i = 0; i < text.size();) {
        if (reserved.testBit(i) || text[i] == '\n') { finish(); ++i; continue; }
        const QChar c = text[i];
        int next = i + 1;
        bool valid = false;
        if (c == '\\' && !escaped(text, i) && i + 1 < text.size()) {
            if (text.mid(i, 7) == "\\begin{") {
                next = environmentEnd(text, i);
                if (next < 0) next = text.size();
                environment = true; valid = true;
            } else if (latin(text[i + 1])) {
                while (next < text.size() && latin(text[next])) ++next;
                valid = true;
            } else if (QStringLiteral(",;! {}|\\").contains(text[i + 1])) {
                next = i + 2; valid = true;
            }
        } else if (c == '{') {
            int depth = 1;
            while (next < text.size() && text[next] != '\n' && !reserved.testBit(next)) {
                if (!escaped(text, next)) {
                    if (text[next] == '{') ++depth;
                    else if (text[next] == '}') --depth;
                }
                ++next;
                if (depth == 0) break;
            }
            valid = true;
        } else if (latin(c)) {
            while (next < text.size() && latin(text[next])) ++next;
            const QString word = text.mid(i, next - i);
            int following = next;
            while (following < text.size() && text[following] == ' ') ++following;
            valid = word.size() == 1 || functions.contains(word)
                || (following < text.size() && QStringLiteral("(_^=<>").contains(text[following]));
        } else if (c.isDigit() || (c.unicode() >= 0x0370 && c.unicode() <= 0x03ff)
            || c.category() == QChar::Symbol_Math || c == ' ' || c == '\t'
            || QStringLiteral("_{}^+-*/=<>!|()[]").contains(c)) {
            valid = true;
            if (c == '(' || c == '[') ++parentheses;
            else if (c == ')' || c == ']') --parentheses;
        } else if (c == ',' && parentheses > 0) valid = true;
        else if (c == '.' && i > 0 && next < text.size() && text[i - 1].isDigit() && text[next].isDigit()) valid = true;
        if (valid) {
            if (start < 0 && !c.isSpace()) start = i;
            end = next;
        } else finish();
        i = next;
    }
    finish();
    std::sort(result.begin(), result.end(), [](const Span& a, const Span& b) { return a.start < b.start; });
    return result;
}

QString sourceError(const QString& source)
{
    int depth = 0;
    for (int i = 0; i < source.size(); ++i) {
        if (escaped(source, i)) continue;
        if (source[i] == '{') ++depth;
        else if (source[i] == '}' && --depth < 0) return QStringLiteral("大括号不匹配");
    }
    if (depth != 0) return QStringLiteral("大括号未闭合");
    if (!source.isEmpty() && QStringLiteral("=+*/^_").contains(source.back()))
        return QStringLiteral("公式尚未完整");
    return {};
}

QString rendererSource(QString source)
{
    // Preserve the authored environment in storage/export; adapt only its display wrapper.
    static const QRegularExpression wrapper(QStringLiteral("\\\\(?:begin|end)\\{(?:equation\\*?|displaymath|math)\\}"));
    source.remove(wrapper);
    for (const auto& pair : {qMakePair(QStringLiteral("align"), QStringLiteral("aligned")),
             qMakePair(QStringLiteral("gather"), QStringLiteral("gathered")),
             qMakePair(QStringLiteral("multline"), QStringLiteral("gathered"))})
        for (const QString& tag : {QStringLiteral("begin"), QStringLiteral("end")})
            for (const QString& star : {QString(), QStringLiteral("*")})
                source.replace("\\" + tag + "{" + pair.first + star + "}", "\\" + tag + "{" + pair.second + "}");
    return source;
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
                || QFontInfo(f.charFormat().font()).fixedPitch()
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
ConversionReport convertAllMath(QTextDocument& document, ConversionTarget target)
{
    ConversionReport report;
    report.alreadyFormatted = formulaObjects(document).size();
    const auto matches = allSpans(document.toPlainText());
    QList<Span> accepted;
    for (const auto& match : matches) {
        if (target == ConversionTarget::RichText && codeAt(document, match.start, match.start + match.length)) continue;
        QTextCursor first(&document), last(&document);
        first.setPosition(match.start); last.setPosition(match.start + match.length - 1);
        QString error = match.error.isEmpty() ? sourceError(match.formula.source) : match.error;
        if (first.currentFrame() != last.currentFrame()
            || (first.currentTable() && first.currentTable()->cellAt(first) != first.currentTable()->cellAt(last)))
            error = QStringLiteral("公式跨越表格单元格，需分别处理");
        if (error.isEmpty()) error = render(match.formula, Qt::white).error;
        if (!error.isEmpty()) {
            ++report.skipped;
            if (report.errors.size() < 5) report.errors.append(match.formula.source.left(60) + "：" + error);
        } else if (target == ConversionTarget::MarkdownSource && match.delimited) ++report.alreadyFormatted;
        else accepted.append(match);
    }
    if (accepted.isEmpty()) return report;
    QTextCursor edit(&document); edit.beginEditBlock();
    for (auto it = accepted.crbegin(); it != accepted.crend(); ++it) {
        QTextCursor cursor(&document); cursor.setPosition(it->start);
        cursor.setPosition(it->start + it->length, QTextCursor::KeepAnchor);
        if (target == ConversionTarget::MarkdownSource) cursor.insertText(delimited(it->formula));
        else {
            QTextImageFormat image; image.merge(cursor.charFormat());
            image.setName(imageName(it->formula));
            image.setProperty(QTextFormat::ImageAltText, delimited(it->formula));
            image.setVerticalAlignment(QTextCharFormat::AlignMiddle);
            cursor.insertImage(image);
        }
        ++report.converted;
    }
    edit.endEditBlock();
    return report;
}
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
    // Qt can flatten a bare <code> tag into ordinary text under a document font.
    // Keep its code identity in the rich-text font format, which survives HTML saving.
    static const QRegularExpression codeOpen(QStringLiteral("<(?:code|pre)\\b[^>]*>"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression codeClose(QStringLiteral("</(?:code|pre)\\s*>"), QRegularExpression::CaseInsensitiveOption);
    output.replace(codeOpen, QStringLiteral("\\0<span style=\"font-family:'Consolas';\">"));
    output.replace(codeClose, QStringLiteral("</span>\\0"));
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
            tex::TeXParser parser(false, ("$" + rendererSource(value.source) + "$").toStdWString(), &parsed);
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
