#include "DocumentOutline.h"
#include "NoteEditor.h"
#include <QLabel>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QTextBlock>
#include <QTimer>
#include <QVBoxLayout>
#include <QScrollBar>
#include <QSet>
#include <QSignalBlocker>

namespace {
class HeadingItem final : public QTreeWidgetItem {
public:
    explicit HeadingItem(const QTextBlock& block) : cursor(block), level(block.blockFormat().headingLevel()) {}
    QTextCursor cursor;
    int level;
};
}
DocumentOutline::DocumentOutline(NoteEditor* editor, QWidget* parent) : QFrame(parent), m_editor(editor)
{
    setObjectName(QStringLiteral("documentOutline")); setFixedWidth(220);
    auto* layout = new QVBoxLayout(this); layout->setContentsMargins(12, 18, 10, 12);
    auto* header = new QHBoxLayout;
    header->addWidget(new QLabel(QStringLiteral("本文大纲"), this)); header->addStretch();
    auto* close = new QToolButton(this); close->setText(QStringLiteral("×")); close->setToolTip(QStringLiteral("收起大纲"));
    header->addWidget(close); layout->addLayout(header);
    connect(close, &QToolButton::clicked, this, &DocumentOutline::closeRequested);
    m_tree = new QTreeWidget(this); m_tree->setObjectName(QStringLiteral("headingTree"));
    m_tree->setHeaderHidden(true); m_tree->setIndentation(14); m_tree->setUniformRowHeights(true);
    m_tree->setAnimated(false); m_tree->setExpandsOnDoubleClick(false); m_tree->setFrameShape(QFrame::NoFrame);
    m_tree->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff); layout->addWidget(m_tree, 1);
    m_empty = new QLabel(QStringLiteral("为段落设置 H1—H6 标题，\n即可在这里跳转。"), this);
    m_empty->setWordWrap(true); layout->addWidget(m_empty);
    m_timer = new QTimer(this); m_timer->setSingleShot(true); m_timer->setInterval(180);
    connect(editor->document(), &QTextDocument::contentsChanged, m_timer, [this] { m_timer->start(); });
    connect(m_timer, &QTimer::timeout, this, [this] { rebuild(); });
    connect(editor, &QTextEdit::cursorPositionChanged, this, &DocumentOutline::highlightCurrent);
    auto jump = [this](QTreeWidgetItem* raw) {
        const auto* heading = static_cast<HeadingItem*>(raw);
        if (!heading || !heading->cursor.block().isValid()) return;
        auto cursor = heading->cursor; cursor.movePosition(QTextCursor::StartOfBlock);
        m_editor->setTextCursor(cursor); m_editor->ensureCursorVisible(); m_editor->setFocus();
    };
    connect(m_tree, &QTreeWidget::itemClicked, this, jump);
    connect(m_tree, &QTreeWidget::itemActivated, this, jump);
}
int DocumentOutline::headingCount() const { return m_count; }
void DocumentOutline::rebuild(bool newNote)
{
    m_timer->stop();
    QSet<int> collapsed;
    const int scroll = newNote ? 0 : m_tree->verticalScrollBar()->value();
    if (!newNote) for (QTreeWidgetItemIterator it(m_tree); *it; ++it) {
        auto* heading = static_cast<HeadingItem*>(*it);
        if (heading->childCount() && !heading->isExpanded()) collapsed.insert(heading->cursor.block().position());
    }
    m_tree->clear(); m_count = 0; QList<HeadingItem*> stack, headings;
    for (auto block = m_editor->document()->begin(); block.isValid(); block = block.next()) {
        const int level = block.blockFormat().headingLevel();
        if (level < 1 || level > 6 || block.text().trimmed().isEmpty()) continue;
        auto* heading = new HeadingItem(block); heading->setText(0, block.text().simplified());
        heading->setToolTip(0, QStringLiteral("H%1 · %2").arg(level).arg(block.text()));
        heading->setSizeHint(0, QSize(0, 32)); heading->setData(0, Qt::UserRole, level);
        while (!stack.isEmpty() && stack.last()->level >= level) stack.removeLast();
        if (stack.isEmpty()) m_tree->addTopLevelItem(heading); else stack.last()->addChild(heading);
        stack.append(heading); headings.append(heading); ++m_count;
    }
    for (auto it = headings.crbegin(); it != headings.crend(); ++it)
        (*it)->setExpanded(!collapsed.contains((*it)->cursor.block().position()));
    m_empty->setVisible(m_count == 0); m_tree->setVisible(m_count > 0);
    m_tree->verticalScrollBar()->setValue(scroll); highlightCurrent(); emit headingsChanged(m_count);
}
void DocumentOutline::highlightCurrent()
{
    HeadingItem* current = nullptr;
    const int position = m_editor->textCursor().position();
    for (QTreeWidgetItemIterator it(m_tree); *it; ++it) {
        auto* heading = static_cast<HeadingItem*>(*it);
        if (heading->cursor.block().position() > position) break;
        current = heading;
    }
    const QSignalBlocker blocker(m_tree);
    for (auto* parent = current ? current->parent() : nullptr; parent; parent = parent->parent())
        if (!parent->isExpanded()) current = static_cast<HeadingItem*>(parent);
    m_tree->selectionModel()->setCurrentIndex(m_tree->indexFromItem(current), QItemSelectionModel::ClearAndSelect);
}
