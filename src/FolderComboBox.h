#pragma once
#include "Database.h"
#include "NocturneStyle.h"
#include <QSet>

class QTreeWidget;
class FolderComboBox final : public NocturneComboBox
{
public:
    explicit FolderComboBox(QWidget* parent = nullptr);
    void setFolders(const QList<FolderRecord>& folders, bool includeAll = false,
                    const QString& rootLabel = QStringLiteral("未分组"), qint64 excludedSubtree = -1);
    static QHash<qint64, QString> folderPaths(const QList<FolderRecord>& folders);
    void showPopup() override;
    void hidePopup() override;
protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
private:
    void selectTreeItem();
    QFrame* m_popup = nullptr;
    QTreeWidget* m_tree = nullptr;
    QList<FolderRecord> m_folders;
};
