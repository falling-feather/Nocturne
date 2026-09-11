#include "MainWindow.h"
#include "WorkspaceStore.h"
#include "DocumentImporter.h"
#include "DocumentOutline.h"
#include "NoteEditor.h"
#include "NotebookTree.h"
#include "FolderComboBox.h"
#include "FindBar.h"
#include "TextDiff.h"
#include "MathSupport.h"
#include "NocturneDialogs.h"
#include "NocturneStyle.h"
#include "StickyNoteWindow.h"
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <algorithm>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

void MainWindow::buildWorkspaceUi()
{
    m_writingLayout->removeWidget(m_editor);
    m_bodyStack = new QStackedWidget(m_writingColumn);
    m_bodyStack->setObjectName("bodyStack");
    m_bodyStack->addWidget(m_editor);
    m_sourceEditor = new QPlainTextEdit(m_bodyStack);
    m_sourceEditor->setObjectName("sourceEditor");
    m_sourceEditor->setFont(QFont(QStringLiteral("Consolas"), 12));
    m_sourceEditor->setFrameShape(QFrame::NoFrame);
    m_sourceEditor->setPlaceholderText(QStringLiteral("直接编辑原文件；保留 Markdown、编码与换行。"));
    m_bodyStack->addWidget(m_sourceEditor);
    m_sourceBar = new QFrame(m_writingColumn);
    m_sourceBar->setObjectName("sourceBar");
    auto* sourceRow = new QHBoxLayout(m_sourceBar);
    sourceRow->setContentsMargins(0, 0, 0, 5);
    m_sourceState = new QLabel(m_sourceBar);
    m_sourceState->setObjectName("sourceState");
    m_sourceState->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    sourceRow->addWidget(m_sourceState, 1);
    auto button = [&](const QString& label, const char* name)
    {
        auto* b = new QToolButton(m_sourceBar);
        b->setText(label);
        b->setObjectName(name);
        sourceRow->addWidget(b);
        return b;
    };
    m_sourceToggle = button(QStringLiteral("预览"), "sourceToggle");
    auto* compare = button(QStringLiteral("比较"), "compareSourceButton");
    auto* reload = button(QStringLiteral("重载"), "reloadSourceButton");
    auto* relink = button(QStringLiteral("关联"), "relinkSourceButton");
    m_writingLayout->addWidget(m_sourceBar);
    m_sourceBar->hide();
    m_findBar = new FindBar(m_editor, m_sourceEditor, m_writingColumn);
    m_writingLayout->addWidget(m_findBar);
    m_writingLayout->addWidget(m_bodyStack, 1);
    connect(m_sourceToggle, &QToolButton::clicked, this, &MainWindow::toggleSourceView);
    connect(compare, &QToolButton::clicked, this, &MainWindow::compareSource);
    connect(reload, &QToolButton::clicked, this, &MainWindow::reloadSource);
    connect(relink, &QToolButton::clicked, this, &MainWindow::relinkSource);
    connect(m_sourceEditor, &QPlainTextEdit::textChanged, this,
        [this]
        {
            scheduleSave();
            updateDocumentInfo();
        });
    connect(m_outlineButton, &QToolButton::clicked, this,
        [this](bool checked)
        {
            if (checked && m_linkedSource && m_bodyStack->currentIndex() == 1)
                toggleSourceView();
        });
    auto action = [this](const QString& label, const char* name, const QKeySequence& shortcut)
    {
        auto* value = new QAction(label, this);
        value->setObjectName(name);
        value->setShortcut(shortcut);
        addAction(value);
        return value;
    };
    connect(action(QStringLiteral("查找"), "findNoteAction", QKeySequence::Find), &QAction::triggered, this,
        [this] { m_findBar->open(); });
    connect(action(QStringLiteral("替换"), "replaceNoteAction", QKeySequence::Replace), &QAction::triggered,
        this, [this] { m_findBar->open(true); });
    auto* navigationRow = new QHBoxLayout;
    for (const auto& choice : QList<QPair<QString, QString>> { { QStringLiteral("全部"), "all" },
             { QStringLiteral("最近"), "recent" }, { QStringLiteral("置顶"), "pinned" } })
    {
        auto* view = new QToolButton(m_navigation);
        view->setText(choice.first);
        view->setObjectName("noteView_" + choice.second);
        view->setCheckable(true);
        view->setChecked(choice.second == m_viewMode);
        navigationRow->addWidget(view);
        connect(view, &QToolButton::clicked, this,
            [this, mode = choice.second]
            {
                if (!saveCurrentNote())
                    return;
                m_viewMode = mode;
                for (const auto& name : { "all", "recent", "pinned" })
                    if (auto* item = findChild<QToolButton*>(QString("noteView_") + name))
                        item->setChecked(m_viewMode == name);
                refreshNotes();
            });
    }
    navigationRow->addStretch();
    auto* refresh = new QToolButton(m_navigation);
    refresh->setText(QStringLiteral("刷新"));
    refresh->setObjectName("refreshLinkedFoldersButton");
    refresh->setToolTip(QStringLiteral("刷新所选目录中的外部文档"));
    navigationRow->addWidget(refresh);
    connect(refresh, &QToolButton::clicked, this, &MainWindow::refreshLinkedFolders);
    auto* nav = qobject_cast<QVBoxLayout*>(m_navigation->layout());
    nav->insertLayout(nav->indexOf(m_noteList), navigationRow);
    auto* header = findChild<QFrame*>("documentHeader");
    auto* row = qobject_cast<QHBoxLayout*>(header->layout());
    m_backButton = new QToolButton(header);
    m_backButton->setText("‹");
    m_backButton->setToolTip(QStringLiteral("上一篇 · Alt+Left"));
    m_backButton->setShortcut(QKeySequence("Alt+Left"));
    m_backButton->setEnabled(false);
    m_forwardButton = new QToolButton(header);
    m_forwardButton->setText("›");
    m_forwardButton->setToolTip(QStringLiteral("下一篇 · Alt+Right"));
    m_forwardButton->setShortcut(QKeySequence("Alt+Right"));
    m_forwardButton->setEnabled(false);
    row->insertWidget(0, m_forwardButton);
    row->insertWidget(0, m_backButton);
    connect(m_backButton, &QToolButton::clicked, this, [this] { navigateHistory(-1); });
    connect(m_forwardButton, &QToolButton::clicked, this, [this] { navigateHistory(1); });
    m_todoFolderFilter = new FolderComboBox(m_todoPane);
    m_todoFolderFilter->setObjectName("todoFolderFilter");
    m_todoFolderFilter->setToolTip(QStringLiteral("按待办来源项目筛选"));
    auto* todos = qobject_cast<QVBoxLayout*>(m_todoPane->layout());
    todos->insertWidget(todos->indexOf(m_todoList), m_todoFolderFilter);
    connect(m_todoFolderFilter, &QComboBox::currentIndexChanged, this,
        [this]
        {
            if (!m_loadingFolders)
                refreshTodos();
        });
    connect(m_editor, &NoteEditor::captureRequested, this, &MainWindow::captureSelection);
    connect(m_editor, &NoteEditor::convertMathRequested, this, &MainWindow::convertCurrentNoteMath);
    m_sourceEditor->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_sourceEditor, &QWidget::customContextMenuRequested, this,
        [this](const QPoint& point)
        {
            auto* menu = m_sourceEditor->createStandardContextMenu();
            menu->addSeparator();
            menu->addAction(m_convertMathAction);
            auto* capture
                = menu->addAction(QStringLiteral("选段生成便签"), this, &MainWindow::captureSelection);
            capture->setEnabled(m_sourceEditor->textCursor().hasSelection());
            menu->exec(m_sourceEditor->mapToGlobal(point));
            delete menu;
        });
}
void MainWindow::loadSourceState(qint64 noteId, const NoteRecord& note)
{
    m_sourceBar->setVisible(m_linkedSource.has_value());
    if (m_linkedSource)
    {
        m_sourceEditor->setPlainText(m_sourceSnapshot ? m_sourceSnapshot->text : note.plainText);
        m_sourceEditor->setReadOnly(!m_sourceSnapshot || !m_sourceLoadError.isEmpty());
        m_sourceState->setText(m_sourceLoadError.isEmpty()
                ? QStringLiteral("源文件 · %1 · %2")
                      .arg(QFileInfo(m_linkedSource->sourcePath).fileName(),
                          SourceFile::encodingName(m_sourceSnapshot->encoding))
                : m_sourceLoadError);
        m_sourceState->setToolTip(m_linkedSource->sourcePath);
        m_bodyStack->setCurrentIndex(1);
        m_sourceToggle->setText(QStringLiteral("预览"));
    }
    else
    {
        m_sourceEditor->clear();
        m_bodyStack->setCurrentIndex(0);
    }
    m_findBar->setSourceMode(m_linkedSource.has_value());
    auto store = WorkspaceStore(*m_database);
    const auto state = store.activity(noteId);
    const bool sourceMode = m_linkedSource && (!state.openedAt.isValid() || state.sourceMode);
    m_bodyStack->setCurrentIndex(sourceMode ? 1 : 0);
    m_findBar->setSourceMode(sourceMode);
    m_sourceToggle->setText(sourceMode ? QStringLiteral("预览") : QStringLiteral("源码"));
    QTextDocument* document = sourceMode ? m_sourceEditor->document() : m_editor->document();
    QTextCursor cursor(document);
    cursor.setPosition(std::clamp(state.cursor, 0, document->characterCount() - 1));
    if (sourceMode)
        m_sourceEditor->setTextCursor(cursor);
    else
        m_editor->setTextCursor(cursor);
    const QString search = m_searchEdit->text().trimmed();
    if (!search.isEmpty())
    {
        auto found = document->find(search, 0);
        if (!found.isNull())
        {
            if (sourceMode)
                m_sourceEditor->setTextCursor(found);
            else
                m_editor->setTextCursor(found);
        }
    }
    QTimer::singleShot(0, this,
        [this, noteId, scroll = state.scroll, search]
        {
            if (m_currentNoteId != noteId)
                return;
            if (search.isEmpty())
                (m_bodyStack->currentIndex() == 1 ? m_sourceEditor->verticalScrollBar()
                                                  : m_editor->verticalScrollBar())
                    ->setValue(scroll);
            else if (m_bodyStack->currentIndex() == 1)
                m_sourceEditor->ensureCursorVisible();
            else
                m_editor->ensureCursorVisible();
        });
    store.recordPosition(noteId, state.cursor, state.scroll, true, nullptr, sourceMode);
}
void MainWindow::renderSourcePreview()
{
    if (!m_linkedSource)
        return;
    QString html, plain;
    DocumentImporter::renderDocument(m_sourceEditor->toPlainText(), m_linkedSource->sourceKind,
        QFileInfo(m_linkedSource->sourcePath).absolutePath(), &html, &plain);
    const bool loading = m_loadingNote;
    m_loadingNote = true;
    m_editor->setHtml(html);
    m_editor->setReadOnly(true);
    m_outline->rebuild(true);
    m_loadingNote = loading;
}
void MainWindow::toggleSourceView()
{
    if (!m_linkedSource)
        return;
    const bool preview = m_bodyStack->currentIndex() == 1;
    if (preview)
        renderSourcePreview();
    m_bodyStack->setCurrentIndex(preview ? 0 : 1);
    m_sourceToggle->setText(preview ? QStringLiteral("源码") : QStringLiteral("预览"));
    m_findBar->setSourceMode(!preview);
}
void MainWindow::convertCurrentNoteMath()
{
    if (m_currentNoteId <= 0 || m_currentNoteKind == "sticky") return;
    resumeHeavyContent();
    if (!saveCurrentNote()) return;
    const bool source = m_linkedSource.has_value();
    const bool previewOnly = source && (m_linkedSource->sourceKind == "html" || m_linkedSource->sourceKind == "htm");
    if (source && !previewOnly && m_sourceEditor->isReadOnly()) {
        setStatusMessage(QStringLiteral("源文件暂不可写，请先处理来源栏中的提示。"), true);
        return;
    }
    if (previewOnly) renderSourcePreview();
    QTextDocument* document = source && !previewOnly ? m_sourceEditor->document() : m_editor->document();
    const bool wasLoading = m_loadingNote;
    if (previewOnly) m_loadingNote = true;
    const auto result = MathSupport::convertAllMath(*document,
        source && !previewOnly ? MathSupport::ConversionTarget::MarkdownSource : MathSupport::ConversionTarget::RichText);
    m_loadingNote = wasLoading;
    if (result.converted && !previewOnly) {
        QString error;
        if (!WorkspaceStore(*m_database).capture(m_currentNoteId, QStringLiteral("全文公式转换前"), &error, true)) {
            document->undo();
            setStatusMessage(QStringLiteral("未能保留转换前版本，已撤销转换：%1").arg(error), true);
            return;
        }
        scheduleSave();
        if (!saveCurrentNote()) return;
    }
    if (source) {
        if (!previewOnly) renderSourcePreview();
        m_bodyStack->setCurrentIndex(0);
        m_sourceToggle->setText(QStringLiteral("源码"));
        m_findBar->setSourceMode(false);
    }
    m_outline->rebuild();
    updateDocumentInfo();
    m_editor->setFocus();
    QString message = QStringLiteral("已转换 %1 处公式 · 已有 %2 处 · 保留源码 %3 处")
        .arg(result.converted).arg(result.alreadyFormatted).arg(result.skipped);
    if (!result.converted && !result.alreadyFormatted && !result.skipped)
        message = QStringLiteral("本文没有可转换的 LaTeX；代码块和普通文本保持原样。" );
    if (previewOnly) message += QStringLiteral(" · HTML 仅转换预览，原文件保留");
    else if (result.converted) message += source ? QStringLiteral(" · 在源码页按 Ctrl+Z 撤销") : QStringLiteral(" · Ctrl+Z 撤销");
    setStatusMessage(message, result.skipped > 0);
    if (!result.errors.isEmpty()) m_saveStateLabel->setToolTip(message + "\n" + result.errors.join("\n"));
}
bool MainWindow::keepRecoveryDraft(const QString& html, const QString& plainText)
{
    if (m_currentNoteId <= 0)
        return false;
    NoteSnapshot draft;
    draft.note.id = m_currentNoteId;
    draft.note.title = m_titleEdit->text();
    draft.note.html = html;
    draft.note.plainText = plainText;
    draft.note.folderId = m_currentFolderId;
    draft.note.kind = m_currentNoteKind;
    if (m_linkedSource)
    {
        draft.sourcePath = m_linkedSource->sourcePath;
        if (m_sourceSnapshot)
            draft.sourceBytes = SourceFile::encode(m_sourceEditor->toPlainText(), *m_sourceSnapshot);
    }
    return WorkspaceStore(*m_database).saveDraft(m_currentNoteId, draft);
}
void MainWindow::compareSource()
{
    if (!m_linkedSource)
        return;
    QString error;
    const auto external = SourceFile::read(m_linkedSource->sourcePath, &error);
    NocturneDialog dialog(this);
    dialog.setObjectName("sourceConflictDialog");
    dialog.setWindowTitle(QStringLiteral("比较源文件与编辑内容 · 夜航"));
    dialog.resize(940, 620);
    auto* layout = new QVBoxLayout(dialog.body());
    layout->setContentsMargins(22, 18, 22, 18);
    auto* explanation = new QLabel(external
            ? QStringLiteral("左侧是磁盘中的内容，右侧是夜航中的编辑。双方内容会保留，选择后再继续。")
            : error,
        dialog.body());
    explanation->setWordWrap(true);
    layout->addWidget(explanation);
    auto* row = new QHBoxLayout;
    auto* left = new QPlainTextEdit(dialog.body());
    auto* right = new QPlainTextEdit(dialog.body());
    left->setReadOnly(true);
    right->setReadOnly(true);
    left->setPlainText(external ? external->text : QStringLiteral("源文件暂不可读"));
    right->setPlainText(m_sourceEditor->toPlainText());
    row->addWidget(left);
    row->addWidget(right);
    layout->addLayout(row, 1);
    auto* buttons = new QHBoxLayout;
    auto* copy = new QPushButton(QStringLiteral("把我的内容另存为笔记"), dialog.body());
    copy->setObjectName("sourceKeepCopyButton");
    auto* reload = new QPushButton(QStringLiteral("采用外部版本，保留我的草稿"), dialog.body());
    reload->setObjectName("sourceAdoptExternalButton");
    auto* close = new QPushButton(QStringLiteral("继续编辑"), dialog.body());
    reload->setEnabled(external.has_value());
    buttons->addWidget(copy);
    buttons->addWidget(reload);
    buttons->addStretch();
    buttons->addWidget(close);
    layout->addLayout(buttons);
    connect(close, &QPushButton::clicked, &dialog, &QDialog::reject);
    connect(copy, &QPushButton::clicked, &dialog,
        [&]
        {
            QString html, plain;
            DocumentImporter::renderDocument(m_sourceEditor->toPlainText(), m_linkedSource->sourceKind,
                QFileInfo(m_linkedSource->sourcePath).absolutePath(), &html, &plain);
            const auto id = m_database->createNote(
                m_titleEdit->text() + QStringLiteral("（编辑副本）"), html, plain, &error, m_currentFolderId);
            if (!id)
            {
                explanation->setText(error);
                return;
            }
            WorkspaceStore(*m_database).clearDraft(m_currentNoteId);
            m_dirty = false;
            m_saveTimer->stop();
            dialog.accept();
            openNoteById(id);
        });
    connect(reload, &QPushButton::clicked, &dialog,
        [&]
        {
            QString html, plain;
            DocumentImporter::renderDocument(m_sourceEditor->toPlainText(), m_linkedSource->sourceKind,
                QFileInfo(m_linkedSource->sourcePath).absolutePath(), &html, &plain);
            if (!keepRecoveryDraft(html, plain))
            {
                explanation->setText(QStringLiteral("草稿保留失败，尚未替换编辑内容。"));
                return;
            }
            m_dirty = false;
            m_saveTimer->stop();
            m_noteCache.remove(m_currentNoteId);
            dialog.accept();
            loadNote(m_currentNoteId);
        });
    dialog.exec();
}
void MainWindow::reloadSource()
{
    if (!m_linkedSource)
        return;
    if (m_dirty)
    {
        compareSource();
        return;
    }
    m_noteCache.remove(m_currentNoteId);
    loadNote(m_currentNoteId);
}
void MainWindow::relinkSource()
{
    if (!m_linkedSource)
        return;
    if (m_dirty)
    {
        compareSource();
        return;
    }
    const auto files = NocturneDialogs::getOpenFileNames(this, QStringLiteral("重新关联源文件"),
        QFileInfo(m_linkedSource->sourcePath).absolutePath(),
        QStringLiteral("文档 (*.md *.markdown *.txt *.html *.htm)"));
    if (files.isEmpty())
        return;
    QString error;
    auto source = SourceFile::read(files.first(), &error);
    if (!source)
    {
        setStatusMessage(error, true);
        return;
    }
    const QString kind = QFileInfo(source->path).suffix().toLower();
    QString html, plain;
    DocumentImporter::renderDocument(
        source->text, kind, QFileInfo(source->path).absolutePath(), &html, &plain);
    WorkspaceStore store(*m_database);
    if (!store.capture(m_currentNoteId, QStringLiteral("重新关联前"), &error, true)
        || !m_database->updateNote(m_currentNoteId, m_titleEdit->text(), html, plain, &error,
            QStringLiteral("重新关联"), m_currentContentHash, &source->bytes, source->path, kind))
    {
        setStatusMessage(error, true);
        return;
    }
    m_noteCache.remove(m_currentNoteId);
    loadNote(m_currentNoteId);
}
void MainWindow::refreshLinkedFolders()
{
    if (m_sourceRefresh)
        return;
    if (!saveCurrentNote())
        return;
    const auto* selected = m_noteList->currentItem();
    const qint64 scope = selected && selected->isFolder() ? selected->data(Qt::UserRole).toLongLong()
                                                          : m_folderFilter->currentData().toLongLong();
    QString scopeError;
    auto roots = WorkspaceStore(*m_database).linkedFolderRoots(scope, &scopeError);
    if (roots.isEmpty())
    {
        setStatusMessage(scopeError.isEmpty() ? QStringLiteral("尚未指定刷新目录；请在具体文档目录右键选择“设置为刷新目录”。") : scopeError, !scopeError.isEmpty());
        return;
    }
    QHash<qint64, qint64> parents;
    for (const auto& folder : m_database->listFolders())
        parents.insert(folder.id, folder.parentId);
    QStringList paths;
    QHash<QString, qint64> rootParents;
    for (const auto& root : roots)
    {
        paths.append(root.second);
        rootParents.insert(root.second, parents.value(root.first));
    }
    auto* worker = new DocumentImporter(paths, 0, this, true);
    worker->setRefreshMode();
    worker->setRootParents(rootParents);
    m_sourceRefresh = worker;
    auto* button = findChild<QToolButton*>("refreshLinkedFoldersButton");
    button->setEnabled(false);
    setStatusMessage(QStringLiteral("正在后台刷新外部目录…"));
    connect(worker, &DocumentImporter::progress, this,
        [this](int imported, int skipped, const QString&)
        {
            setStatusMessage(QStringLiteral("刷新外部目录 · 更新 %1 · 无变化 %2").arg(imported).arg(skipped));
        });
    connect(worker, &QThread::finished, this,
        [this, worker, button]
        {
            m_sourceRefresh = nullptr;
            button->setEnabled(true);
            const QString result = worker->errors.isEmpty()
                ? QStringLiteral("目录刷新完成 · 更新 %1 · 无变化 %2")
                      .arg(worker->imported)
                      .arg(worker->skipped)
                : worker->errors.join('\n');
            const bool failed = !worker->errors.isEmpty();
            worker->deleteLater();
            refreshFolders();
            refreshNotes(m_currentNoteId);
            setStatusMessage(result, failed);
        });
    worker->start(QThread::LowPriority);
}

void MainWindow::relinkSelectedDirectory()
{
    if (!saveCurrentNote())
        return;
    const qint64 id = selectedFolderFilter();
    const QString target
        = NocturneDialogs::getExistingDirectory(this, QStringLiteral("选择对接目录的新位置"));
    if (target.isEmpty())
        return;
    QString error;
    int count = 0;
    if (!WorkspaceStore(*m_database).relinkFolder(id, target, &error, &count))
    {
        setStatusMessage(error, true);
        return;
    }
    m_noteCache.clear();
    loadNote(m_currentNoteId);
    setStatusMessage(QStringLiteral("目录路径已重新关联 · %1 个文件引用已更新").arg(count));
}
void MainWindow::configureSelectedRefreshRoot()
{
    WorkspaceStore store(*m_database);
    const QString path = store.linkedFolderPath(selectedFolderFilter());
    if (path.isEmpty()) return;
    const bool enabled = store.refreshRootPaths().contains(path, Qt::CaseInsensitive);
    if (!enabled && NocturneDialogs::question(this, QStringLiteral("确认刷新范围"),
            QStringLiteral("今后刷新会读取以下目录及所有子目录中的 MD、TXT、HTML 文档：\n\n%1\n\n"
                           "请选择实际存放文档的目录。整个项目目录可能包含大量生成文件。")
                .arg(QDir::toNativeSeparators(path)), QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel) != QMessageBox::Yes) return;
    QString error;
    if (!store.setRefreshRoot(path, !enabled, &error)) { setStatusMessage(error, true); return; }
    setStatusMessage(enabled ? QStringLiteral("已取消此目录的独立刷新设置，笔记保留。")
                             : QStringLiteral("已保存刷新目录；刷新不会自动扩大到上级目录。"));
}
void MainWindow::rememberReadingPosition()
{
    if (m_currentNoteId <= 0 || m_loadingNote || m_contentSuspended || !m_bodyStack)
        return;
    const bool source = m_bodyStack->currentIndex() == 1;
    WorkspaceStore(*m_database)
        .recordPosition(m_currentNoteId,
            source ? m_sourceEditor->textCursor().position() : m_editor->textCursor().position(),
            source ? m_sourceEditor->verticalScrollBar()->value() : m_editor->verticalScrollBar()->value(),
            false, nullptr, source);
}
void MainWindow::openNoteById(qint64 id)
{
    if (!m_database->note(id))
    {
        setStatusMessage(QStringLiteral("这篇笔记已被移除，可以到恢复中心查看。"), true);
        return;
    }
    if (!saveCurrentNote())
        return;
    m_viewMode = "all";
    {
        QSignalBlocker b(m_folderFilter);
        m_folderFilter->setCurrentIndex(0);
    }
    {
        QSignalBlocker b(m_searchEdit);
        m_searchEdit->clear();
    }
    refreshNotes(id);
}
void MainWindow::navigateHistory(int delta)
{
    const int index = m_navigationIndex + delta;
    if (index < 0 || index >= m_navigationHistory.size() || !saveCurrentNote())
        return;
    m_historyJump = true;
    m_navigationIndex = index;
    openNoteById(m_navigationHistory[index]);
    m_historyJump = false;
    m_backButton->setEnabled(m_navigationIndex > 0);
    m_forwardButton->setEnabled(m_navigationIndex + 1 < m_navigationHistory.size());
}
void MainWindow::captureSelection()
{
    const bool source = m_bodyStack->currentIndex() == 1;
    const auto selection = source ? m_sourceEditor->textCursor() : m_editor->textCursor();
    const QString text = selection.selectedText().replace(QChar::ParagraphSeparator, '\n');
    if (text.trimmed().isEmpty())
        return;
    if (!saveCurrentNote())
        return;
    QString error;
    const qint64 id = m_database->saveStickyNote(0, text, &error);
    if (id <= 0
        || !WorkspaceStore(*m_database)
            .setCaptureSource(id, m_currentNoteId, selection.selectionStart(), text, &error))
    {
        setStatusMessage(error, true);
        return;
    }
    openSticky(id);
    setStatusMessage(QStringLiteral("已生成便签，原文保留，可从便签返回来源。"));
}
