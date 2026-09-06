#include "NotebookTree.h"
#include "NocturneStyle.h"
#include <QHeaderView>
#include <QSettings>
#include <QDropEvent>
#include <QMimeData>
#include <QUrl>
#include <functional>

NotebookTree::NotebookTree(QWidget* parent) : QTreeWidget(parent)
{
    setColumnCount(2); setHeaderHidden(true); setRootIsDecorated(true);
    header()->setStretchLastSection(false);
    setIndentation(16); setUniformRowHeights(true); setAnimated(false);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setDragEnabled(true); setAcceptDrops(true); setDropIndicatorShown(true);
    setDragDropMode(QAbstractItemView::InternalMove); setDefaultDropAction(Qt::MoveAction);
    header()->setSectionResizeMode(0, QHeaderView::Stretch);
    header()->setSectionResizeMode(1, QHeaderView::Fixed); header()->resizeSection(1, 32);
    m_hasSavedExpansion = QSettings().contains("navigation/expandedFolders");
    for (const QString& value : QSettings().value("navigation/expandedFolders").toStringList())
        m_expanded.insert(value.toLongLong());
    auto remember = [this](QTreeWidgetItem* raw) {
        auto* item = static_cast<NotebookItem*>(raw);
        if (m_restoring || !item->isFolder()) return;
        const qint64 id = item->data(Qt::UserRole).toLongLong();
        if (item->isExpanded()) m_expanded.insert(id); else m_expanded.remove(id);
        QStringList values;
        for (qint64 expanded : m_expanded) values.append(QString::number(expanded));
        QSettings().setValue("navigation/expandedFolders", values);
        m_hasSavedExpansion = true;
    };
    connect(this, &QTreeWidget::itemExpanded, this, remember);
    connect(this, &QTreeWidget::itemCollapsed, this, remember);
}

void NotebookTree::rebuildFolders(const QList<FolderRecord>& folders)
{
    m_restoring = true;
    clear(); m_notes.clear(); m_folders.clear();
    const QIcon folderIcon = NocturneUi::icon(NocturneUi::Glyph::Folder);
    m_noteIcon = NocturneUi::icon(NocturneUi::Glyph::Notebook);
    QHash<qint64, qint64> parents;
    auto addFolder = [&](qint64 id, const QString& name) {
        auto* item = new NotebookItem;
        item->setText(name); item->setData(Qt::UserRole, id);
        item->setData(Qt::UserRole + 100, true); item->setIcon(0, folderIcon);
        if (id == 0) item->setFlags(item->flags() & ~Qt::ItemIsDragEnabled);
        item->setForeground(1, NocturneUi::theme().muted);
        item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
        item->setSizeHint(QSize(0, 38));
        m_folders.insert(id, item);
    };
    for (const auto& folder : folders) { addFolder(folder.id, folder.name); parents.insert(folder.id, folder.parentId); }
    for (const auto& folder : folders) {
        qint64 parent = folder.parentId;
        QSet<qint64> seen{folder.id};
        for (qint64 walk = parent; walk > 0; walk = parents.value(walk)) {
            if (seen.contains(walk)) { parent = 0; break; }
            seen.insert(walk);
        }
        if (parent > 0 && m_folders.contains(parent)) m_folders[parent]->addChild(m_folders[folder.id]);
        else addTopLevelItem(m_folders[folder.id]);
    }
    addFolder(0, QStringLiteral("未分组"));
    addTopLevelItem(m_folders[0]);
}
NotebookItem* NotebookTree::addNote(const QString& text, qint64 folderId)
{
    auto* item = new NotebookItem;
    item->setText(text); item->setIcon(0, m_noteIcon); item->setSizeHint(QSize(0, 34));
    item->setFlags((item->flags() | Qt::ItemIsDragEnabled) & ~Qt::ItemIsDropEnabled);
    m_folders.value(folderId, m_folders.value(0))->addChild(item);
    item->setFirstColumnSpanned(true);
    m_notes.append(item);
    return item;
}
void NotebookTree::finishRebuild(bool searching)
{
    std::function<int(NotebookItem*)> visit = [&](NotebookItem* item) {
        int count = 0;
        for (int i = 0; i < item->childCount(); ++i) {
            auto* child = static_cast<NotebookItem*>(item->child(i));
            count += child->isFolder() ? visit(child) : 1;
        }
        item->QTreeWidgetItem::setText(1, QString::number(count));
        item->setHidden(searching && count == 0);
        const qint64 id = item->data(Qt::UserRole).toLongLong();
        item->setExpanded(searching || m_expanded.contains(id) || (!m_hasSavedExpansion && item->parent() == nullptr));
        return count;
    };
    for (int i = 0; i < topLevelItemCount(); ++i) visit(static_cast<NotebookItem*>(topLevelItem(i)));
    m_restoring = false;
}
void NotebookTree::setCurrentRow(int row)
{
    auto* selected = item(row);
    if (!selected) return;
    setCurrentItem(selected);
    for (auto* parent = selected->parent(); parent; parent = parent->parent()) parent->setExpanded(true);
    scrollToItem(selected);
}
void NotebookTree::refreshTheme()
{
    const QIcon folder = NocturneUi::icon(NocturneUi::Glyph::Folder);
    m_noteIcon = NocturneUi::icon(NocturneUi::Glyph::Notebook);
    for (auto* item : m_folders) { item->setIcon(0, folder); item->setForeground(1, NocturneUi::theme().muted); }
    for (auto* item : m_notes) item->setIcon(0, m_noteIcon);
    viewport()->update();
}
void NotebookTree::dragEnterEvent(QDragEnterEvent* event)
{
    if (event->mimeData()->hasUrls()) event->acceptProposedAction();
    else QTreeWidget::dragEnterEvent(event);
}
void NotebookTree::dragMoveEvent(QDragMoveEvent* event)
{
    if (event->mimeData()->hasUrls()) event->acceptProposedAction();
    else QTreeWidget::dragMoveEvent(event);
}
void NotebookTree::dropEvent(QDropEvent* event)
{
    NotebookItem* target = itemAt(event->position().toPoint());
    if (target && !event->mimeData()->hasUrls()
        && (dropIndicatorPosition() == QAbstractItemView::AboveItem || dropIndicatorPosition() == QAbstractItemView::BelowItem))
        target = static_cast<NotebookItem*>(target->parent());
    if (target && !target->isFolder()) target = static_cast<NotebookItem*>(target->parent());
    const qint64 parentId = target ? target->data(Qt::UserRole).toLongLong() : 0;
    if (event->mimeData()->hasUrls()) {
        QStringList paths;
        for (const auto& url : event->mimeData()->urls()) if (url.isLocalFile()) paths.append(url.toLocalFile());
        if (!paths.isEmpty()) { emit filesDropped(paths, parentId); event->acceptProposedAction(); }
        return;
    }
    auto* source = currentItem();
    if (event->source() == this && source) {
        emit moveRequested(source->isFolder(), source->data(Qt::UserRole).toLongLong(), parentId);
        event->setDropAction(Qt::MoveAction); event->accept();
    }
}
