#pragma once

#include <QColor>
#include <QImage>
#include <QString>
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
