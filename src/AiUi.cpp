#include "MainWindow.h"
#include "AiExchange.h"
#include "WorkspaceStore.h"
#include "FolderComboBox.h"
#include "DocumentImporter.h"
#include "NoteEditor.h"
#include "NocturneDialogs.h"
#include "TextDiff.h"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSaveFile>
#include "StickyNoteWindow.h"
#include <QSplitter>
#include <QStackedWidget>
#include <QTabWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QTextDocumentFragment>

void MainWindow::showAiHandoff()
{
    if (!saveCurrentNote())
        return;
    std::optional<HandoffSelection> capturedSelection;
    const bool sourceMode = m_bodyStack->currentIndex() == 1;
    const auto cursor = sourceMode ? m_sourceEditor->textCursor() : m_editor->textCursor();
    if (cursor.hasSelection())
    {
        HandoffSelection value;
        value.noteId = m_currentNoteId;
        value.start = cursor.selectionStart();
        value.end = cursor.selectionEnd();
        value.noteHash = m_currentContentHash;
        if (sourceMode)
            value.markdown = cursor.selectedText().replace(QChar::ParagraphSeparator, '\n');
        else
        {
            NoteSnapshot fragment;
            fragment.note.html = cursor.selection().toHtml();
            value.markdown = AiExchange::markdown(fragment);
        }
        value.readOnly = (m_linkedSource && !sourceMode) || m_currentNoteKind == "sticky"
            || (sourceMode && m_sourceEditor->isReadOnly());
        if (m_linkedSource)
            value.sourcePath = m_linkedSource->sourcePath;
        if (m_sourceSnapshot)
            value.sourceHash = m_sourceSnapshot->hash;
        capturedSelection = value;
    }
    NocturneDialog dialog(this);
    dialog.setObjectName("aiHandoffDialog");
    dialog.setWindowTitle(QStringLiteral("AI 交接与共享 · 夜航"));
    dialog.resize(1060, 720);
    auto* layout = new QVBoxLayout(dialog.body());
    layout->setContentsMargins(22, 18, 22, 18);
    auto* status = new QLabel(
        QStringLiteral("选择本次交给 AI 的笔记。MCP 只读取这次选择的资料，修改必须由你比较并应用。"),
        dialog.body());
    status->setWordWrap(true);
    layout->addWidget(status);
    auto* tabs = new QTabWidget(dialog.body());
    layout->addWidget(tabs, 1);
    auto* selection = new QWidget(tabs);
    auto* selectLayout = new QVBoxLayout(selection);
    selectLayout->setContentsMargins(0, 8, 0, 0);
    auto* folder = new FolderComboBox(selection);
    folder->setFolders(m_database->listFolders(), true);
    folder->setObjectName("handoffFolder");
    selectLayout->addWidget(folder);
    auto* split = new QSplitter(selection);
    auto* notes = new QListWidget(split);
    notes->setObjectName("handoffNotes");
    auto* preview = new QPlainTextEdit(split);
    preview->setReadOnly(true);
    preview->setObjectName("handoffPreview");
    split->setStretchFactor(1, 1);
    selectLayout->addWidget(split, 1);
    auto* selectRow = new QHBoxLayout;
    auto* all = new QPushButton(QStringLiteral("选择当前列表全部"), selection);
    auto* none = new QPushButton(QStringLiteral("取消选择"), selection);
    auto* images = new QCheckBox(QStringLiteral("包含所选笔记直接引用的图片"), selection);
    auto* selectionOnly = new QCheckBox(QStringLiteral("只共享当前选区"), selection);
    selectionOnly->setObjectName("shareSelectionOnly");
    selectionOnly->setEnabled(capturedSelection.has_value());
    selectRow->addWidget(all);
    selectRow->addWidget(none);
    selectRow->addWidget(images);
    selectRow->addWidget(selectionOnly);
    selectRow->addStretch();
    selectLayout->addLayout(selectRow);
    tabs->addTab(selection, QStringLiteral("选择交接范围"));
    auto* review = new QWidget(tabs);
    auto* reviewLayout = new QVBoxLayout(review);
    auto* proposals = new QListWidget(review);
    proposals->setObjectName("proposalList");
    reviewLayout->addWidget(proposals);
    auto* proposalDiff = new QPlainTextEdit(review);
    proposalDiff->setObjectName("proposalDiff");
    proposalDiff->setReadOnly(true);
    reviewLayout->addWidget(proposalDiff, 1);
    auto* reviewRow = new QHBoxLayout;
    auto* refresh = new QPushButton(QStringLiteral("刷新修改建议"), review);
    auto* apply = new QPushButton(QStringLiteral("确认并应用所选建议"), review);
    apply->setObjectName("applyProposalButton");
    reviewRow->addWidget(refresh);
    reviewRow->addWidget(apply);
    reviewRow->addStretch();
    reviewLayout->addLayout(reviewRow);
    reviewLayout->addWidget(new QLabel(
        QStringLiteral("建议按 Markdown 更新正文；原内容与格式保留在航迹中。版本变化时会停止应用。"),
        review));
    tabs->addTab(review, QStringLiteral("修改建议"));
    auto* commands = new QHBoxLayout;
    auto* exportPackage = new QPushButton(QStringLiteral("导出交接包"), dialog.body());
    auto* share = new QPushButton(QStringLiteral("启用 MCP 共享"), dialog.body());
    share->setObjectName("startMcpSharingButton");
    auto* config = new QPushButton(QStringLiteral("复制 MCP 配置"), dialog.body());
    auto* codexConfig = new QPushButton(QStringLiteral("复制 Codex 配置"), dialog.body());
    codexConfig->setObjectName("copyCodexMcpButton");
    auto* stop = new QPushButton(QStringLiteral("停止共享"), dialog.body());
    stop->setObjectName("stopMcpSharingButton");
    auto* close = new QPushButton(QStringLiteral("关闭"), dialog.body());
    commands->addWidget(exportPackage);
    commands->addWidget(share);
    commands->addWidget(config);
    commands->addWidget(codexConfig);
    commands->addWidget(stop);
    commands->addStretch();
    commands->addWidget(close);
    layout->addLayout(commands);
    QString sessionFile = QSettings().value("ai/lastSession").toString();
    QList<QString> proposalFiles;
    QJsonObject selectedProposal, selectedEntry;
    auto populate = [&]
    {
        notes->clear();
        for (const auto& note :
            m_database->listNoteSummaries({}, nullptr, folder->currentData().toLongLong()))
        {
            auto* item = new QListWidgetItem(note.title, notes);
            item->setData(Qt::UserRole, note.id);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(note.id == m_currentNoteId ? Qt::Checked : Qt::Unchecked);
        }
        if (notes->count())
            notes->setCurrentRow(0);
    };
    auto selectedIds = [&]
    {
        QList<qint64> ids;
        for (int i = 0; i < notes->count(); ++i)
            if (notes->item(i)->checkState() == Qt::Checked)
                ids.append(notes->item(i)->data(Qt::UserRole).toLongLong());
        return ids;
    };
    connect(folder, &QComboBox::currentIndexChanged, &dialog, [&] { populate(); });
    connect(all, &QPushButton::clicked, &dialog,
        [&]
        {
            for (int i = 0; i < notes->count(); ++i)
                notes->item(i)->setCheckState(Qt::Checked);
        });
    connect(none, &QPushButton::clicked, &dialog,
        [&]
        {
            for (int i = 0; i < notes->count(); ++i)
                notes->item(i)->setCheckState(Qt::Unchecked);
        });
    connect(notes, &QListWidget::currentRowChanged, &dialog,
        [&](int row)
        {
            if (row < 0)
                return;
            QString error;
            auto snapshot = WorkspaceStore(*m_database)
                                .snapshot(notes->item(row)->data(Qt::UserRole).toLongLong(), false, &error);
            if (!snapshot)
            {
                preview->setPlainText(error);
                return;
            }
            if (!snapshot->sourcePath.isEmpty())
            {
                auto source = SourceFile::read(snapshot->sourcePath, &error);
                if (!source)
                {
                    preview->setPlainText(error);
                    return;
                }
                snapshot->sourceBytes = source->bytes;
            }
            preview->setPlainText(AiExchange::markdown(*snapshot));
        });
    auto shareSelection = [&](bool active, const QString& parent)
    {
        const auto scope = selectionOnly->isChecked() ? capturedSelection : std::optional<HandoffSelection>();
        const auto result
            = AiExchange::create(*m_database, scope ? QList<qint64> { scope->noteId } : selectedIds(), parent,
                images->isChecked(), active, scope);
        if (!result.error.isEmpty())
        {
            status->setText(result.error);
            return;
        }
        if (active)
        {
            if (!sessionFile.isEmpty())
                AiExchange::setActive(sessionFile, false);
            sessionFile = result.manifestPath;
            QSettings().setValue("ai/lastSession", sessionFile);
        }
        status->setText(QStringLiteral("已生成 %1 篇笔记、%2 个图片引用的交接包：%3%4")
                .arg(result.noteCount)
                .arg(result.imageCount)
                .arg(QFileInfo(result.manifestPath).absolutePath(),
                    active ? QStringLiteral("\n共享已启用；将 MCP 配置加入客户端后即可访问。")
                           : QStringLiteral("\n交接检查点已保存。")));
    };
    connect(selectionOnly, &QCheckBox::toggled, &dialog,
        [&](bool checked)
        {
            notes->setEnabled(!checked);
            folder->setEnabled(!checked);
            all->setEnabled(!checked);
            none->setEnabled(!checked);
            if (checked && capturedSelection)
                preview->setPlainText(capturedSelection->markdown);
        });
    connect(exportPackage, &QPushButton::clicked, &dialog,
        [&]
        {
            const QString parent
                = NocturneDialogs::getExistingDirectory(&dialog, QStringLiteral("选择交接包存放目录"));
            if (!parent.isEmpty())
                shareSelection(false, parent);
        });
    connect(share, &QPushButton::clicked, &dialog,
        [&] { shareSelection(true, QDir(m_database->dataDirectory()).filePath("ai-sessions")); });
    connect(codexConfig, &QPushButton::clicked, &dialog,
        [&]
        {
            if (sessionFile.isEmpty())
            {
                status->setText(QStringLiteral("请先启用 MCP 共享。"));
                return;
            }
            auto quoted = [](const QString& value)
            {
                const auto json = QJsonDocument(QJsonArray { value }).toJson(QJsonDocument::Compact);
                return QString::fromUtf8(json.mid(1, json.size() - 2));
            };
            const QString text
                = QStringLiteral("[mcp_servers.nocturne]\ncommand = %1\nargs = [\"--mcp\", \"--session\", "
                                 "%2]\nstartup_timeout_sec = 10\ntool_timeout_sec = 60\n")
                      .arg(quoted(QCoreApplication::applicationFilePath()), quoted(sessionFile));
            QApplication::clipboard()->setText(text);
            status->setText(QStringLiteral("已复制 Codex TOML 配置。合并到 Codex 配置后重载 MCP 连接。"));
        });
    connect(config, &QPushButton::clicked, &dialog,
        [&]
        {
            if (sessionFile.isEmpty())
            {
                status->setText(QStringLiteral("请先选择资料并启用 MCP 共享。"));
                return;
            }
            const QJsonObject command {
                { "command", QDir::toNativeSeparators(QCoreApplication::applicationFilePath()) },
                { "args", QJsonArray { "--mcp", "--session", QDir::toNativeSeparators(sessionFile) } }
            };
            const auto configuration
                = QJsonDocument(QJsonObject { { "mcpServers", QJsonObject { { "nocturne", command } } } })
                      .toJson(QJsonDocument::Indented);
            QApplication::clipboard()->setText(QString::fromUtf8(configuration));
            status->setText(QStringLiteral("已复制 stdio MCP 配置；仅此交接包中的资料可访问。"));
        });
    connect(stop, &QPushButton::clicked, &dialog,
        [&]
        {
            QString error;
            if (sessionFile.isEmpty() || !AiExchange::setActive(sessionFile, false, &error))
                status->setText(error.isEmpty() ? QStringLiteral("当前没有共享会话。") : error);
            else
                status->setText(QStringLiteral("共享已停止，MCP 进程将在约一秒内退出；交接文件保留。"));
        });
    auto refreshProposals = [&]
    {
        proposalFiles.clear();
        proposals->clear();
        selectedProposal = {};
        selectedEntry = {};
        proposalDiff->clear();
        if (sessionFile.isEmpty())
            return;
        const QDir directory(QDir(QFileInfo(sessionFile).absolutePath()).filePath("proposals"));
        for (const auto& file : directory.entryInfoList({ "*.json" }, QDir::Files, QDir::Time))
        {
            QFile input(file.absoluteFilePath());
            if (!input.open(QIODevice::ReadOnly) || input.size() > 16 * 1024 * 1024)
                continue;
            const auto value = QJsonDocument::fromJson(input.readAll()).object();
            if (value.value("status").toString() == "applied")
                continue;
            proposalFiles.append(file.absoluteFilePath());
            proposals->addItem(QStringLiteral("笔记 %1 · %2")
                    .arg(value.value("id").toString(), value.value("createdAt").toString()));
        }
        if (proposals->count())
            proposals->setCurrentRow(0);
    };
    connect(refresh, &QPushButton::clicked, &dialog, refreshProposals);
    connect(proposals, &QListWidget::currentRowChanged, &dialog,
        [&](int row)
        {
            selectedProposal = {};
            selectedEntry = {};
            if (row < 0 || row >= proposalFiles.size())
                return;
            QFile input(proposalFiles[row]);
            if (!input.open(QIODevice::ReadOnly))
                return;
            selectedProposal = QJsonDocument::fromJson(input.readAll()).object();
            const auto session = AiExchange::readSession(sessionFile);
            for (const auto& value : session.value("notes").toArray())
                if (value.toObject().value("id") == selectedProposal.value("id"))
                {
                    selectedEntry = value.toObject();
                    break;
                }
            if (selectedEntry.isEmpty()
                || selectedEntry.value("exportHash") != selectedProposal.value("expected_hash"))
            {
                proposalDiff->setPlainText(QStringLiteral("建议不属于当前共享版本，不能应用。"));
                apply->setEnabled(false);
                return;
            }
            auto current
                = WorkspaceStore(*m_database).snapshot(selectedEntry.value("id").toString().toLongLong());
            if (!current)
            {
                apply->setEnabled(false);
                return;
            }
            QString before = AiExchange::markdown(*current);
            if (selectedEntry.value("selection").toBool())
            {
                QFile file(AiExchange::scopedPath(sessionFile, selectedEntry.value("file").toString()));
                if (file.open(QIODevice::ReadOnly))
                    before = QString::fromUtf8(file.readAll());
            }
            proposalDiff->setPlainText(
                TextDiff::unified(before, selectedProposal.value("markdown").toString()));
            apply->setEnabled(true);
        });
    connect(apply, &QPushButton::clicked, &dialog,
        [&]
        {
            if (selectedEntry.isEmpty() || selectedProposal.isEmpty())
                return;
            QString error;
            const auto session = AiExchange::readSession(sessionFile, &error);
            if (!session.value("active").toBool())
            {
                status->setText(QStringLiteral("共享已停止，不能应用该会话的建议。"));
                return;
            }
            QJsonObject authorized;
            for (const auto& value : session.value("notes").toArray())
                if (value.toObject().value("id") == selectedProposal.value("id"))
                {
                    authorized = value.toObject();
                    break;
                }
            if (authorized.isEmpty()
                || authorized.value("exportHash") != selectedProposal.value("expected_hash"))
            {
                status->setText(QStringLiteral("建议不属于当前共享范围或版本，未应用。"));
                return;
            }
            selectedEntry = authorized;
            if (selectedEntry.value("readOnly").toBool())
            {
                status->setText(QStringLiteral("此选区只读，不能应用修改。"));
                return;
            }
            const qint64 id = selectedEntry.value("id").toString().toLongLong();
            const auto current = m_database->note(id, &error);
            if (!current || current->title != selectedEntry.value("title").toString())
            {
                status->setText(QStringLiteral("笔记已变化或不再存在，请重新交接。"));
                return;
            }
            const auto linked = m_database->linkedSource(id);
            if (linked)
            {
                const auto source = SourceFile::read(linked->sourcePath, &error);
                if (!source
                    || QString::fromLatin1(source->hash.toHex())
                        != selectedEntry.value("sourceHash").toString())
                {
                    status->setText(QStringLiteral("源文件已经变化，请重新交接，未应用旧建议。"));
                    return;
                }
            }
            else if (QString::fromLatin1(current->contentHash.toHex())
                != selectedEntry.value("noteHash").toString())
            {
                status->setText(QStringLiteral("笔记已被编辑，未应用过期建议。"));
                return;
            }
            QString proposed = selectedProposal.value("markdown").toString();
            for (const auto& value : session.value("assets").toArray())
            {
                const auto asset = value.toObject();
                if (asset.value("noteId") != selectedEntry.value("id"))
                    continue;
                const QString exported = "../" + asset.value("file").toString(),
                              original = asset.value("originalReference").toString();
                AiExchange::replaceImageReference(proposed, exported, original);
            }
            if (!WorkspaceStore(*m_database).capture(id, QStringLiteral("应用AI建议前"), &error, true))
            {
                status->setText(error);
                return;
            }
            openNoteById(id);
            if (m_currentNoteId != id)
                return;
            if (m_linkedSource)
            {
                if (!m_sourceSnapshot
                    || m_linkedSource->sourcePath != selectedEntry.value("sourcePath").toString()
                    || QString::fromLatin1(m_sourceSnapshot->hash.toHex())
                        != selectedEntry.value("sourceHash").toString())
                {
                    status->setText(QStringLiteral("打开文档时源文件发生了变化，请重新交接。"));
                    return;
                }
            }
            else if (QString::fromLatin1(m_currentContentHash.toHex())
                != selectedEntry.value("noteHash").toString())
            {
                status->setText(QStringLiteral("打开文档时版本发生了变化，未应用旧建议。"));
                return;
            }
            if (current->kind == "sticky")
            {
                if (m_database->saveStickyNote(id, proposed, &error) < 0)
                {
                    status->setText(error);
                    return;
                }
                for (auto* sticky : m_stickyWindows)
                    if (sticky->noteId() == id)
                        sticky->reloadFromDatabase();
                m_noteCache.remove(id);
                loadNote(id);
            }
            else
            {
                const bool onlySelection = selectedEntry.value("selection").toBool();
                if (onlySelection)
                {
                    auto* document = m_linkedSource ? m_sourceEditor->document() : m_editor->document();
                    const int start = selectedEntry.value("selectionStart").toInt(),
                              end = selectedEntry.value("selectionEnd").toInt();
                    if (start < 0 || end <= start || end >= document->characterCount())
                    {
                        status->setText(QStringLiteral("选区位置无效，未应用修改。"));
                        return;
                    }
                    QTextCursor target(document);
                    target.setPosition(start);
                    target.setPosition(end, QTextCursor::KeepAnchor);
                    if (m_linkedSource)
                    {
                        m_sourceEditor->setTextCursor(target);
                        m_sourceEditor->insertPlainText(proposed);
                    }
                    else
                    {
                        m_editor->setTextCursor(target);
                        m_editor->insertMarkdownText(proposed);
                    }
                }
                else if (m_linkedSource)
                {
                    m_sourceEditor->setPlainText(proposed);
                }
                else
                {
                    m_editor->selectAll();
                    m_editor->insertMarkdownText(proposed);
                }
                scheduleSave();
                if (!saveCurrentNote())
                {
                    status->setText(QStringLiteral("建议仍保留，写入未完成；请处理编辑器中的冲突状态。"));
                    return;
                }
            }
            WorkspaceStore(*m_database).capture(id, QStringLiteral("AI建议（用户确认）"), &error, true);
            const int row = proposals->currentRow();
            if (row >= 0)
            {
                selectedProposal.insert("status", "applied");
                QSaveFile file(proposalFiles[row]);
                const auto bytes = QJsonDocument(selectedProposal).toJson(QJsonDocument::Indented);
                if (file.open(QIODevice::WriteOnly))
                {
                    file.write(bytes);
                    file.commit();
                }
            }
            status->setText(QStringLiteral("已应用建议；修改前版本保留在航迹中。"));
            refreshProposals();
        });
    connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);
    populate();
    refreshProposals();
    dialog.exec();
}
