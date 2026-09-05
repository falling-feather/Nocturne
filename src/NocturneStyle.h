#pragma once

#include <QFrame>
#include <QColor>
#include <QIcon>
#include <QComboBox>
#include <QStyledItemDelegate>
#include <QWidget>

class QEvent;
class QPaintEvent;

namespace NocturneUi {

// Three small palettes share the same native widget tree and vector icons.
struct Theme {
    QString id, name;
    QColor rail, titleBar, sidebar, paper, panel, border;
    QColor text, muted, accent, accentText, selected, hover, success, heading;
};

enum class Glyph {
    Notebook, Sticky, Book, Todo, Settings, Palette, Search, Folder,
    Plus, Close, Minimize, Maximize, Restore, Focus, Image, Bullet,
    Numbered, Pin, More, Trash, Chevron, ArrowLeft
};

const Theme& theme();
void setTheme(const QString& id);
QString styleSheet();
void applyPalette();
QIcon icon(Glyph glyph, const QColor& color = QColor());
void setGlyph(QObject* object, Glyph glyph);
void refreshIcons(QWidget* root);

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

class NocturneComboBox final : public QComboBox
{
public:
    using QComboBox::QComboBox;
protected:
    void paintEvent(QPaintEvent* event) override;
};

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
