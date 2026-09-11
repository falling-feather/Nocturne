#pragma once
#include <QFrame>
class NoteEditor;
class QTreeWidget;
class QLabel;
class QTimer;

class DocumentOutline final : public QFrame
{
    Q_OBJECT
public:
    explicit DocumentOutline(NoteEditor* editor, QWidget* parent = nullptr);
    void rebuild(bool newNote = false);
    int headingCount() const;
signals:
    void headingsChanged(int count);
    void closeRequested();
private:
    void highlightCurrent();
    NoteEditor* m_editor;
    QTreeWidget* m_tree;
    QLabel* m_empty;
    QTimer* m_timer;
    int m_count = 0;
};
