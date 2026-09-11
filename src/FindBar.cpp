#include "FindBar.h"
#include <QTextEdit>
#include <QPlainTextEdit>
#include <QLineEdit>
#include <QLabel>
#include <QCheckBox>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

FindBar::FindBar(QTextEdit* rich, QPlainTextEdit* source, QWidget* parent)
    : QFrame(parent)
    , m_rich(rich)
    , m_source(source)
{
    setObjectName("findBar");
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 6, 0, 6);
    layout->setSpacing(4);
    auto* row = new QHBoxLayout;
    layout->addLayout(row);
    m_query = new QLineEdit(this);
    m_query->setObjectName("findQuery");
    m_query->setPlaceholderText(QStringLiteral("在当前笔记中查找"));
    row->addWidget(m_query, 1);
    auto* previous = new QToolButton(this);
    previous->setText("↑");
    previous->setToolTip(QStringLiteral("上一个"));
    row->addWidget(previous);
    auto* next = new QToolButton(this);
    next->setText("↓");
    next->setToolTip(QStringLiteral("下一个"));
    row->addWidget(next);
    m_case = new QCheckBox(QStringLiteral("区分大小写"), this);
    row->addWidget(m_case);
    auto* close = new QToolButton(this);
    close->setText("×");
    row->addWidget(close);
    m_replaceRow = new QWidget(this);
    auto* replaceLayout = new QHBoxLayout(m_replaceRow);
    replaceLayout->setContentsMargins(0, 0, 0, 0);
    m_replacement = new QLineEdit(m_replaceRow);
    m_replacement->setObjectName("replaceText");
    m_replacement->setPlaceholderText(QStringLiteral("替换为"));
    replaceLayout->addWidget(m_replacement, 1);
    auto* replace = new QPushButton(QStringLiteral("替换当前"), m_replaceRow);
    replaceLayout->addWidget(replace);
    auto* all = new QPushButton(QStringLiteral("替换全部"), m_replaceRow);
    all->setObjectName("replaceAllButton");
    replaceLayout->addWidget(all);
    layout->addWidget(m_replaceRow);
    m_status = new QLabel(QStringLiteral("仅当前笔记；替换可撤销"), this);
    layout->addWidget(m_status);
    connect(next, &QToolButton::clicked, this, [this] { findNext(); });
    connect(previous, &QToolButton::clicked, this, [this] { findNext(true); });
    connect(m_query, &QLineEdit::returnPressed, this, [this] { findNext(); });
    connect(close, &QToolButton::clicked, this, &QWidget::hide);
    connect(all, &QPushButton::clicked, this, [this] { replaceAll(); });
    connect(replace, &QPushButton::clicked, this,
        [this]
        {
            if (readOnly() || m_query->text().isEmpty())
                return;
            auto selection = cursor();
            if (selection.selectedText().compare(
                    m_query->text(), m_case->isChecked() ? Qt::CaseSensitive : Qt::CaseInsensitive)
                != 0)
            {
                findNext();
                return;
            }
            selection.insertText(m_replacement->text());
            select(selection);
            findNext();
        });
    hide();
}
QTextDocument* FindBar::document() const
{
    return m_sourceMode ? m_source->document() : m_rich->document();
}
QTextCursor FindBar::cursor() const
{
    return m_sourceMode ? m_source->textCursor() : m_rich->textCursor();
}
bool FindBar::readOnly() const
{
    return m_sourceMode ? m_source->isReadOnly() : m_rich->isReadOnly();
}
void FindBar::select(const QTextCursor& value)
{
    if (m_sourceMode)
    {
        m_source->setTextCursor(value);
        m_source->ensureCursorVisible();
    }
    else
    {
        m_rich->setTextCursor(value);
        m_rich->ensureCursorVisible();
    }
}
QTextDocument::FindFlags FindBar::flags() const
{
    return m_case->isChecked() ? QTextDocument::FindCaseSensitively : QTextDocument::FindFlags();
}
void FindBar::setSourceMode(bool source)
{
    m_sourceMode = source;
    m_replaceRow->setEnabled(!readOnly());
}
void FindBar::open(bool replace)
{
    m_replaceRow->setVisible(replace);
    m_replaceRow->setEnabled(!readOnly());
    if (cursor().hasSelection() && cursor().selectedText().size() < 160)
        m_query->setText(cursor().selectedText());
    show();
    m_query->setFocus();
    m_query->selectAll();
}
bool FindBar::findNext(bool backwards)
{
    if (m_query->text().isEmpty())
        return false;
    auto options = flags();
    if (backwards)
        options |= QTextDocument::FindBackward;
    auto found = document()->find(m_query->text(), cursor(), options);
    if (found.isNull())
        found = document()->find(m_query->text(), backwards ? document()->characterCount() - 1 : 0, options);
    m_status->setText(found.isNull() ? QStringLiteral("当前笔记没有匹配内容")
                                     : QStringLiteral("已定位匹配内容 · 仅当前笔记"));
    if (found.isNull())
        return false;
    select(found);
    return true;
}
int FindBar::replaceAll()
{
    if (readOnly() || m_query->text().isEmpty())
        return 0;
    QTextCursor edit(document());
    edit.beginEditBlock();
    int count = 0;
    auto found = document()->find(m_query->text(), 0, flags());
    while (!found.isNull())
    {
        found.insertText(m_replacement->text());
        ++count;
        found = document()->find(m_query->text(), found.position(), flags());
    }
    edit.endEditBlock();
    m_status->setText(QStringLiteral("当前笔记已替换 %1 处 · Ctrl+Z 撤销").arg(count));
    return count;
}
