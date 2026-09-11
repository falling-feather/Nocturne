#pragma once

#include <QColor>
#include <QImage>
#include <QString>
#include <QStringList>
#include <QTextDocument>

namespace MathSupport {
struct Formula {
    QString source;
    bool display = false;
};
struct Rendered {
    QImage image;
    QString error;
};
enum class ConversionTarget { RichText, MarkdownSource };
struct ConversionReport {
    int converted = 0;
    int alreadyFormatted = 0;
    int skipped = 0;
    QStringList errors;
};
ConversionReport convertAllMath(QTextDocument& document,
    ConversionTarget target = ConversionTarget::RichText);
void initialize();
QString imageName(const Formula& formula);
bool isFormula(const QString& name);
Formula formula(const QString& name);
QString delimited(const Formula& formula);
QString protectMarkdown(const QString& markdown);
void setMarkdown(QTextDocument& document, const QString& markdown);
QString normalizeHtml(const QString& html);
void renderDelimitedMath(QTextDocument& document);
QString plainText(const QTextDocument& document);
QString markdown(const QTextDocument& document);
Rendered render(const Formula& formula, const QColor& color, qreal fontPixels = 18);
void clearCache();
}
