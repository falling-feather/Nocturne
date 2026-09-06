#pragma once

#include <QImage>
#include <QStringList>
#include <QTextEdit>
#include <QHash>
#include <QTextObjectInterface>

class QMimeData;
class QSyntaxHighlighter;
class QTimer;

class NoteEditor final : public QTextEdit, public QTextObjectInterface
{
    Q_OBJECT
    Q_INTERFACES(QTextObjectInterface)

public:
    explicit NoteEditor(QWidget* parent = nullptr);
    void refreshTheme();
    void applyHeadingLevel(int level);
    void applyQuote();
    void applyCodeBlock();
    void insertMarkdownText(const QString& markdown);
    QString markdownForExport() const;
    QString markSelectionTodo(const QString& anchor);
    void clearSelectionTodo();
    bool locateTodo(const QString& anchor);
    void setTodoStates(const QHash<QString, bool>& states);
    void setHtml(const QString& html);
    void repairImageBlocks();
    void setImageCaption(int position, const QString& caption);
    void createTable(int rows, int columns);
    void showTableDialog();
    bool selectionToTable(QString* error = nullptr);
    void showSelectionToTable();
    QSizeF intrinsicSize(QTextDocument*, int, const QTextFormat&) override;
    void drawObject(QPainter*, const QRectF&, QTextDocument*, int, const QTextFormat&) override;

signals:
    void imagePasted(const QImage& image);
    void imageFilesDropped(const QStringList& files);
    void selectionTodoRequested();
    void filesImportRequested(const QStringList& files);
    void todoActivated(const QString& anchor);

protected:
    bool canInsertFromMimeData(const QMimeData* source) const override;
    void insertFromMimeData(const QMimeData* source) override;
    void keyPressEvent(QKeyEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    QSyntaxHighlighter* m_contrastHighlighter = nullptr;
    QPoint m_linkPressPosition;
    QString m_pressedTodo;
    int m_pressedImage = -1;
    QTimer* m_imageClickTimer = nullptr;
    QTextCursor m_pendingImage;
    int imageAt(const QPoint& point) const;
    QImage imageResource(const QTextFormat& format) const;
};
