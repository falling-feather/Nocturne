#include "NocturneStyle.h"

#include <QAbstractItemModel>
#include <QEvent>
#include <QFontDatabase>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QStyle>
#include <QStyleOptionViewItem>

#include <initializer_list>

namespace {

QString firstInstalledFamily(std::initializer_list<QString> candidates)
{
    const QFontDatabase database;
    const QStringList installed = database.families();
    for (const QString& candidate : candidates) {
        if (installed.contains(candidate, Qt::CaseInsensitive))
            return candidate;
    }
    return QStringLiteral("Microsoft YaHei UI");
}

} // namespace

namespace NocturneUi {

QString serifFamily()
{
    static const QString family = firstInstalledFamily({
        QStringLiteral("FZShuSong-Z01S"),
        QStringLiteral("Source Han Serif SC"),
        QStringLiteral("Noto Serif CJK SC"),
        QStringLiteral("STZhongsong"),
        QStringLiteral("SimSun")
    });
    return family;
}

QString sansFamily()
{
    static const QString family = firstInstalledFamily({
        QStringLiteral("Microsoft YaHei UI"),
        QStringLiteral("Source Han Sans SC"),
        QStringLiteral("Noto Sans CJK SC"),
        QStringLiteral("Microsoft YaHei")
    });
    return family;
}

} // namespace NocturneUi

NocturneBackdrop::NocturneBackdrop(QWidget* parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_StyledBackground, false);
    setAutoFillBackground(false);
}

void NocturneBackdrop::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(), QColor(QStringLiteral("#071422")));

    // 用少量无动画线稿保留夜色气氛，避免背景位图的解码与缩放缓存常驻。
    for (int index = 0; index < 17; ++index) {
        const qreal x = 18.0 + ((index * 149) % 977) / 977.0 * qMax(1, width() - 36);
        const qreal y = 12.0 + ((index * 71) % 251) / 251.0
            * qMax<qreal>(1.0, height() * 0.28);
        const qreal radius = index % 4 == 0 ? 1.1 : 0.65;
        painter.setBrush(QColor(235, 203, 139, 48 + (index % 3) * 11));
        painter.setPen(Qt::NoPen);
        painter.drawEllipse(QPointF(x, y), radius, radius);
    }

    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QColor(218, 180, 105, 72), 1.0));
    const int waterTop = qRound(height() * 0.68);
    for (int index = 0; index < 9; ++index) {
        const int y = waterTop + index * qMax(18, height() / 42);
        const int inset = 24 + ((index * 83) % qMax(25, width() / 5));
        const int span = qMax(72, width() / 7 + ((index * 47) % qMax(73, width() / 4)));
        painter.drawLine(inset, y, qMin(width() - 18, inset + span), y);
        const int right = width() - inset / 2;
        painter.drawLine(qMax(18, right - span / 2), y + 8, right, y + 8);
    }

    painter.setPen(QPen(QColor(194, 79, 62, 78), 1.0));
    painter.drawLine(qRound(width() * 0.72), waterTop - 22,
                     qRound(width() * 0.79), waterTop - 22);

    const qreal motifTop = qMax<qreal>(110.0, height() * 0.48);
    painter.setPen(QPen(QColor(224, 190, 119, 76), 1.1));
    painter.drawArc(QRectF(38.0, motifTop, 242.0, 242.0), 36 * 16, 242 * 16);
    QPainterPath boat;
    boat.moveTo(45.0, motifTop + 205.0);
    boat.cubicTo(105.0, motifTop + 226.0,
                 202.0, motifTop + 230.0,
                 276.0, motifTop + 192.0);
    boat.cubicTo(231.0, motifTop + 242.0,
                 119.0, motifTop + 249.0,
                 45.0, motifTop + 205.0);
    painter.drawPath(boat);
    painter.drawLine(QPointF(169.0, motifTop + 83.0),
                     QPointF(169.0, motifTop + 218.0));
}

NocturneGlassPanel::NocturneGlassPanel(bool decorativeRipples, QWidget* parent)
    : QFrame(parent)
    , m_decorativeRipples(decorativeRipples)
{
    setAttribute(Qt::WA_StyledBackground, false);
    setAutoFillBackground(false);
}

void NocturneGlassPanel::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)
    QPainter painter(this);
    painter.fillRect(rect(), QColor(6, 20, 35, 218));

    painter.setPen(QPen(QColor(214, 183, 116, 52), 1.0));
    if (objectName() == QStringLiteral("navigation"))
        painter.drawLine(width() - 1, 0, width() - 1, height());
    else
        painter.drawLine(0, 0, 0, height());

    if (m_decorativeRipples) {
        painter.setPen(QPen(QColor(202, 168, 99, 84), 1.0));
        painter.drawLine(width() - 88, height() - 28, width() - 26, height() - 28);
        painter.setPen(QPen(QColor(QStringLiteral("#A84E3D")), 2.0));
        painter.drawLine(width() - 25, height() - 28, width() - 17, height() - 28);
    }
}

NocturnePaperPanel::NocturnePaperPanel(QWidget* parent)
    : QFrame(parent)
{
    setAttribute(Qt::WA_StyledBackground, false);
    setAutoFillBackground(false);
}

void NocturnePaperPanel::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)
    QPainter painter(this);
    painter.fillRect(rect(), QColor(QStringLiteral("#F4E8D0")));

    painter.setPen(QPen(QColor(107, 76, 42, 11), 1.0));
    for (int i = 0; i < 28; ++i) {
        const qreal x = 10.0 + ((i * 97) % 983) / 983.0 * qMax(1, width() - 20);
        const qreal y = 8.0 + ((i * 193) % 977) / 977.0 * qMax(1, height() - 16);
        const qreal length = 8.0 + ((i * 31) % 29);
        painter.drawLine(QPointF(x, y), QPointF(qMin<qreal>(width() - 8, x + length), y + 0.4));
    }

    painter.setPen(QPen(QColor(112, 79, 43, 52), 1.0));
    painter.drawLine(0, 0, 0, height());
    painter.drawLine(width() - 1, 0, width() - 1, height());
}

NocturneNoteDelegate::NocturneNoteDelegate(QObject* parent)
    : QStyledItemDelegate(parent)
{
}

void NocturneNoteDelegate::paint(QPainter* painter,
                                 const QStyleOptionViewItem& option,
                                 const QModelIndex& index) const
{
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);

    const bool selected = option.state.testFlag(QStyle::State_Selected);
    const bool hovered = option.state.testFlag(QStyle::State_MouseOver);
    const QRectF row = QRectF(option.rect).adjusted(0.0, 0.0, -1.0, -1.0);
    if (selected) {
        painter->fillRect(row, QColor(21, 42, 61, 218));
        painter->setPen(QPen(QColor(QStringLiteral("#E2BC6B")), 3.0));
        painter->drawLine(QPointF(row.left() + 1.5, row.top() + 8.0),
                          QPointF(row.left() + 1.5, row.bottom() - 8.0));
    } else if (hovered) {
        painter->fillRect(row, QColor(24, 45, 65, 154));
    }
    painter->setPen(QPen(QColor(185, 159, 109, 36), 1.0));
    painter->drawLine(QPointF(row.left() + 12.0, row.bottom()),
                      QPointF(row.right() - 10.0, row.bottom()));

    const QString title = index.data(NocturneUi::NoteTitleRole).toString();
    const QString group = index.data(NocturneUi::NoteGroupRole).toString();
    const QString time = index.data(NocturneUi::NoteTimeRole).toString();
    const QString kind = index.data(NocturneUi::NoteKindRole).toString();
    const qreal textLeft = row.left() + 15.0;
    const qreal textWidth = row.width() - 36.0;

    QFont titleFont(NocturneUi::serifFamily(), 12);
    titleFont.setWeight(QFont::DemiBold);
    painter->setFont(titleFont);
    painter->setPen(selected ? QColor(QStringLiteral("#F3D28E"))
                             : QColor(QStringLiteral("#E6D8B8")));
    const QString elidedTitle = painter->fontMetrics().elidedText(title, Qt::ElideRight,
                                                                  qMax(30, qRound(textWidth)));
    painter->drawText(QRectF(textLeft, row.top() + 8.0, textWidth, 24.0),
                      Qt::AlignLeft | Qt::AlignVCenter, elidedTitle);

    QFont metaFont(NocturneUi::sansFamily(), 9);
    painter->setFont(metaFont);
    painter->setPen(selected ? QColor(201, 182, 143) : QColor(143, 157, 174));
    const QString groupLabel = kind == QStringLiteral("sticky")
        ? QStringLiteral("快捷便签")
        : (group.isEmpty() ? QStringLiteral("未分组") : group);
    painter->drawText(QRectF(textLeft, row.top() + 33.0, textWidth, 19.0),
                      Qt::AlignLeft | Qt::AlignVCenter,
                      painter->fontMetrics().elidedText(groupLabel, Qt::ElideRight,
                                                        qMax(30, qRound(textWidth))));
    painter->setPen(QColor(130, 145, 164));
    painter->drawText(QRectF(textLeft, row.top() + 52.0, textWidth, 18.0),
                      Qt::AlignLeft | Qt::AlignVCenter, time);

    if (kind == QStringLiteral("sticky")) {
        painter->setBrush(QColor(QStringLiteral("#B65C49")));
        painter->setPen(Qt::NoPen);
        painter->drawRect(QRectF(row.right() - 17.0, row.top() + 13.0, 5.0, 5.0));
    }

    painter->restore();
}

QSize NocturneNoteDelegate::sizeHint(const QStyleOptionViewItem& option,
                                     const QModelIndex& index) const
{
    Q_UNUSED(option)
    Q_UNUSED(index)
    return QSize(0, 80);
}

NocturneTodoDelegate::NocturneTodoDelegate(QObject* parent)
    : QStyledItemDelegate(parent)
{
}

void NocturneTodoDelegate::paint(QPainter* painter,
                                 const QStyleOptionViewItem& option,
                                 const QModelIndex& index) const
{
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);

    const bool done = index.data(Qt::CheckStateRole).toInt() == Qt::Checked;
    const bool hovered = option.state.testFlag(QStyle::State_MouseOver);
    const QRectF row = QRectF(option.rect).adjusted(0.0, 0.0, -1.0, -1.0);
    if (hovered)
        painter->fillRect(row, QColor(24, 45, 64, 164));
    painter->setPen(QPen(QColor(190, 163, 109, 38), 1.0));
    painter->drawLine(QPointF(row.left() + 8.0, row.bottom()),
                      QPointF(row.right() - 8.0, row.bottom()));

    const QPointF center(row.left() + 18.0, row.center().y());
    painter->setPen(QPen(QColor(224, 191, 119, done ? 170 : 210), 1.4));
    painter->setBrush(done ? QColor(QStringLiteral("#D9BC7A")) : Qt::NoBrush);
    painter->drawEllipse(center, 9.0, 9.0);
    if (done) {
        painter->setPen(QPen(QColor(QStringLiteral("#14243A")), 1.7,
                             Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter->drawLine(QPointF(center.x() - 4.0, center.y()),
                          QPointF(center.x() - 1.0, center.y() + 3.0));
        painter->drawLine(QPointF(center.x() - 1.0, center.y() + 3.0),
                          QPointF(center.x() + 5.0, center.y() - 4.0));
    }

    QFont textFont(NocturneUi::sansFamily(), 10);
    textFont.setStrikeOut(done);
    painter->setFont(textFont);
    painter->setPen(done ? QColor(130, 143, 157) : QColor(220, 211, 187));
    const QRectF textRect(row.left() + 38.0, row.top(), row.width() - 50.0, row.height());
    painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter,
                      painter->fontMetrics().elidedText(index.data(Qt::DisplayRole).toString(),
                                                        Qt::ElideRight,
                                                        qMax(30, qRound(textRect.width()))));

    painter->restore();
}

QSize NocturneTodoDelegate::sizeHint(const QStyleOptionViewItem& option,
                                     const QModelIndex& index) const
{
    Q_UNUSED(option)
    Q_UNUSED(index)
    return QSize(0, 52);
}

bool NocturneTodoDelegate::editorEvent(QEvent* event,
                                       QAbstractItemModel* model,
                                       const QStyleOptionViewItem& option,
                                       const QModelIndex& index)
{
    if (!model || !(index.flags() & Qt::ItemIsEnabled)
        || !(index.flags() & Qt::ItemIsUserCheckable)) {
        return false;
    }

    if (event->type() == QEvent::MouseButtonRelease) {
        const auto* mouseEvent = static_cast<QMouseEvent*>(event);
        if (mouseEvent->button() == Qt::LeftButton && option.rect.contains(mouseEvent->position().toPoint())) {
            const Qt::CheckState state = static_cast<Qt::CheckState>(
                index.data(Qt::CheckStateRole).toInt());
            return model->setData(index,
                                  state == Qt::Checked ? Qt::Unchecked : Qt::Checked,
                                  Qt::CheckStateRole);
        }
    }
    if (event->type() == QEvent::KeyPress) {
        const auto* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->key() == Qt::Key_Space || keyEvent->key() == Qt::Key_Select) {
            const Qt::CheckState state = static_cast<Qt::CheckState>(
                index.data(Qt::CheckStateRole).toInt());
            return model->setData(index,
                                  state == Qt::Checked ? Qt::Unchecked : Qt::Checked,
                                  Qt::CheckStateRole);
        }
    }
    return false;
}
