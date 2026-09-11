#include "MainWindow.h"
#include "WorkspaceStore.h"
#include "NoteEditor.h"
#include "FolderComboBox.h"
#include "NocturneDialogs.h"
#include "TextDiff.h"
#include "SourceFile.h"
#include <QDateTimeEdit>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QSpinBox>
#include <QSplitter>
#include <QSqlQuery>
#include <QSqlError>
#include <QTabWidget>
#include <QTextBlock>
#include <QTextFragment>
#include <QTextImageFormat>
#include <QTimer>
#include <QUuid>
#include <QUrl>
#include <QVBoxLayout>

namespace
{
QString snapshotText(const NoteSnapshot& value)
{
    if (!value.sourcePath.isEmpty())
        if (auto source = SourceFile::decode(value.sourceBytes))
            return source->text;
    return value.note.plainText;
}
QPushButton* button(QHBoxLayout* row, QWidget* parent, const QString& text, const char* name)
{
    auto* value = new QPushButton(text, parent);
    value->setObjectName(name);
    row->addWidget(value);
    return value;
}
}

void MainWindow::showHistory()
{
    if (m_currentNoteId <= 0 || !saveCurrentNote())
        return;
    WorkspaceStore store(*m_database);
    QString error;
    if (!store.capture(m_currentNoteId, QStringLiteral("记录起点"), &error))
    {
        setStatusMessage(error, true);
        return;
    }
    const auto current = store.snapshot(m_currentNoteId);
    if (!current)
        return;
    NocturneDialog dialog(this);
    dialog.setObjectName("historyDialog");
    dialog.setWindowTitle(QStringLiteral("航迹 · %1").arg(current->note.title));
    dialog.resize(1050, 700);
    auto* layout = new QVBoxLayout(dialog.body());
    layout->setContentsMargins(22, 18, 22, 18);
    auto* summary = new QLabel(
        QStringLiteral("连续编辑合并记录；手动检查点保留。恢复前会保存当前版本。"), dialog.body());
    summary->setWordWrap(true);
    layout->addWidget(summary);
    auto* timeRow = new QHBoxLayout;
    auto* from = new QDateTimeEdit(QDateTime::currentDateTime().addDays(-7), dialog.body());
    auto* to = new QDateTimeEdit(QDateTime::currentDateTime(), dialog.body());
    from->setDisplayFormat("yyyy-MM-dd HH:mm");
    to->setDisplayFormat("yyyy-MM-dd HH:mm");
    timeRow->addWidget(new QLabel(QStringLiteral("查看修改时段"), dialog.body()));
    timeRow->addWidget(from);
    timeRow->addWidget(to);
    auto* range = button(timeRow, dialog.body(), QStringLiteral("比较此时段"), "historyRangeButton");
    timeRow->addStretch();
    layout->addLayout(timeRow);
    auto* split = new QSplitter(dialog.body());
    auto* list = new QListWidget(split);
    list->setObjectName("versionList");
    list->setMinimumWidth(250);
    auto* tabs = new QTabWidget(split);
    auto* preview = new NoteEditor(tabs);
    preview->setReadOnly(true);
    auto* difference = new QPlainTextEdit(tabs);
    difference->setReadOnly(true);
    difference->setObjectName("historyDiff");
    auto* source = new QPlainTextEdit(tabs);
    source->setReadOnly(true);
    tabs->addTab(preview, QStringLiteral("版本预览"));
    tabs->addTab(difference, QStringLiteral("与当前比较"));
    tabs->addTab(source, QStringLiteral("文字 / 源码"));
    split->setStretchFactor(1, 1);
    layout->addWidget(split, 1);
    auto* commands = new QHBoxLayout;
    auto* checkpoint = button(commands, dialog.body(), QStringLiteral("保存检查点"), "checkpointButton");
    auto* restore = button(commands, dialog.body(), QStringLiteral("恢复此版本"), "restoreVersionButton");
    auto* copy = button(commands, dialog.body(), QStringLiteral("恢复为副本"), "restoreVersionCopyButton");
    auto* pin = button(commands, dialog.body(), QStringLiteral("保留 / 取消保留"), "pinVersionButton");
    auto* exportDiff = button(commands, dialog.body(), QStringLiteral("导出差异"), "exportDiffButton");
    layout->addLayout(commands);
    auto* retention = new QHBoxLayout;
    retention->addWidget(new QLabel(QStringLiteral("每篇保留"), dialog.body()));
    auto* keep = new QSpinBox(dialog.body());
    keep->setRange(10, 200);
    keep->setValue(store.historyKeep());
    retention->addWidget(keep);
    retention->addWidget(new QLabel(QStringLiteral("历史空间 MiB"), dialog.body()));
    auto* limit = new QSpinBox(dialog.body());
    limit->setRange(32, 512);
    limit->setValue(store.historyLimitMiB());
    retention->addWidget(limit);
    auto* apply = button(retention, dialog.body(), QStringLiteral("应用保留规则"), "historyLimitsButton");
    retention->addStretch();
    auto* close = button(retention, dialog.body(), QStringLiteral("关闭"), "closeHistoryButton");
    layout->addLayout(retention);
    QList<NoteVersion> versions;
    std::optional<NoteSnapshot> selected;
    auto loadList = [&]
    {
        versions = store.versions(m_currentNoteId, &error);
        list->clear();
        for (const auto& version : versions)
        {
            auto* item
                = new QListWidgetItem(QStringLiteral("%1%2\n%3")
                                          .arg(version.pinned ? QStringLiteral("★ ") : QString(),
                                              version.updatedAt.toLocalTime().toString("MM-dd HH:mm:ss"),
                                              version.label.isEmpty() ? version.reason : version.label),
                    list);
            item->setData(Qt::UserRole, version.id);
        }
        summary->setText(QStringLiteral(
            "历史占用 %1 MiB / %2 MiB；保留检查点及每篇最新两个状态。配额不足时不会删除已保留内容。")
                .arg(store.historyBytes() / 1048576.0, 0, 'f', 1)
                .arg(store.historyLimitMiB()));
        if (list->count())
            list->setCurrentRow(0);
    };
    connect(list, &QListWidget::currentRowChanged, &dialog,
        [&](int row)
        {
            selected.reset();
            if (row < 0 || row >= versions.size())
                return;
            selected = store.version(versions[row].id, &error);
            if (!selected)
            {
                summary->setText(error);
                return;
            }
            preview->setHtml(selected->note.html);
            source->setPlainText(snapshotText(*selected));
            difference->setPlainText(TextDiff::unified(snapshotText(*selected), snapshotText(*current)));
            restore->setEnabled(current->note.kind != "sticky"
                && (current->sourcePath.isEmpty() || !selected->sourcePath.isEmpty()));
        });
    connect(checkpoint, &QPushButton::clicked, &dialog,
        [&]
        {
            bool accepted = false;
            const auto label = NocturneDialogs::getText(&dialog, QStringLiteral("保存检查点"),
                QStringLiteral("检查点名称"), QLineEdit::Normal, QStringLiteral("手动检查点"), &accepted);
            if (accepted)
            {
                if (!store.capture(m_currentNoteId, QStringLiteral("手动检查点"), &error, true, label))
                    summary->setText(error);
                else
                    loadList();
            }
        });
    connect(pin, &QPushButton::clicked, &dialog,
        [&]
        {
            int row = list->currentRow();
            if (row >= 0 && row < versions.size())
            {
                if (!store.setVersionPinned(versions[row].id, !versions[row].pinned, &error))
                    summary->setText(error);
                else
                    loadList();
            }
        });
    connect(apply, &QPushButton::clicked, &dialog,
        [&]
        {
            if (!store.setHistoryLimits(keep->value(), limit->value(), &error))
                summary->setText(error);
            else
                loadList();
        });
    connect(range, &QPushButton::clicked, &dialog,
        [&]
        {
            if (from->dateTime() > to->dateTime())
            {
                summary->setText(QStringLiteral("开始时间不能晚于结束时间。"));
                return;
            }
            std::optional<NoteSnapshot> before, after;
            for (const auto& version : versions)
            {
                if (!after && version.updatedAt <= to->dateTime().toUTC())
                    after = store.version(version.id);
                if (!before && version.updatedAt <= from->dateTime().toUTC())
                    before = store.version(version.id);
            }
            if (!before && !versions.isEmpty())
                before = store.version(versions.last().id);
            if (!after)
            {
                summary->setText(QStringLiteral("所选结束时间之前没有记录。"));
                return;
            }
            difference->setPlainText(
                TextDiff::unified(before ? snapshotText(*before) : QString(), snapshotText(*after),
                    from->dateTime().toString(Qt::ISODate), to->dateTime().toString(Qt::ISODate)));
            tabs->setCurrentWidget(difference);
        });
    connect(copy, &QPushButton::clicked, &dialog,
        [&]
        {
            if (!selected)
                return;
            const auto id = m_database->createNote(selected->note.title + QStringLiteral("（历史副本）"),
                selected->note.html, selected->note.plainText, &error, m_currentFolderId);
            if (!id)
            {
                summary->setText(error);
                return;
            }
            dialog.accept();
            openNoteById(id);
        });
    connect(restore, &QPushButton::clicked, &dialog,
        [&]
        {
            if (!selected)
                return;
            const auto latest = m_database->note(m_currentNoteId, &error);
            if (!latest || latest->contentHash != current->note.contentHash)
            {
                summary->setText(QStringLiteral("当前笔记已变化，请重新打开航迹后比较。"));
                return;
            }
            if (!store.capture(m_currentNoteId, QStringLiteral("恢复前检查点"), &error, true))
            {
                summary->setText(error);
                return;
            }
            if (m_linkedSource)
            {
                const auto raw = SourceFile::decode(selected->sourceBytes, &error);
                if (!raw)
                {
                    summary->setText(error);
                    return;
                }
                m_sourceEditor->setPlainText(raw->text);
            }
            else
                m_editor->setHtml(selected->note.html);
            m_titleEdit->setText(selected->note.title);
            scheduleSave();
            if (!saveCurrentNote())
            {
                summary->setText(QStringLiteral("恢复内容仍保留在编辑器；写回未完成，请处理源文件冲突。"));
                return;
            }
            dialog.accept();
            m_noteCache.remove(m_currentNoteId);
            loadNote(m_currentNoteId);
            setStatusMessage(QStringLiteral("已恢复；原版本保留在航迹中。"));
        });
    connect(exportDiff, &QPushButton::clicked, &dialog,
        [&]
        {
            const auto path = NocturneDialogs::getSaveFileName(&dialog, QStringLiteral("导出修改差异"),
                QStringLiteral("夜航修改.diff"), QStringLiteral("差异文件 (*.diff);;文本 (*.txt)"));
            if (path.isEmpty())
                return;
            QSaveFile file(path);
            const auto bytes = difference->toPlainText().toUtf8();
            if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
                summary->setText(file.errorString());
            else
                summary->setText(QStringLiteral("差异已导出。"));
        });
    connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);
    loadList();
    dialog.exec();
}

void MainWindow::showRecovery()
{
    saveCurrentNote();
    WorkspaceStore store(*m_database);
    QString error;
    NocturneDialog dialog(this);
    dialog.setObjectName("recoveryDialog");
    dialog.setWindowTitle(QStringLiteral("恢复中心 · 夜航"));
    dialog.resize(1020, 670);
    auto* layout = new QVBoxLayout(dialog.body());
    layout->setContentsMargins(22, 18, 22, 18);
    auto* hint = new QLabel(
        QStringLiteral(
            "回收站保留夜航中的记录；外部路径移除不会删除磁盘源文件。恢复草稿和备份内容可另存为笔记。"),
        dialog.body());
    hint->setWordWrap(true);
    layout->addWidget(hint);
    auto* modes = new QHBoxLayout;
    auto* trashButton = button(modes, dialog.body(), QStringLiteral("回收站"), "recoveryTrashTab");
    auto* draftsButton = button(modes, dialog.body(), QStringLiteral("恢复草稿"), "recoveryDraftTab");
    auto* backupButton
        = button(modes, dialog.body(), QStringLiteral("读取备份中的单篇笔记"), "recoveryBackupTab");
    auto* writesButton = button(modes, dialog.body(), QStringLiteral("未完成写入"), "recoveryWritesTab");
    layout->addLayout(modes);
    auto* split = new QSplitter(dialog.body());
    auto* list = new QListWidget(split);
    list->setObjectName("recoveryList");
    auto* preview = new NoteEditor(split);
    preview->setReadOnly(true);
    split->setStretchFactor(1, 1);
    layout->addWidget(split, 1);
    auto* row = new QHBoxLayout;
    auto* restore = button(row, dialog.body(), QStringLiteral("恢复所选笔记"), "recoverNoteButton");
    auto* copy = button(row, dialog.body(), QStringLiteral("另存为笔记"), "recoverCopyButton");
    row->addStretch();
    auto* close = button(row, dialog.body(), QStringLiteral("关闭"), "closeRecoveryButton");
    layout->addLayout(row);
    QString mode = "trash", backupPath;
    QList<NoteSnapshot> entries;
    auto fill = [&]
    {
        list->clear();
        preview->clear();
        for (const auto& entry : entries)
        {
            auto* item = new QListWidgetItem(entry.note.title, list);
            item->setToolTip(entry.sourcePath);
            item->setData(Qt::UserRole, entry.note.id);
        }
        restore->setEnabled(mode == "trash");
        if (list->count())
            list->setCurrentRow(0);
    };
    auto loadTrash = [&]
    {
        mode = "trash";
        entries.clear();
        for (const auto& summary : store.deletedNotes(&error))
        {
            NoteSnapshot value;
            static_cast<NoteSummary&>(value.note) = summary;
            entries.append(value);
        }
        fill();
    };
    connect(list, &QListWidget::currentRowChanged, &dialog,
        [&](int index)
        {
            if (index >= 0 && index < entries.size())
            {
                error.clear();
                if (mode == "trash")
                {
                    const auto selected = store.snapshot(entries[index].note.id, true, &error);
                    if (!selected)
                    {
                        hint->setText(error);
                        return;
                    }
                    entries[index] = *selected;
                }
                if (mode == "backup")
                {
                    const QString connectionName
                        = "backup-note-" + QUuid::createUuid().toString(QUuid::Id128);
                    {
                        auto backup = QSqlDatabase::addDatabase("QSQLITE", connectionName);
                        backup.setDatabaseName(QDir(backupPath).filePath("notebook.sqlite3"));
                        backup.setConnectOptions("QSQLITE_OPEN_READONLY");
                        if (backup.open())
                        {
                            QSqlQuery query(backup);
                            query.prepare("SELECT html,plain_text FROM notes WHERE id=?");
                            query.addBindValue(entries[index].note.id);
                            if (query.exec() && query.next())
                            {
                                entries[index].note.html = query.value(0).toString();
                                entries[index].note.plainText = query.value(1).toString();
                            }
                            else
                                error = query.lastError().text();
                        }
                        else
                            error = backup.lastError().text();
                        backup.close();
                    }
                    QSqlDatabase::removeDatabase(connectionName);
                    if (!error.isEmpty())
                    {
                        hint->setText(error);
                        return;
                    }
                }
                preview->setHtml(entries[index].note.html);
                hint->setText(entries[index].sourcePath.isEmpty()
                        ? QStringLiteral("恢复选中内容不会改变其他笔记。")
                        : QStringLiteral("来源：%1\n恢复引用不写入原文件；另存副本保留可读正文。")
                              .arg(entries[index].sourcePath));
            }
        });
    connect(trashButton, &QPushButton::clicked, &dialog, loadTrash);
    connect(draftsButton, &QPushButton::clicked, &dialog,
        [&]
        {
            mode = "draft";
            entries = store.drafts(&error);
            fill();
        });
    connect(backupButton, &QPushButton::clicked, &dialog,
        [&]
        {
            const auto files = NocturneDialogs::getOpenFileNames(&dialog,
                QStringLiteral("选择本地备份数据库"), QDir(m_database->dataDirectory()).filePath("backups"),
                QStringLiteral("夜航备份 (notebook.sqlite3)"));
            if (files.isEmpty())
                return;
            backupPath = QFileInfo(files.first()).absolutePath();
            QFile manifest(QDir(backupPath).filePath("backup.json"));
            if (!manifest.open(QIODevice::ReadOnly)
                || QJsonDocument::fromJson(manifest.readAll()).object().value("application").toString()
                    != "Nocturne")
            {
                hint->setText(QStringLiteral("该目录缺少有效的夜航备份清单。"));
                return;
            }
            const QString connectionName = "backup-preview-" + QUuid::createUuid().toString(QUuid::Id128);
            {
                auto backup = QSqlDatabase::addDatabase("QSQLITE", connectionName);
                backup.setDatabaseName(files.first());
                backup.setConnectOptions("QSQLITE_OPEN_READONLY");
                entries.clear();
                if (!backup.open())
                {
                    hint->setText(backup.lastError().text());
                }
                else
                {
                    QSqlQuery query(backup);
                    if (query.exec("SELECT id,title,folder_id,kind FROM notes WHERE deleted_at IS NULL ORDER "
                                   "BY updated_at DESC"))
                    {
                        while (query.next())
                        {
                            NoteSnapshot value;
                            value.note.id = query.value(0).toLongLong();
                            value.note.title = query.value(1).toString();
                            value.note.folderId = query.value(2).toLongLong();
                            value.note.kind = query.value(3).toString();
                            entries.append(value);
                        }
                    }
                    else
                        hint->setText(query.lastError().text());
                }
                backup.close();
            }
            QSqlDatabase::removeDatabase(connectionName);
            mode = "backup";
            fill();
        });
    connect(writesButton, &QPushButton::clicked, &dialog,
        [&]
        {
            mode = "write";
            entries.clear();
            const QDir directory(QDir(m_database->dataDirectory()).filePath("write-recovery"));
            for (const auto& fileInfo : directory.entryInfoList({ "*.json" }, QDir::Files))
            {
                QFile file(fileInfo.absoluteFilePath());
                if (!file.open(QIODevice::ReadOnly) || file.size() > 32 * 1024 * 1024)
                    continue;
                const auto object = QJsonDocument::fromJson(file.readAll()).object();
                for (const auto& part : QStringList { "before", "after" })
                {
                    const auto bytes
                        = qUncompress(QByteArray::fromBase64(object.value(part).toString().toLatin1()));
                    const auto source = SourceFile::decode(bytes);
                    if (!source)
                        continue;
                    NoteSnapshot value;
                    value.sourcePath = object.value("path").toString();
                    value.sourceBytes = bytes;
                    value.note.title = QFileInfo(value.sourcePath).fileName()
                        + (part == "before" ? QStringLiteral("（写入前）")
                                            : QStringLiteral("（待写入新稿）"));
                    value.note.plainText = source->text;
                    value.note.html = "<pre>" + source->text.toHtmlEscaped() + "</pre>";
                    entries.append(value);
                }
            }
            fill();
        });
    connect(restore, &QPushButton::clicked, &dialog,
        [&]
        {
            int index = list->currentRow();
            if (mode != "trash" || index < 0)
                return;
            const auto id = entries[index].note.id;
            if (!store.restoreDeleted(id, &error))
            {
                hint->setText(error);
                return;
            }
            dialog.accept();
            m_noteCache.remove(id);
            openNoteById(id);
        });
    connect(copy, &QPushButton::clicked, &dialog,
        [&]
        {
            int index = list->currentRow();
            if (index < 0 || index >= entries.size())
                return;
            auto value = entries[index];
            if (mode == "backup")
            {
                QTextDocument document;
                document.setHtml(value.note.html);
                QStringList missing;
                struct Image
                {
                    int position;
                    QTextImageFormat format;
                };
                QList<Image> images;
                for (auto block = document.begin(); block.isValid(); block = block.next())
                    for (auto it = block.begin(); !it.atEnd(); ++it)
                        if (it.fragment().charFormat().isImageFormat())
                            images.append(
                                { it.fragment().position(), it.fragment().charFormat().toImageFormat() });
                for (auto image : images)
                {
                    const QUrl url(image.format.name());
                    if (!url.isLocalFile())
                        continue;
                    const QString relative = url.toLocalFile().section("/attachments/", 1);
                    if (relative.isEmpty())
                        continue;
                    const QString root = QDir(backupPath).filePath("attachments");
                    const QString source = QDir::cleanPath(QDir(root).filePath(relative));
                    if (!source.startsWith(QDir::cleanPath(root) + "/"))
                    {
                        missing.append(relative);
                        continue;
                    }
                    if (!QFileInfo::exists(source))
                    {
                        missing.append(relative);
                        continue;
                    }
                    const QString destination
                        = QDir(m_database->dataDirectory())
                              .filePath("attachments/recovered-" + QUuid::createUuid().toString(QUuid::Id128)
                                  + "." + QFileInfo(source).suffix());
                    if (!QFile::copy(source, destination))
                    {
                        missing.append(relative);
                        continue;
                    }
                    image.format.setName(QUrl::fromLocalFile(destination).toString());
                    QTextCursor cursor(&document);
                    cursor.setPosition(image.position);
                    cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
                    cursor.setCharFormat(image.format);
                }
                if (!missing.isEmpty())
                {
                    hint->setText(
                        QStringLiteral("部分图片没有可恢复的备份，未恢复笔记：%1").arg(missing.join("、")));
                    return;
                }
                value.note.html = document.toHtml();
            }
            const auto id = m_database->createNote(value.note.title + QStringLiteral("（恢复副本）"),
                value.note.html, value.note.plainText, &error);
            if (!id)
            {
                hint->setText(error);
                return;
            }
            if (mode == "draft")
                store.clearDraft(value.note.id);
            dialog.accept();
            openNoteById(id);
        });
    connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);
    loadTrash();
    dialog.exec();
}

void MainWindow::showTemplates()
{
    if (!saveCurrentNote())
        return;
    WorkspaceStore store(*m_database);
    QString error;
    NocturneDialog dialog(this);
    dialog.setObjectName("templateDialog");
    dialog.setWindowTitle(QStringLiteral("笔记模板 · 夜航"));
    dialog.resize(880, 570);
    auto* layout = new QVBoxLayout(dialog.body());
    layout->setContentsMargins(22, 18, 22, 18);
    auto* status = new QLabel(QStringLiteral("保存常用结构，用模板创建新笔记。"), dialog.body());
    layout->addWidget(status);
    auto* split = new QSplitter(dialog.body());
    auto* list = new QListWidget(split);
    list->setObjectName("templateList");
    auto* preview = new NoteEditor(split);
    preview->setReadOnly(true);
    split->setStretchFactor(1, 1);
    layout->addWidget(split, 1);
    auto* folder = new FolderComboBox(dialog.body());
    folder->setFolders(m_database->listFolders());
    folder->setCurrentIndex(std::max(0, folder->findData(m_currentFolderId)));
    layout->addWidget(folder);
    auto* commands = new QHBoxLayout;
    auto* save = button(commands, dialog.body(), QStringLiteral("当前笔记存为模板"), "saveTemplateButton");
    auto* create = button(commands, dialog.body(), QStringLiteral("使用模板新建"), "useTemplateButton");
    auto* remove = button(commands, dialog.body(), QStringLiteral("删除模板"), "deleteTemplateButton");
    commands->addStretch();
    auto* close = button(commands, dialog.body(), QStringLiteral("关闭"), "closeTemplateButton");
    layout->addLayout(commands);
    save->setEnabled(m_currentNoteId > 0);
    QList<NoteTemplate> templates;
    auto refresh = [&]
    {
        templates = store.templates(&error);
        list->clear();
        for (const auto& item : templates)
            list->addItem(item.name);
        if (list->count())
            list->setCurrentRow(0);
    };
    connect(list, &QListWidget::currentRowChanged, &dialog,
        [&](int row)
        {
            if (row >= 0 && row < templates.size())
                preview->setHtml(templates[row].content.note.html);
            else
                preview->clear();
        });
    connect(save, &QPushButton::clicked, &dialog,
        [&]
        {
            bool accepted = false;
            const auto name = NocturneDialogs::getText(&dialog, QStringLiteral("保存笔记模板"),
                QStringLiteral("模板名称"), QLineEdit::Normal, m_titleEdit->text(), &accepted);
            if (!accepted)
                return;
            const auto content = store.snapshot(m_currentNoteId);
            if (!content || !store.saveTemplate(name, *content, &error))
                status->setText(error);
            else
                refresh();
        });
    connect(create, &QPushButton::clicked, &dialog,
        [&]
        {
            const int row = list->currentRow();
            if (row < 0)
                return;
            const auto& value = templates[row];
            const auto id = m_database->createNote(value.name, value.content.note.html,
                value.content.note.plainText, &error, folder->currentData().toLongLong());
            if (!id)
            {
                status->setText(error);
                return;
            }
            dialog.accept();
            openNoteById(id);
        });
    connect(remove, &QPushButton::clicked, &dialog,
        [&]
        {
            const int row = list->currentRow();
            if (row >= 0)
            {
                if (!store.deleteTemplate(templates[row].id, &error))
                    status->setText(error);
                else
                    refresh();
            }
        });
    connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);
    refresh();
    dialog.exec();
}
