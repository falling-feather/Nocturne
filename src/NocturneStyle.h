#pragma once

#include <QFrame>
#include <QStyledItemDelegate>
#include <QWidget>

class QEvent;
class QPaintEvent;

namespace NocturneUi {

enum NoteDataRole {
    NoteTitleRole = Qt::UserRole + 10,
    NoteExcerptRole,
    NoteGroupRole,
    NoteTimeRole,
    NoteKindRole
};

QString serifFamily();
QString sansFamily();

} // namespace NocturneUi

class NocturneBackdrop final : public QWidget
{
public:
    explicit NocturneBackdrop(QWidget* parent = nullptr);

protected:
    void paintEvent(QPaintEvent* event) override;
};

class NocturneGlassPanel final : public QFrame
{
public:
    explicit NocturneGlassPanel(bool decorativeRipples, QWidget* parent = nullptr);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    bool m_decorativeRipples = false;
};

class NocturnePaperPanel final : public QFrame
{
public:
    explicit NocturnePaperPanel(QWidget* parent = nullptr);

protected:
    void paintEvent(QPaintEvent* event) override;
};

class NocturneNoteDelegate final : public QStyledItemDelegate
{
public:
    explicit NocturneNoteDelegate(QObject* parent = nullptr);

    void paint(QPainter* painter,
               const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option,
                   const QModelIndex& index) const override;
};

class NocturneTodoDelegate final : public QStyledItemDelegate
{
public:
    explicit NocturneTodoDelegate(QObject* parent = nullptr);

    void paint(QPainter* painter,
               const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option,
                   const QModelIndex& index) const override;
    bool editorEvent(QEvent* event,
                     QAbstractItemModel* model,
                     const QStyleOptionViewItem& option,
                     const QModelIndex& index) override;
};
