#include "FolderComboBox.h"
#include <QApplication>
#include <QKeyEvent>
#include <QScreen>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>

FolderComboBox::FolderComboBox(QWidget* parent) : NocturneComboBox(parent)
{
    m_popup = new QFrame(this, Qt::Popup);
    m_popup->setObjectName(QStringLiteral("folderTreePopup"));
    auto* layout = new QVBoxLayout(m_popup); layout->setContentsMargins(6, 6, 6, 6);
    m_tree = new QTreeWidget(m_popup); m_tree->setObjectName(QStringLiteral("folderChoiceTree"));
    m_tree->setHeaderHidden(true); m_tree->setIndentation(20); m_tree->setUniformRowHeights(true);
    m_tree->setAnimated(false); m_tree->setExpandsOnDoubleClick(false);
    m_tree->installEventFilter(this); layout->addWidget(m_tree);
    connect(m_tree, &QTreeWidget::itemClicked, this, [this] { selectTreeItem(); });
}
QHash<qint64, QString> FolderComboBox::folderPaths(const QList<FolderRecord>& folders)
{
    QHash<qint64, FolderRecord> records;
    for (const auto& folder : folders) records.insert(folder.id, folder);
    QHash<qint64, QString> paths;
    for (const auto& folder : folders) {
        QStringList parts; QSet<qint64> visited;
        for (qint64 id = folder.id; id > 0 && records.contains(id) && !visited.contains(id); id = records.value(id).parentId) {
            visited.insert(id); parts.prepend(records.value(id).name);
        }
        paths.insert(folder.id, parts.join(QStringLiteral(" / ")));
    }
    return paths;
}
void FolderComboBox::setFolders(const QList<FolderRecord>& folders, bool includeAll, const QString& rootLabel, qint64 excludedSubtree)
{
    hidePopup(); clear(); m_tree->clear(); m_folders.clear();
    const auto paths = folderPaths(folders);
    QSet<qint64> excluded;
    if (excludedSubtree > 0) excluded.insert(excludedSubtree);
    bool changed;
    do {
        changed = false;
        for (const auto& f : folders) if (excluded.contains(f.parentId) && !excluded.contains(f.id)) {
            excluded.insert(f.id); changed = true;
        }
    } while (changed);
    auto add = [this](const QString& name, qint64 id, const QString& path) {
        addItem(path, id);
        auto* item = new QTreeWidgetItem;
        item->setText(0, name); item->setToolTip(0, path); item->setData(0, Qt::UserRole, id);
        item->setIcon(0, NocturneUi::icon(NocturneUi::Glyph::Folder)); item->setSizeHint(0, QSize(0, 34));
        return item;
    };
    if (includeAll) m_tree->addTopLevelItem(add(QStringLiteral("全部笔记"), Database::AllFolders, QStringLiteral("全部笔记")));
    m_tree->addTopLevelItem(add(rootLabel, 0, rootLabel));
    QHash<qint64, QTreeWidgetItem*> items;
    for (const auto& f : folders) {
        if (excluded.contains(f.id)) continue;
        m_folders.append(f); items.insert(f.id, add(f.name, f.id, paths.value(f.id)));
    }
    for (const auto& f : m_folders) {
        QSet<qint64> seen{f.id}; bool cycle = false;
        qint64 parent = f.parentId;
        while (items.contains(parent)) {
            if (seen.contains(parent)) { cycle = true; break; }
            seen.insert(parent);
            const auto found = std::find_if(m_folders.cbegin(), m_folders.cend(), [parent](const FolderRecord& entry) { return entry.id == parent; });
            if (found == m_folders.cend()) break;
            parent = found->parentId;
        }
        if (!cycle && items.contains(f.parentId)) items[f.parentId]->addChild(items[f.id]);
        else m_tree->addTopLevelItem(items[f.id]);
    }
    m_tree->collapseAll();
}
void FolderComboBox::showPopup()
{
    if (!isEnabled()) return;
    m_tree->collapseAll(); // Every selection starts at the first directory level.
    m_tree->clearSelection();
    const QRect available = screen()->availableGeometry();
    const QSize size(std::min(460, available.width() - 20), std::min(420, available.height() - 20));
    QPoint position = mapToGlobal(QPoint(0, height()));
    position.setX(std::clamp(position.x(), available.left(), available.right() - size.width()));
    if (position.y() + size.height() > available.bottom()) position.setY(mapToGlobal(QPoint(0, 0)).y() - size.height());
    position.setY(std::max(position.y(), available.top()));
    m_popup->resize(size); m_popup->move(position); m_popup->show(); m_tree->setFocus();
}
void FolderComboBox::hidePopup()
{
    if (m_popup) m_popup->hide();
    NocturneComboBox::hidePopup();
}
void FolderComboBox::selectTreeItem()
{
    const auto* item = m_tree->currentItem();
    if (!item) return;
    const int index = findData(item->data(0, Qt::UserRole));
    if (index >= 0) { setCurrentIndex(index); hidePopup(); }
}
bool FolderComboBox::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_tree && event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) { selectTreeItem(); return true; }
        if (key->key() == Qt::Key_Escape) { hidePopup(); return true; }
    }
    return NocturneComboBox::eventFilter(watched, event);
}
