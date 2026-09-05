#include "NocturneStyle.h"

#include <QAbstractButton>
#include <QAbstractItemModel>
#include <QAction>
#include <QApplication>
#include <QFontDatabase>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QStyleOptionViewItem>
#include <initializer_list>

namespace {
QString firstInstalledFamily(std::initializer_list<QString> candidates)
{
    const QStringList installed = QFontDatabase::families();
    for (const QString& candidate : candidates)
        if (installed.contains(candidate, Qt::CaseInsensitive))
            return candidate;
    return QStringLiteral("Microsoft YaHei UI");
}

const NocturneUi::Theme themes[] = {
    { "night", "夜航 · 墨蓝暖金",
      "#0C141F", "#101924", "#131F2C", "#192532", "#15212E", "#2A3847",
      "#DEE5EB", "#94A4B5", "#D8BD88", "#19232C", "#293849", "#223140", "#94BFA3", "#E5D5B7" },
    { "harbor", "雾港 · 青灰银月",
      "#0C1C20", "#102226", "#132529", "#182C30", "#16292D", "#2C4447",
      "#D9E8E5", "#97B1AE", "#A5C9BC", "#16312C", "#29443F", "#233B3B", "#9BC7B0", "#C5DFD5" },
    { "moonlight", "月白 · 清朗纸页",
      "#172B3A", "#F1F4F7", "#E9EEF2", "#F4F6F8", "#EDF1F4", "#D6DFE6",
      "#253846", "#637586", "#526F83", "#FFFFFF", "#D6E2EB", "#E1E8EE", "#48775D", "#283F50" }
};
int currentTheme = 0;

void drawGlyph(QPainter& p, NocturneUi::Glyph glyph)
{
    using G = NocturneUi::Glyph;
    QPainterPath path;
    switch (glyph) {
    case G::Notebook:
        p.drawRoundedRect(QRectF(5, 3, 15, 18), 1.5, 1.5);
        p.drawLine(9, 3, 9, 21);
        for (int y : {7, 12, 17}) p.drawLine(3, y, 6, y);
        p.drawLine(12, 8, 17, 8); p.drawLine(12, 12, 17, 12); break;
    case G::Sticky:
        path.moveTo(20, 11); path.lineTo(20, 4); path.lineTo(4, 4);
        path.lineTo(4, 20); path.lineTo(12, 20); p.drawPath(path);
        p.drawLine(18, 14, 18, 22); p.drawLine(14, 18, 22, 18); break;
    case G::Book:
        path.moveTo(12, 6); path.cubicTo(8, 3, 5, 3, 2, 5);
        path.lineTo(2, 20); path.cubicTo(5, 18, 8, 18, 12, 21);
        path.cubicTo(16, 18, 19, 18, 22, 20); path.lineTo(22, 5);
        path.cubicTo(19, 3, 16, 3, 12, 6); path.lineTo(12, 21);
        p.drawPath(path); break;
    case G::Todo:
        p.drawEllipse(QRectF(3, 3, 18, 18));
        path.moveTo(7, 12); path.lineTo(10.5, 15.5); path.lineTo(17, 9); p.drawPath(path); break;
    case G::Settings:
        p.drawEllipse(QRectF(8, 8, 8, 8));
        path.moveTo(9, 2); path.lineTo(15, 2); path.lineTo(16, 5);
        path.lineTo(19, 5); path.lineTo(22, 10); path.lineTo(20, 12);
        path.lineTo(22, 15); path.lineTo(19, 20); path.lineTo(16, 19);
        path.lineTo(15, 22); path.lineTo(9, 22); path.lineTo(8, 19);
        path.lineTo(5, 20); path.lineTo(2, 15); path.lineTo(4, 12);
        path.lineTo(2, 10); path.lineTo(5, 5); path.lineTo(8, 5); path.closeSubpath();
        p.drawPath(path); break;
    case G::Palette:
        path.moveTo(20, 15); path.cubicTo(24, 6, 17, 1, 10, 3);
        path.cubicTo(0, 5, 1, 20, 10, 21); path.cubicTo(15, 22, 11, 16, 15, 16);
        path.cubicTo(17, 16, 19, 17, 20, 15); p.drawPath(path);
        for (const QPointF& pt : {QPointF(7, 8), QPointF(12, 6), QPointF(17, 8), QPointF(6, 13)})
            p.drawEllipse(pt, 0.8, 0.8);
        break;
    case G::Search:
        p.drawEllipse(QRectF(3, 3, 12, 12)); p.drawLine(14, 14, 21, 21); break;
    case G::Folder:
        path.moveTo(3, 6); path.lineTo(9, 6); path.lineTo(11, 9); path.lineTo(21, 9);
        path.lineTo(21, 20); path.lineTo(3, 20); path.closeSubpath(); p.drawPath(path); break;
    case G::Plus: p.drawLine(12, 5, 12, 19); p.drawLine(5, 12, 19, 12); break;
    case G::Close: p.drawLine(6, 6, 18, 18); p.drawLine(6, 18, 18, 6); break;
    case G::Minimize: p.drawLine(5, 13, 19, 13); break;
    case G::Maximize: p.drawRect(QRectF(5, 5, 14, 14)); break;
    case G::Restore:
        path.moveTo(9, 5); path.lineTo(20, 5); path.lineTo(20, 16); p.drawPath(path);
        p.drawRect(QRectF(5, 9, 11, 11)); break;
    case G::Focus:
        p.drawEllipse(QRectF(3, 3, 18, 18)); p.drawEllipse(QRectF(8, 8, 8, 8)); break;
    case G::Image:
        p.drawRoundedRect(QRectF(3, 3, 18, 18), 1.5, 1.5);
        p.drawEllipse(QPointF(8, 8), 1.5, 1.5);
        path.moveTo(4, 18); path.lineTo(11, 11); path.lineTo(15, 15);
        path.lineTo(18, 12); path.lineTo(21, 15); p.drawPath(path); break;
    case G::Bullet:
    case G::Numbered:
        for (int y : {6, 12, 18}) {
            p.drawLine(10, y, 21, y);
            if (glyph == G::Bullet) p.drawEllipse(QPointF(4.5, y), 0.7, 0.7);
            else { QFont f("Segoe UI"); f.setPixelSize(7); p.setFont(f);
                p.drawText(QRectF(1, y - 4.5, 6, 9), Qt::AlignCenter, QString::number(y / 6)); }
        }
        break;
    case G::Pin:
        path.moveTo(8, 3); path.lineTo(17, 3); path.lineTo(16, 10);
        path.lineTo(20, 14); path.lineTo(5, 14); path.lineTo(9, 10);
        path.closeSubpath(); p.drawPath(path); p.drawLine(12, 14, 12, 22); break;
    case G::More:
        for (int x : {5, 12, 19}) p.drawEllipse(QPointF(x, 12), 0.8, 0.8);
        break;
    case G::Trash:
        p.drawLine(4, 6, 20, 6); p.drawLine(9, 3, 15, 3);
        path.moveTo(6, 6); path.lineTo(7, 21); path.lineTo(17, 21); path.lineTo(18, 6);
        p.drawPath(path); p.drawLine(10, 10, 10, 17); p.drawLine(14, 10, 14, 17); break;
    case G::Chevron:
        path.moveTo(6, 9); path.lineTo(12, 15); path.lineTo(18, 9); p.drawPath(path); break;
    case G::ArrowLeft:
        path.moveTo(14, 5); path.lineTo(7, 12); path.lineTo(14, 19); p.drawPath(path); break;
    }
}
}

namespace NocturneUi {
const Theme& theme() { return themes[currentTheme]; }

void setTheme(const QString& id)
{
    currentTheme = 0;
    for (int i = 0; i < 3; ++i)
        if (themes[i].id == id) currentTheme = i;
}

QString serifFamily()
{
    static const QString family = firstInstalledFamily({
        QStringLiteral("Noto Serif SC"), QStringLiteral("Source Han Serif SC"), QStringLiteral("Noto Serif CJK SC"),
        QStringLiteral("FZShuSong-Z01S"), QStringLiteral("STZhongsong"), QStringLiteral("SimSun")});
    return family;
}

QString sansFamily()
{
    static const QString family = firstInstalledFamily({
        QStringLiteral("Noto Sans SC"), QStringLiteral("Microsoft YaHei UI"), QStringLiteral("Source Han Sans SC"),
        QStringLiteral("Noto Sans CJK SC"), QStringLiteral("Microsoft YaHei")});
    return family;
}

QIcon icon(Glyph glyph, const QColor& color)
{
    QIcon result;
    for (auto mode : {QIcon::Normal, QIcon::Active, QIcon::Disabled}) {
        for (auto state : {QIcon::Off, QIcon::On}) {
            QColor ink = color.isValid() ? color
                : (state == QIcon::On ? theme().accent
                   : (mode == QIcon::Active ? theme().text : theme().muted));
            if (mode == QIcon::Disabled) ink.setAlpha(95);
            for (int scale : {1, 2, 3}) {
                QPixmap pixmap(24 * scale, 24 * scale);
                pixmap.setDevicePixelRatio(scale);
                pixmap.fill(Qt::transparent);
                QPainter painter(&pixmap);
                painter.setRenderHint(QPainter::Antialiasing);
                painter.setPen(QPen(ink, 1.55, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                painter.setBrush(Qt::NoBrush);
                drawGlyph(painter, glyph);
                painter.end();
                result.addPixmap(pixmap, mode, state);
            }
        }
    }
    return result;
}

void setGlyph(QObject* object, Glyph glyph)
{
    object->setProperty("glyph", static_cast<int>(glyph));
    const QColor color = object->property("iconTone").toString() == QStringLiteral("primary")
        ? theme().accentText
        : (object->property("iconTone").toString() == QStringLiteral("rail")
            ? QColor("#B0C0CC") : QColor());
    if (auto* button = qobject_cast<QAbstractButton*>(object)) {
        button->setIcon(icon(glyph, color));
        button->setIconSize(QSize(20, 20));
    } else if (auto* action = qobject_cast<QAction*>(object)) {
        action->setIcon(icon(glyph, color));
    }
}

void refreshIcons(QWidget* root)
{
    for (QObject* object : root->findChildren<QObject*>())
        if (object->property("glyph").isValid())
            setGlyph(object, static_cast<Glyph>(object->property("glyph").toInt()));
}

void applyPalette()
{
    const auto& t = theme();
    QPalette p;
    p.setColor(QPalette::Window, t.panel);
    p.setColor(QPalette::WindowText, t.text);
    p.setColor(QPalette::Base, t.paper);
    p.setColor(QPalette::AlternateBase, t.sidebar);
    p.setColor(QPalette::Text, t.text);
    p.setColor(QPalette::PlaceholderText, t.muted);
    p.setColor(QPalette::Button, t.selected);
    p.setColor(QPalette::ButtonText, t.text);
    p.setColor(QPalette::Highlight, t.selected);
    p.setColor(QPalette::HighlightedText, t.text);
    p.setColor(QPalette::Link, t.accent);
    for (auto role : {QPalette::Text, QPalette::WindowText, QPalette::ButtonText})
        p.setColor(QPalette::Disabled, role, t.muted);
    qApp->setPalette(p);
}

QString styleSheet()
{
    QString css = QStringLiteral(R"(
        QWidget { font-family: "@sans"; color: @text;
                  selection-background-color: @selected; selection-color: @text; }
        QMainWindow, QWidget#windowShell { background: @paper; }
        QFrame#appTitleBar { background: @title; border-bottom: 1px solid @border; }
        QFrame#toolRail, QWidget#logoCell { background: @rail; border: 0; }
        QFrame#navigation { background: @sidebar; border-right: 1px solid @border; }
        QFrame#todoPane { background: @panel; border-left: 1px solid @border; }
        QFrame#editorPane, QWidget#writingColumn { background: @paper; }
        QFrame#documentHeader, QFrame#formatBar { background: @paper; border: 0;
            border-bottom: 1px solid @border; }
        QFrame#editorFooter { background: @paper; border: 0; border-top: 1px solid @border; }
        QLabel { background: transparent; border: 0; }
        QLabel#titleBrandName { color: @heading; font-family: "@serif"; font-size: 24px; }
        QLabel#titleBrandEnglish { color: @muted; font-family: "Segoe UI";
            font-size: 9px; letter-spacing: 3px; }
        QLabel#primaryHint { color: @accentText; font-family: "Segoe UI"; font-size: 10px; }
        QLabel#libraryCount { color: @muted; font-size: 12px; }
        QLabel#sectionCaption, QLabel#panelTitle { font-size: 21px; font-weight: 600; }
        QLabel#brandMotto { color: @muted; font-family: "@serif"; font-size: 12px; }
        QLabel#mutedLabel, QLabel#noteCount, QLabel#noteMeta, QLabel#wordCount,
        QLabel#todoSummary, QLabel#emptyHint { color: @muted; font-size: 12px; }
        QLabel#saveState, QLabel#saveHint { color: @success; font-size: 11px; }
        QLabel#emptyHeading { color: @heading; font-size: 24px; font-family: "@serif"; }
        QFrame#separator, QFrame#toolSeparator { background: @border; border: 0; }
        QLineEdit, QKeySequenceEdit { background: @panel; color: @text;
            border: 1px solid @border; border-radius: 6px; padding: 7px 10px; font-size: 12px; }
        QLineEdit:focus, QKeySequenceEdit:focus { border-color: @accent; }
        QLineEdit:disabled { color: @muted; background: @sidebar; }
        QLineEdit#titleEdit { background: transparent; color: @heading; border: 0;
            padding: 0; font-family: "@serif"; font-size: 32px; font-weight: 400; }
        QLineEdit#searchEdit { background: @sidebar; padding: 7px 8px; }
        QTextEdit { background: @paper; color: @text; border: 0; font-size: 12pt; }
        QTextEdit#noteEditor { padding: 0; }
        QPushButton { background: @selected; color: @text; border: 1px solid @border;
            border-radius: 6px; padding: 7px 11px; font-size: 12px; }
        QPushButton:hover { background: @hover; border-color: @muted; }
        QPushButton:pressed { border-color: @accent; }
        QPushButton:focus, QToolButton:focus { border: 1px solid @accent; }
        QPushButton:disabled { color: @muted; background: @sidebar; }
        QPushButton#primaryButton, QPushButton#roundButton {
            color: @accentText; background: @accent; border: 1px solid @accent; font-weight: 600; }
        QPushButton#primaryButton { text-align: left; padding-left: 16px; font-size: 13px; }
        QPushButton#primaryButton:hover, QPushButton#roundButton:hover { background: @heading; }
        QPushButton#quietButton { border: 0; background: transparent; color: @muted; text-align: left; }
        QPushButton#quietButton:hover { color: @accent; background: @hover; }
        QToolButton { background: transparent; color: @muted; border: 1px solid transparent;
            border-radius: 5px; padding: 3px; font-size: 13px; }
        QToolButton:hover { background: @hover; color: @text; }
        QToolButton:checked { background: @selected; color: @accent; }
        QToolButton[railButton="true"], QPushButton#secondaryButton {
            background: transparent; border: 1px solid transparent; border-radius: 7px; padding: 0; }
        QToolButton[railButton="true"]:hover, QPushButton#secondaryButton:hover { background: #233542; }
        QToolButton[railButton="true"]:checked { background: #233542; border-color: @accent; }
        QToolButton::menu-indicator, QPushButton::menu-indicator { image: none; width: 0; height: 0; }
        QToolButton#windowCloseButton:hover, QToolButton#stickyCloseButton:hover {
            background: #A64B52; color: #FFFFFF; }
        QToolButton[windowControl="true"] { border: 0; border-radius: 0; }
        QComboBox { background: transparent; color: @text; border: 1px solid transparent;
            border-radius: 5px; padding: 5px 8px; font-size: 12px; min-height: 22px; }
        QComboBox:hover { background: @hover; }
        QComboBox:focus { border-color: @accent; }
        QComboBox::drop-down { border: 0; width: 22px; }
        QComboBox#fontSizeCombo QLineEdit { padding: 0; border: 0;
            background: transparent; font-family: "Segoe UI"; font-size: 12px; }
        QToolButton#colorButton { border-bottom: 2px solid @accent; border-radius: 0; }
        QComboBox QAbstractItemView { background: @panel; color: @text;
            border: 1px solid @border; selection-background-color: @selected; selection-color: @text;
            outline: none; padding: 4px; }
        QComboBox QAbstractItemView::item { min-height: 30px; padding: 3px 8px; }
        QListWidget { background: transparent; color: @text; border: 0; outline: none; }
        QListWidget#noteList::item, QListWidget#todoList::item { padding: 0; border: 0; background: transparent; }
        QMenuBar { background: transparent; color: @muted; padding: 0; spacing: 3px; font-size: 12px; }
        QMenuBar::item { background: transparent; color: @muted; padding: 7px 12px; border-radius: 4px; }
        QMenuBar::item:selected, QMenuBar::item:pressed { background: @hover; color: @text; }
        QMenu { background: @panel; color: @text; border: 1px solid @border; padding: 6px; }
        QMenu::item { padding: 9px 32px 9px 14px; background: transparent; color: @text; border-radius: 4px; }
        QMenu::item:selected { background: @selected; color: @text; }
        QMenu::item:disabled { color: @muted; }
        QMenu::separator { height: 1px; background: @border; margin: 5px 8px; }
        QToolTip { background: @panel; color: @text; border: 1px solid @border; padding: 6px 9px; }
        QDialog, QMessageBox { background: @panel; color: @text; }
        QDialog[nocturneDialog="true"] { border: 1px solid @border; }
        QFrame#dialogHeader { background: @title; border-bottom: 1px solid @border; }
        QWidget#dialogBody { background: @panel; }
        QLabel#dialogCaption { color: @muted; font-size: 12px; }
        QLabel#dialogHeading { color: @heading; font-family: "@serif"; font-size: 25px; }
        QLabel#dialogMessage { color: @muted; font-size: 13px; }
        QPushButton[primaryAction="true"], QPushButton#hotkeyApplyButton,
        QPushButton#collectStickiesAccept { background: @accent; color: @accentText;
            border-color: @accent; padding: 8px 18px; }
        QPushButton[primaryAction="true"]:disabled { background: @selected; color: @muted; border-color: @border; }
        QTreeView, QListView { background: @paper; color: @text; border: 1px solid @border;
            selection-background-color: @selected; selection-color: @text; }
        QListWidget#noteList, QListWidget#todoList { background: transparent; border: 0; }
        QHeaderView::section { background: @panel; color: @muted; border: 0;
            border-bottom: 1px solid @border; padding: 6px; }
        QLabel#collectHeading, QLabel#hotkeyHeading { color: @heading; font-family: "@serif"; }
        QLabel#collectEyebrow, QLabel#hotkeyEyebrow { color: @accent; font-size: 9px; letter-spacing: 2px; }
        QLabel#collectHint, QLabel#hotkeyHint, QLabel#hotkeyValidationLabel { color: @muted; font-size: 12px; }
        QLabel#collectFormLabel, QLabel#hotkeyFormLabel { color: @text; }
        QListWidget#collectStickyList { background: @paper; border: 1px solid @border; border-radius: 6px; }
        QListWidget#collectStickyList::item { color: @text; padding: 10px; border-bottom: 1px solid @border; }
        QListWidget#collectStickyList::item:selected { background: @selected; }
        QCheckBox { spacing: 8px; }
        QCheckBox::indicator { width: 16px; height: 16px; }
        QProgressBar { background: @border; border: 0; border-radius: 2px; }
        QProgressBar::chunk { background: @accent; border-radius: 2px; }
        QScrollBar:vertical { background: transparent; width: 8px; margin: 2px; }
        QScrollBar::handle:vertical { background: @border; min-height: 28px; border-radius: 3px; }
        QScrollBar::handle:vertical:hover { background: @muted; }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
        QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }
        QWidget#stickyNoteWindow { background: @paper; border: 1px solid @border; }
        QFrame#stickyHeader { background: @title; border: 0; border-top: 2px solid @accent;
            border-bottom: 1px solid @border; }
        QFrame#stickyBody { background: @paper; }
        QLabel#stickyTitle { color: @heading; font-family: "@serif"; font-size: 16px; }
        QTextEdit#stickyEditor { background: @paper; font-size: 12pt; padding: 0; }
        QToolButton#stickyPinButton:checked { background: transparent; color: @accent; }
        QWidget#opacityPanel { background: @panel; min-width: 240px; }
        QLabel#opacityTitle { font-size: 12px; font-weight: 600; }
        QLabel#opacityValue { color: @accent; font-size: 12px; }
        QLabel#opacityHint { color: @muted; font-size: 11px; }
        QSlider::groove:horizontal { height: 4px; background: @border; border-radius: 2px; }
        QSlider::sub-page:horizontal { background: @accent; }
        QSlider::handle:horizontal { width: 14px; margin: -5px 0;
            border-radius: 7px; background: @accent; }
    )");
    const auto& t = theme();
    const QList<QPair<QString, QString>> tokens = {
        {"@sans", sansFamily()}, {"@serif", serifFamily()},
        {"@accentText", t.accentText.name()}, {"@selected", t.selected.name()},
        {"@heading", t.heading.name()}, {"@success", t.success.name()},
        {"@sidebar", t.sidebar.name()}, {"@border", t.border.name()},
        {"@accent", t.accent.name()}, {"@muted", t.muted.name()}, {"@hover", t.hover.name()},
        {"@title", t.titleBar.name()}, {"@paper", t.paper.name()}, {"@panel", t.panel.name()},
        {"@rail", t.rail.name()}, {"@text", t.text.name()}
    };
    for (const auto& token : tokens) css.replace(token.first, token.second);
    return css;
}
} // namespace NocturneUi

void NocturneComboBox::paintEvent(QPaintEvent* event)
{
    QComboBox::paintEvent(event);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(NocturneUi::theme().muted, 1.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    const qreal x = width() - 14;
    const qreal y = height() / 2.0;
    QPainterPath arrow;
    arrow.moveTo(x - 3, y - 1.5); arrow.lineTo(x, y + 1.5); arrow.lineTo(x + 3, y - 1.5);
    p.drawPath(arrow);
}

NocturneBackdrop::NocturneBackdrop(QWidget* parent) : QWidget(parent) {}
void NocturneBackdrop::paintEvent(QPaintEvent*) {
    QPainter p(this); p.fillRect(rect(), NocturneUi::theme().paper);
}
NocturneGlassPanel::NocturneGlassPanel(bool decorativeRipples, QWidget* parent)
    : QFrame(parent), m_decorativeRipples(decorativeRipples) {}
void NocturneGlassPanel::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), m_decorativeRipples ? NocturneUi::theme().panel : NocturneUi::theme().sidebar);
    p.setPen(NocturneUi::theme().border);
    const int x = m_decorativeRipples ? 0 : width() - 1;
    p.drawLine(x, 0, x, height());
}
NocturnePaperPanel::NocturnePaperPanel(QWidget* parent) : QFrame(parent) {}
void NocturnePaperPanel::paintEvent(QPaintEvent*) {
    QPainter p(this); p.fillRect(rect(), NocturneUi::theme().paper);
}

NocturneNoteDelegate::NocturneNoteDelegate(QObject* parent) : QStyledItemDelegate(parent) {}
void NocturneNoteDelegate::paint(QPainter* p, const QStyleOptionViewItem& option,
                                 const QModelIndex& index) const
{
    using namespace NocturneUi;
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    const bool selected = option.state.testFlag(QStyle::State_Selected);
    const QRectF row = QRectF(option.rect).adjusted(0, 2, -2, -2);
    if (selected || option.state.testFlag(QStyle::State_MouseOver)) {
        p->setPen(Qt::NoPen);
        p->setBrush(selected ? theme().selected : theme().hover);
        p->drawRoundedRect(row, 5, 5);
    }
    if (selected) {
        p->setPen(Qt::NoPen); p->setBrush(theme().accent);
        p->drawRoundedRect(QRectF(row.left(), row.top() + 5, 3, row.height() - 10), 1.5, 1.5);
    }
    const qreal left = row.left() + 15;
    const qreal width = row.width() - 30;
    auto text = [&](const QString& value, int size, int y, int height, const QColor& color, bool bold = false) {
        QFont f(sansFamily()); f.setPixelSize(size); f.setWeight(bold ? QFont::DemiBold : QFont::Normal);
        p->setFont(f); p->setPen(color);
        p->drawText(QRectF(left, row.top() + y, width, height), Qt::AlignLeft | Qt::AlignVCenter,
                    p->fontMetrics().elidedText(value, Qt::ElideRight, qMax(1, qRound(width))));
    };
    text(index.data(NoteTitleRole).toString(), 14, 10, 24, theme().text, selected);
    QString excerpt = index.data(NoteExcerptRole).toString().simplified();
    text(excerpt.isEmpty() ? QStringLiteral("还没有正文，写下第一句…") : excerpt,
         12, 38, 22, theme().muted);
    const bool sticky = index.data(NoteKindRole).toString() == QStringLiteral("sticky");
    QString group = sticky ? QStringLiteral("桌面便签") : index.data(NoteGroupRole).toString();
    QFont meta(sansFamily()); meta.setPixelSize(10); p->setFont(meta); p->setPen(theme().muted);
    p->drawText(QRectF(left, row.top() + 70, width * 0.45, 18), Qt::AlignVCenter,
                p->fontMetrics().elidedText(group, Qt::ElideRight, qRound(width * 0.45)));
    p->drawText(QRectF(left + width * 0.45, row.top() + 70, width * 0.55, 18),
                Qt::AlignVCenter | Qt::AlignRight, index.data(NoteTimeRole).toString());
    if (option.state.testFlag(QStyle::State_HasFocus)) {
        p->setBrush(Qt::NoBrush); p->setPen(QPen(theme().accent, 1, Qt::DotLine));
        p->drawRoundedRect(row.adjusted(1, 1, -1, -1), 5, 5);
    }
    p->restore();
}
QSize NocturneNoteDelegate::sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const { return {0, 104}; }

NocturneTodoDelegate::NocturneTodoDelegate(QObject* parent) : QStyledItemDelegate(parent) {}
void NocturneTodoDelegate::paint(QPainter* p, const QStyleOptionViewItem& option,
                                 const QModelIndex& index) const
{
    using namespace NocturneUi;
    p->save(); p->setRenderHint(QPainter::Antialiasing);
    const bool done = index.data(Qt::CheckStateRole).toInt() == Qt::Checked;
    const QRectF row = QRectF(option.rect).adjusted(0, 1, -1, -1);
    if (option.state.testFlag(QStyle::State_MouseOver)) p->fillRect(row, theme().hover);
    const QPointF center(row.left() + 12, row.center().y());
    p->setPen(QPen(done ? theme().accent : theme().muted, 1.3));
    p->setBrush(done ? QBrush(theme().accent) : Qt::NoBrush);
    p->drawEllipse(center, 8, 8);
    if (done) {
        p->setPen(QPen(theme().accentText, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        QPainterPath check; check.moveTo(center + QPointF(-4, 0));
        check.lineTo(center + QPointF(-1, 3)); check.lineTo(center + QPointF(4, -3)); p->drawPath(check);
    }
    QFont f(sansFamily()); f.setPixelSize(13); f.setStrikeOut(done);
    p->setFont(f); p->setPen(done ? theme().muted : theme().text);
    const QRectF textRect = row.adjusted(32, 4, -5, -4);
    p->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter,
                p->fontMetrics().elidedText(index.data().toString(), Qt::ElideRight, qMax(1, qRound(textRect.width()))));
    if (option.state.testFlag(QStyle::State_HasFocus)) {
        p->setPen(QPen(theme().accent, 1, Qt::DotLine)); p->setBrush(Qt::NoBrush);
        p->drawRoundedRect(row, 4, 4);
    }
    p->restore();
}
QSize NocturneTodoDelegate::sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const { return {0, 48}; }

bool NocturneTodoDelegate::editorEvent(QEvent* event, QAbstractItemModel* model,
                                       const QStyleOptionViewItem& option, const QModelIndex& index)
{
    if (!model || !(index.flags() & Qt::ItemIsEnabled) || !(index.flags() & Qt::ItemIsUserCheckable))
        return false;
    bool toggle = false;
    if (event->type() == QEvent::MouseButtonRelease) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        toggle = mouse->button() == Qt::LeftButton && option.rect.contains(mouse->position().toPoint());
    } else if (event->type() == QEvent::KeyPress) {
        const int key = static_cast<QKeyEvent*>(event)->key();
        toggle = key == Qt::Key_Space || key == Qt::Key_Select;
    }
    if (toggle)
        return model->setData(index, index.data(Qt::CheckStateRole).toInt() == Qt::Checked
                                    ? Qt::Unchecked : Qt::Checked, Qt::CheckStateRole);
    return false;
}
