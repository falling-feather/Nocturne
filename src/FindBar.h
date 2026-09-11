#pragma once
#include <QFrame>
#include <QTextCursor>
#include <QTextDocument>
class QTextEdit;
class QPlainTextEdit;
class QLineEdit;
class QLabel;
class QCheckBox;
class QPushButton;

class FindBar final : public QFrame
{
    Q_OBJECT
public:
    FindBar(QTextEdit* rich, QPlainTextEdit* source, QWidget* parent = nullptr);
    void open(bool replace = false);
    void setSourceMode(bool source);
    bool findNext(bool backwards = false);
    int replaceAll();

private:
    QTextDocument* document() const;
    QTextCursor cursor() const;
    void select(const QTextCursor& cursor);
    QTextDocument::FindFlags flags() const;
    bool readOnly() const;
    QTextEdit* m_rich;
    QPlainTextEdit* m_source;
    bool m_sourceMode = false;
    QLineEdit* m_query;
    QLineEdit* m_replacement;
    QCheckBox* m_case;
    QLabel* m_status;
    QWidget* m_replaceRow;
};
