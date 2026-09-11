#pragma once
#include "Database.h"
#include <QTreeWidget>
#include <QHash>
#include <QSet>

class NotebookItem final : public QTreeWidgetItem {
public:
    using QTreeWidgetItem::QTreeWidgetItem;
    QVariant data(int role) const { return QTreeWidgetItem::data(0, role); }
    void setData(int role, const QVariant& value) { QTreeWidgetItem::setData(0, role, value); }
    void setText(const QString& text) { QTreeWidgetItem::setText(0, text.section('\n', 0, 0)); setToolTip(0, text); }
    void setSizeHint(const QSize& size) { QTreeWidgetItem::setSizeHint(0, size); }
    bool isFolder() const { return data(Qt::UserRole + 100).toBool(); }
};

class NotebookTree final : public QTreeWidget {
    Q_OBJECT
public:
    explicit NotebookTree(QWidget* parent = nullptr);
    void rebuildFolders(const QList<FolderRecord>& folders);
    NotebookItem* addNote(const QString& text, qint64 folderId);
    void finishRebuild(bool searching);
    void refreshTheme();
    int count() const { return m_notes.size(); }
    NotebookItem* item(int row) const { return m_notes.value(row); }
    NotebookItem* currentItem() const { return static_cast<NotebookItem*>(QTreeWidget::currentItem()); }
    NotebookItem* itemAt(const QPoint& point) const { return static_cast<NotebookItem*>(QTreeWidget::itemAt(point)); }
    void setCurrentRow(int row, bool reveal = true);
signals:
    void moveRequested(bool folder, qint64 id, qint64 parentId);
    void filesDropped(const QStringList& files, qint64 parentId);
protected:
    void dropEvent(QDropEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
private:
    QHash<qint64, NotebookItem*> m_folders;
    QList<NotebookItem*> m_notes;
    QSet<qint64> m_expanded;
    bool m_restoring = false;
    QIcon m_noteIcon;
};
