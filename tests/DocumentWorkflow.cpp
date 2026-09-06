#include "Database.h"
#include "ProfileMigration.h"
#include "DocumentImporter.h"
#include "MainWindow.h"
#include "NoteEditor.h"
#include "NotebookTree.h"
#include "NocturneStyle.h"
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QImage>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeData>
#include <QMenu>
#include <QFileDialog>
#include <QDialog>
#include <QMouseEvent>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextFragment>
#include <QTextLayout>
#include <QTextTable>
#include <QAbstractTextDocumentLayout>
#include <QTimer>
#include <QToolButton>
#include <iostream>

namespace {
void settle(int ms = 220) { QEventLoop loop; QTimer::singleShot(ms, &loop, &QEventLoop::quit); loop.exec(); }
void key(QWidget* widget, int key, const QString& text = QString()) {
    QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier, text); QApplication::sendEvent(widget, &press);
    QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier, text); QApplication::sendEvent(widget, &release);
}
}

bool runDocumentWorkflowTests(Database& database, MainWindow& window, const QString& outputDirectory)
{
    std::cerr << "WORKFLOW stage: folders\n";
    bool ok = true;
    auto check = [&](bool value, const char* message) {
        if (!value) { std::cerr << "FAIL WORKFLOW: " << message << '\n'; ok = false; }
    };
    QString error;
    const qint64 root = database.createFolder(QStringLiteral("工作资料"), &error);
    const qint64 projectA = database.createFolder(QStringLiteral("项目甲"), &error, root);
    const qint64 projectB = database.createFolder(QStringLiteral("项目乙"), &error, root);
    const qint64 childA = database.createFolder(QStringLiteral("方案"), &error, projectA);
    const qint64 childB = database.createFolder(QStringLiteral("方案"), &error, projectB);
    check(root && projectA && projectB && childA && childB && childA != childB, "same child names are valid under different parents");
    check(!database.moveFolder(root, childA, &error), "folder cycle is rejected");
    check(database.moveFolder(childB, root, &error), "folder moves to a valid parent");
    check(database.moveFolder(childB, projectB, &error), "folder can move back");
    check(!database.moveFolder(childB, projectA, &error), "sibling name collision is rejected without mutation");

    QTemporaryDir source;
    const QString sourceRoot = source.path() + QStringLiteral("/历史文档");
    QDir().mkpath(sourceRoot + QStringLiteral("/第一组/方案"));
    QDir().mkpath(sourceRoot + QStringLiteral("/第二组/方案"));
    auto write = [&](const QString& name, const QByteArray& bytes) {
        const QString path = sourceRoot + "/" + name;
        QFile file(path); check(file.open(QIODevice::WriteOnly), "fixture file opens");
        check(file.write(bytes) == bytes.size(), "fixture file writes");
        return path;
    };
    const QString markdownPath = write(QStringLiteral("第一组/方案/分级文档.md"),
        QStringLiteral("# 项目计划\n\n## 阶段目标\n\n### 本周行动\n\n准备项目资料\n\n- 校对目录\n- 核对文档\n\n**重点**与*补充*\n\n```cpp\nint value = 1;\n```\n").toUtf8());
    write(QStringLiteral("第二组/方案/普通文本.txt"), QStringLiteral("这是普通文本。").toUtf8());
    write(QStringLiteral("UTF16.txt"), QByteArray::fromHex("fffe2d4e8765"));
#ifdef Q_OS_WIN
    write(QStringLiteral("GB18030.txt"), QByteArray::fromHex("d6d0cec4"));
#endif
    write(QStringLiteral("损坏文本.txt"), QByteArray::fromHex("0001000200"));
    auto runWorker = [&](DocumentImporter& worker) {
        QEventLoop loop;
        QTimer timeout; timeout.setSingleShot(true);
        QObject::connect(&worker, &QThread::finished, &loop, &QEventLoop::quit);
        QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        worker.start(); timeout.start(15000); loop.exec();
        if (worker.isRunning()) worker.requestInterruption();
        return worker.wait(1000);
    };
    std::cerr << "WORKFLOW stage: import\n";
    DocumentImporter importer({sourceRoot}, root);
    check(runWorker(importer), "batch import completes");
    if (importer.isRunning()) { importer.requestInterruption(); importer.wait(); return false; }
#ifdef Q_OS_WIN
    check(importer.imported == 4, "UTF8, UTF16, GB18030 and Markdown import");
#else
    check(importer.imported == 3, "UTF8, UTF16 and Markdown import");
#endif
    check(importer.errors.size() == 1, "bad text is reported while valid files succeed");
    const auto matches = database.listNoteSummaries(QStringLiteral("分级文档"), &error, root);
    check(matches.size() == 1, "root folder query includes descendant notes");
    if (matches.isEmpty()) return false;
    const qint64 importedId = matches.first().id;
    const auto imported = database.note(importedId);
    check(imported && imported->html.contains("<h1") && imported->html.contains("<h2"), "import preserves semantic heading levels");
    check(database.noteSourcePath(importedId) == QFileInfo(markdownPath).canonicalFilePath(), "source file is retained as provenance");
    const auto importedFolders = database.listFolders();
    QHash<qint64, FolderRecord> folderMap;
    for (const auto& folder : importedFolders) folderMap.insert(folder.id, folder);
    check(folderMap.value(imported->folderId).name == QStringLiteral("方案")
        && folderMap.value(folderMap.value(imported->folderId).parentId).name == QStringLiteral("第一组"),
        "folder import preserves real nesting");
    DocumentImporter repeat({sourceRoot}, root);
    runWorker(repeat);
    check(repeat.imported == 0 && repeat.skipped >= 3, "reimport skips previously imported source bytes");
    const auto originalText = imported->plainText;
    write(QStringLiteral("第一组/方案/分级文档.md"), QStringLiteral("# 外部新版本\n\n改过的内容").toUtf8());
    DocumentImporter changed({markdownPath}, root);
    runWorker(changed);
    check(changed.imported == 1 && database.note(importedId)->plainText == originalText,
        "changed external file creates a copy and preserves previous edits");
    QFile sourceFile(markdownPath); check(sourceFile.open(QIODevice::ReadOnly), "source reopens for verification");
    check(sourceFile.readAll() == QStringLiteral("# 外部新版本\n\n改过的内容").toUtf8(), "import never writes to source files");

    // Cancelled batches keep their committed prefix and never continue in the background.
    const QString cancelRoot = source.path() + "/cancel";
    QDir().mkpath(cancelRoot);
    for (int i = 0; i < 100; ++i) {
        QFile file(cancelRoot + QStringLiteral("/%1.txt").arg(i));
        check(file.open(QIODevice::WriteOnly), "cancellation fixture opens"); file.write("cancellable fixture");
    }
    DocumentImporter cancelled({cancelRoot}, root);
    cancelled.start(); cancelled.requestInterruption();
    check(cancelled.wait(15000) && cancelled.imported < 100, "batch cancellation stops the worker");

    std::cerr << "WORKFLOW stage: Markdown\n";
    NoteEditor scratch;
    scratch.setPlainText(QStringLiteral("语义标题"));
    scratch.applyHeadingLevel(4);
    check(scratch.textCursor().blockFormat().headingLevel() == 4
        && scratch.toMarkdown().contains(QStringLiteral("#### 语义标题")), "H4 exports as a Markdown heading");
    scratch.applyHeadingLevel(0);
    check(scratch.textCursor().blockFormat().headingLevel() == 0, "body resets heading semantics");
    scratch.setPlainText("###");
    QTextCursor typing = scratch.textCursor(); typing.movePosition(QTextCursor::End); scratch.setTextCursor(typing);
    key(&scratch, Qt::Key_Space, " ");
    check(scratch.textCursor().blockFormat().headingLevel() == 3 && scratch.toPlainText().isEmpty(), "typed Markdown heading converts on space");
    key(&scratch, Qt::Key_A, "标题");
    key(&scratch, Qt::Key_Return, "\n");
    check(scratch.textCursor().blockFormat().headingLevel() == 0, "Enter after heading returns to body");
    scratch.clear();
    for (QChar c : QStringLiteral("**重点**")) key(&scratch, c.unicode(), QString(c));
    check(scratch.toPlainText() == QStringLiteral("重点") && scratch.toMarkdown().contains(QStringLiteral("**重点**")),
        "typed bold Markdown becomes real rich text");
    scratch.setPlainText(QStringLiteral("代码"));
    scratch.applyCodeBlock();
    check(scratch.toMarkdown().contains("```"), "code block exports fenced Markdown");

    std::cerr << "WORKFLOW stage: import UI\n";
    QTimer inspectImport;
    inspectImport.setSingleShot(true);
    QObject::connect(&inspectImport, &QTimer::timeout, &window, [&] {
        if (auto* modal = qobject_cast<QDialog*>(QApplication::activeModalWidget())) {
            check(false, "import UI timed out"); modal->reject();
        }
    });
    inspectImport.start(10000);
    bool filePicked = false, resultSeen = false, chooseFolder = false;
    QTimer importDriver;
    QObject::connect(&importDriver, &QTimer::timeout, &window, [&] {
        if (auto* picker = window.findChild<QFileDialog*>(chooseFolder ? QStringLiteral("directoryImportDialog") : QStringLiteral("nocturneFileDialog")); picker) {
            if (!filePicked) {
                filePicked = true;
                picker->setDirectory(sourceRoot);
                picker->selectFile(chooseFolder ? QStringLiteral(".") : QStringLiteral("UTF16.txt"));
            } else {
                picker->selectFile(chooseFolder ? QStringLiteral(".") : QStringLiteral("UTF16.txt"));
                if (auto* fileName = picker->findChild<QLineEdit*>(QStringLiteral("fileNameEdit")))
                    fileName->setText(chooseFolder ? QStringLiteral(".") : QStringLiteral("UTF16.txt"));
                QMetaObject::invokeMethod(picker, "accept", Qt::DirectConnection);
            }
        }
        if (auto* result = window.findChild<QDialog*>(QStringLiteral("nocturneMessageDialog"));
            result && result->windowTitle().contains(QStringLiteral("导入结果"))) {
            resultSeen = true; result->accept();
        }
    });
    importDriver.start(60);
    window.findChild<QAction*>(QStringLiteral("importFilesAction"))->trigger();
    importDriver.stop();
    inspectImport.stop();
    check(filePicked && resultSeen, "file import action runs picker, worker, and result dialog");
    chooseFolder = true; filePicked = false; resultSeen = false;
    const auto foldersBeforeRepeat = database.listFolders().size();
    inspectImport.start(10000); importDriver.start(60);
    window.findChild<QAction*>(QStringLiteral("importFolderAction"))->trigger();
    importDriver.stop(); inspectImport.stop();
    check(filePicked && resultSeen, "directory import action runs the complete UI workflow");
    check(database.listFolders().size() == foldersBeforeRepeat, "reimport from another selection does not create empty duplicate directories");

    std::cerr << "WORKFLOW stage: annotation\n";
    auto* filter = window.findChild<QComboBox*>(QStringLiteral("folderFilter"));
    auto* search = window.findChild<QLineEdit*>(QStringLiteral("searchEdit"));
    auto* editor = window.findChild<NoteEditor*>(QStringLiteral("noteEditor"));
    auto* tree = window.findChild<NotebookTree*>(QStringLiteral("noteList"));
    auto* todos = window.findChild<QListWidget*>(QStringLiteral("todoList"));
    filter->setCurrentIndex(0); search->setText(QStringLiteral("分级文档")); settle();
    // There are two imported revisions; select the first one explicitly.
    int row = -1;
    for (int i = 0; i < tree->count(); ++i) if (tree->item(i)->data(Qt::UserRole).toLongLong() == importedId) row = i;
    check(row >= 0, "imported note appears in the navigation tree");
    if (row < 0) return false;
    tree->setCurrentRow(row); settle();
    auto* item = tree->currentItem();
    check(item && item->parent() && item->parent()->parent() && item->parent()->parent()->parent(),
        "actual navigation renders multiple folder levels");
    QTextCursor selection = editor->document()->find(QStringLiteral("准备项目资料"));
    check(!selection.isNull(), "source phrase is present");
    editor->setTextCursor(selection);
    window.findChild<QAction*>(QStringLiteral("selectionTodoAction"))->trigger(); settle();
    QList<TodoRecord> linked;
    for (const auto& todo : database.listTodos()) if (todo.noteId == importedId) linked.append(todo);
    check(linked.size() == 1, "selected text creates a linked todo");
    if (linked.isEmpty()) return false;
    const auto todo = linked.first();
    check(database.note(importedId)->html.contains(QStringLiteral("nocturne-todo:") + todo.anchor), "todo anchor survives storage");
    check(!editor->markdownForExport().contains(QStringLiteral("nocturne-todo:"))
        && editor->markdownForExport().contains(QStringLiteral("准备项目资料")),
        "Markdown export retains todo text without internal identifiers");
    bool marked = false;
    for (auto block = editor->document()->begin(); block.isValid(); block = block.next())
        for (const auto& range : block.layout()->formats())
            marked |= range.format.background().color() == QColor("#101924");
    check(marked, "selected todo text has a dark-background overlay");
    editor->undo(); settle(750);
    bool visibleAfterUndo = false;
    for (const auto& value : database.listTodos()) visibleAfterUndo |= value.id == todo.id;
    check(!visibleAfterUndo, "undo removes annotation and hides its todo");
    editor->redo(); settle(750);
    bool visibleAfterRedo = false;
    for (const auto& value : database.listTodos()) visibleAfterRedo |= value.id == todo.id;
    check(visibleAfterRedo, "redo restores the same todo identity");
    QTextCursor prefix(editor->document()); prefix.movePosition(QTextCursor::Start);
    prefix.insertText(QStringLiteral("补充说明\n")); // deliberately still dirty when navigating back
    QListWidgetItem* todoItem = nullptr;
    for (int i = 0; i < todos->count(); ++i)
        if (todos->item(i)->data(Qt::UserRole).toLongLong() == todo.id) todoItem = todos->item(i);
    check(todoItem, "linked todo row exists");
    if (todoItem) todos->itemDoubleClicked(todoItem);
    check(editor->textCursor().selectedText() == QStringLiteral("准备项目资料"), "todo navigation follows text after preceding edits");
    for (int i = 0; i < todos->count(); ++i) {
        if (todos->item(i)->data(Qt::UserRole).toLongLong() == todo.id) {
            todos->item(i)->setCheckState(Qt::Checked); break;
        }
    }
    bool completed = false;
    for (const auto& value : database.listTodos()) if (value.id == todo.id) completed = value.done;
    check(completed, "linked todo completion persists through the UI");
    NoteEditor restored;
    restored.setHtml(database.note(importedId)->html);
    restored.setTodoStates({{todo.anchor, true}});
    check(restored.locateTodo(todo.anchor) && restored.textCursor().selectedText() == QStringLiteral("准备项目资料"),
        "todo anchor and selected text restore from saved HTML into a new editor");
    search->setText(QStringLiteral("分级文档")); settle();
    for (int i = 0; i < tree->count(); ++i)
        if (tree->item(i)->data(Qt::UserRole).toLongLong() == importedId) tree->setCurrentRow(i);
    settle();
    check(editor->locateTodo(todo.anchor), "stored anchor can be located after reload");

    search->clear(); settle();
    for (int i = 0; i < tree->count(); ++i)
        if (tree->item(i)->data(Qt::UserRole).toLongLong() == importedId) tree->setCurrentRow(i);
    window.resize(1440, 900); settle();
    if (tree->currentItem()) tree->scrollToItem(tree->currentItem(), QAbstractItemView::PositionAtCenter);
    editor->locateTodo(todo.anchor);
    QTextCursor caret = editor->textCursor(); caret.clearSelection(); editor->setTextCursor(caret);
    check(window.grab().save(QDir(outputDirectory).filePath("Nocturne-document-workflow.png")), "workflow screenshot saves");
    auto* headings = window.findChild<QToolButton*>(QStringLiteral("headingButton"))->menu();
    headings->popup(window.findChild<QToolButton*>(QStringLiteral("headingButton"))->mapToGlobal(QPoint(0, 32)));
    settle(80);
    check(headings->grab().save(QDir(outputDirectory).filePath("Nocturne-heading-menu.png")), "heading menu screenshot saves");
    headings->hide();

    // Standard Qt editing actions retain their built-in behavior and Chinese labels.
    editor->locateTodo(todo.anchor);
    auto* editMenu = editor->createStandardContextMenu();
    QStringList editLabels;
    for (auto* action : editMenu->actions()) editLabels.append(action->text().section('\t', 0, 0).remove('&'));
    for (const QString& label : {QStringLiteral("撤销"), QStringLiteral("重做"), QStringLiteral("剪切"),
         QStringLiteral("复制"), QStringLiteral("粘贴"), QStringLiteral("删除"), QStringLiteral("全选")})
        check(editLabels.contains(label), "standard text editing menu is Chinese");
    editMenu->popup(editor->mapToGlobal(QPoint(20, 20))); settle(60);
    check(editMenu->grab().save(QDir(outputDirectory).filePath("Nocturne-chinese-edit-menu.png")), "Chinese menu screenshot saves");
    editMenu->hide(); delete editMenu;
    QLineEdit field; field.setText("test"); field.selectAll();
    auto* fieldMenu = field.createStandardContextMenu();
    bool fieldCopy = false;
    for (auto* action : fieldMenu->actions()) fieldCopy |= action->text().contains(QStringLiteral("复制"));
    check(fieldCopy, "line edit menus are also Chinese"); delete fieldMenu;

    QTextCursor linkPosition = editor->textCursor();
    linkPosition.setPosition(linkPosition.selectionStart() + 1);
    editor->setTextCursor(linkPosition); editor->ensureCursorVisible(); settle(40);
    QPoint linkPoint = editor->cursorRect(linkPosition).center() + QPoint(2, 0);
    auto sendMouse = [&](QEvent::Type type, const QPoint& point, Qt::MouseButton button, Qt::MouseButtons buttons) {
        QMouseEvent event(type, point, editor->viewport()->mapToGlobal(point), button, buttons, Qt::NoModifier);
        QApplication::sendEvent(editor->viewport(), &event);
    };
    bool detailsSeen = false, sourceSeen = false;
    QTimer::singleShot(70, &window, [&] {
        auto* dialog = window.findChild<QDialog*>(QStringLiteral("todoDetailsDialog"));
        if (!dialog) return;
        auto* detailText = dialog->findChild<QTextEdit*>(QStringLiteral("todoDetailText"));
        auto* detailSource = dialog->findChild<QLabel*>(QStringLiteral("todoDetailSource"));
        detailsSeen = detailText && detailText->toPlainText() == todo.text;
        sourceSeen = detailSource && detailSource->text().contains(QStringLiteral("分级文档"));
        check(dialog->grab().save(QDir(outputDirectory).filePath("Nocturne-todo-details.png")), "todo details screenshot saves");
        dialog->findChild<QPushButton*>(QStringLiteral("todoDetailToggle"))->click();
        check(dialog->findChild<QLabel*>(QStringLiteral("todoDetailStatus"))->text().contains(QStringLiteral("未完成")),
            "details can restore a completed todo");
        dialog->findChild<QPushButton*>(QStringLiteral("todoDetailClose"))->click();
    });
    sendMouse(QEvent::MouseButtonPress, linkPoint, Qt::LeftButton, Qt::LeftButton);
    sendMouse(QEvent::MouseButtonRelease, linkPoint, Qt::LeftButton, Qt::NoButton);
    check(detailsSeen && sourceSeen, "plain click on a todo anchor opens its full details and source");
    int activatedOnDrag = 0;
    const auto dragConnection = QObject::connect(editor, &NoteEditor::todoActivated, &window, [&](const QString&) { ++activatedOnDrag; });
    sendMouse(QEvent::MouseButtonPress, linkPoint, Qt::LeftButton, Qt::LeftButton);
    sendMouse(QEvent::MouseMove, linkPoint + QPoint(45, 0), Qt::NoButton, Qt::LeftButton);
    sendMouse(QEvent::MouseButtonRelease, linkPoint + QPoint(45, 0), Qt::LeftButton, Qt::NoButton);
    check(activatedOnDrag == 0 && editor->textCursor().hasSelection(), "dragging todo text selects instead of opening details");
    QObject::disconnect(dragConnection);

    const QString bodyBeforeDelete = database.note(importedId)->plainText;
    const int countBeforeDelete = database.listTodos().size();
    QListWidgetItem* deleteItem = nullptr;
    for (int i = 0; i < todos->count(); ++i)
        if (todos->item(i)->data(Qt::UserRole).toLongLong() == todo.id) deleteItem = todos->item(i);
    check(deleteItem, "individual todo is available for deletion");
    if (deleteItem) {
        todos->scrollToItem(deleteItem);
        const QPoint position = todos->visualItemRect(deleteItem).center();
        QTimer::singleShot(60, &window, [&] {
            auto* menu = window.findChild<QMenu*>(QStringLiteral("todoContextMenu"));
            if (!menu) return;
            auto* remove = menu->findChild<QAction*>(QStringLiteral("todoDeleteAction"));
            if (remove) { menu->setActiveAction(remove); key(menu, Qt::Key_Return, "\n"); }
            else menu->close();
        });
        todos->customContextMenuRequested(position);
    }
    bool deleted = true;
    for (const auto& value : database.listTodos()) deleted &= value.id != todo.id;
    check(deleted && database.listTodos().size() == countBeforeDelete - 1, "context menu deletes only the requested todo");
    check(database.note(importedId)->plainText == bodyBeforeDelete, "deleting a linked todo preserves its source text");
    int staleActivation = 0;
    const auto staleConnection = QObject::connect(editor, &NoteEditor::todoActivated, &window, [&](const QString&) { ++staleActivation; });
    sendMouse(QEvent::MouseButtonPress, linkPoint, Qt::LeftButton, Qt::LeftButton);
    sendMouse(QEvent::MouseButtonRelease, linkPoint, Qt::LeftButton, Qt::NoButton);
    check(staleActivation == 0, "deleted todo anchors are no longer interactive");
    QObject::disconnect(staleConnection);
    const qint64 independentA = database.createTodo(QStringLiteral("独立待办 A"), &error);
    const qint64 independentB = database.createTodo(QStringLiteral("独立待办 B"), &error);
    check(database.deleteTodo(independentA, &error) && !database.deleteTodo(independentA, &error), "standalone deletion handles missing items");
    bool otherRemains = false;
    for (const auto& value : database.listTodos()) otherRemains |= value.id == independentB;
    check(otherRemains, "deleting one independent todo preserves the others");
    {
        NoteEditor media;
        media.resize(640, 600); media.show();
        QImage panorama(2000, 240, QImage::Format_RGB32); panorama.fill(Qt::darkCyan);
        media.document()->addResource(QTextDocument::ImageResource, QUrl("test-panorama"), panorama);
        media.setHtml("<p style='line-height:160%'><img src='test-panorama' width='2000' height='900'></p><p>After image</p>");
        settle(50);
        QTextCursor img(media.document()); img.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
        auto size = media.intrinsicSize(media.document(), 0, img.charFormat());
        check(size.width() <= media.viewport()->width() && qAbs(size.height()/size.width()-0.12)<0.001, "image layout preserves source ratio and fits viewport despite legacy dimensions");
        check(media.document()->firstBlock().blockFormat().lineHeightType() == QTextBlockFormat::SingleHeight, "image does not inherit proportional text line height");
        media.grab().save(outputDirectory + "/Nocturne-image-layout.png");
        QTextCursor imageCursor(media.document()); imageCursor.setPosition(0);
        const QRect caret = media.cursorRect(imageCursor);
        const QPointF imagePoint(caret.left() + size.width()/2, caret.bottom() - size.height()/2);
        auto imageMouse = [&](QEvent::Type type, Qt::MouseButtons buttons) {
            QMouseEvent event(type, imagePoint, imagePoint, Qt::LeftButton, buttons, Qt::NoModifier);
            QApplication::sendEvent(media.viewport(), &event);
        };
        bool previewOpened = false;
        QTimer::singleShot(100, &media, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            previewOpened = dialog && dialog->windowTitle() == QStringLiteral("图片预览");
            if (dialog) { dialog->grab().save(outputDirectory + "/Nocturne-image-preview.png"); dialog->reject(); }
        });
        imageMouse(QEvent::MouseButtonPress, Qt::LeftButton);
        imageMouse(QEvent::MouseButtonRelease, Qt::NoButton);
        imageMouse(QEvent::MouseButtonDblClick, Qt::LeftButton);
        check(previewOpened, "double click opens image preview instead of caption dialog");
        settle(QApplication::doubleClickInterval() + 30);
        check(!QApplication::activeModalWidget(), "double click cancels delayed caption editor");
        bool captionOpened = false;
        QTimer::singleShot(QApplication::doubleClickInterval() + 100, &media, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            captionOpened = dialog && dialog->windowTitle().contains(QStringLiteral("图片描述"));
            if (dialog) dialog->reject();
        });
        imageMouse(QEvent::MouseButtonPress, Qt::LeftButton);
        imageMouse(QEvent::MouseButtonRelease, Qt::NoButton);
        settle(QApplication::doubleClickInterval() + 180);
        check(captionOpened, "single click opens optional caption editor");
        const QString beforeCaption = media.toPlainText();
        media.setImageCaption(0, QStringLiteral("全景描述"));
        check(media.toPlainText().contains(QStringLiteral("全景描述")) && media.toPlainText().contains("After image"), "caption inserts below image and preserves following text");
        media.undo(); check(media.toPlainText() == beforeCaption, "caption undo is atomic"); media.redo();
        media.grab().save(outputDirectory + "/Nocturne-image-caption.png");
        check(!media.markdownForExport().contains("nocturne-caption:"), "Markdown export omits internal caption markers");
        const auto savedHtml = media.toHtml(); media.setHtml(savedHtml);
        media.setImageCaption(0, QStringLiteral("更新描述"));
        check(!media.toPlainText().contains(QStringLiteral("全景描述")) && media.toPlainText().contains(QStringLiteral("更新描述")), "caption edits in place after HTML reload");
        media.setImageCaption(0, QString());
        check(!media.toPlainText().contains(QStringLiteral("更新描述")) && media.toPlainText().contains("After image"), "empty caption removes only description");
        media.clear(); media.createTable(3, 2);
        auto* table = media.textCursor().currentTable();
        check(table && table->rows()==3 && table->columns()==2, "manual table creates editable cells");
        media.textCursor().insertText(QStringLiteral("表头"));
        const auto tableHtml = media.toHtml(); media.setHtml(tableHtml);
        check(media.toPlainText().contains(QStringLiteral("表头")) && media.toHtml().contains("<table"), "table cells survive HTML persistence");
        media.clear(); media.insertMarkdownText("| Name | Value |\n| --- | --- |\n| Alpha | 42 |\n");
        check(media.toHtml().contains("<table") && media.toPlainText().contains("Alpha"), "GitHub Markdown table becomes editable table");
        media.grab().save(outputDirectory + "/Nocturne-table.png");
        check(media.markdownForExport().contains("Alpha") && media.markdownForExport().contains('|'), "table exports to Markdown");
    }
    {
        NoteEditor conversion;
        conversion.setPlainText("Region\tTheme\tCraft\nSouth\tWater\tSilk\nNorth\tMountain\tIron");
        conversion.selectAll();
        const auto original = conversion.toPlainText();
        QString reason;
        check(conversion.selectionToTable(&reason), "tab-separated selection becomes table");
        check(conversion.toHtml().contains("<table") && conversion.toPlainText().contains("Mountain"), "conversion retains cell content");
        conversion.undo(); check(conversion.toPlainText() == original && !conversion.toHtml().contains("<table"), "selection conversion is one undo operation");
        conversion.redo();
        conversion.setHtml("<p>Before</p><table border='0'><tr><td>A</td><td>B</td></tr><tr><td>C</td><td>D</td></tr></table><p>After</p>");
        conversion.selectAll(); const auto existing = conversion.toPlainText();
        check(conversion.selectionToTable(&reason), "whole-document selection styles existing borderless table");
        check(conversion.toPlainText()==existing && conversion.toHtml().count("<table")==1, "existing table conversion preserves outside paragraphs and avoids nesting");
        conversion.undo(); check(conversion.toPlainText()==existing, "existing table restyle is reversible");
        conversion.setPlainText("| Region | Theme |\n| --- | --- |\n| South | **Water** |\n| North | Iron |"); conversion.selectAll();
        check(conversion.selectionToTable(&reason) && !conversion.toPlainText().contains("---") && conversion.toPlainText().contains("Water"), "Markdown table recognizes header separator and inline formatting");
        conversion.setPlainText("A\tB\nonly one cell"); conversion.selectAll(); const auto invalid = conversion.toHtml();
        check(!conversion.selectionToTable(&reason) && conversion.toHtml()==invalid, "ambiguous rows do not mutate selection");
        conversion.setPlainText("A  B\nC  D"); conversion.selectAll();
        check(conversion.selectionToTable(&reason), "aligned multiple spaces delimit columns");
        conversion.resize(700,400); conversion.show(); settle(50);
        conversion.grab().save(outputDirectory + "/Nocturne-selection-table.png");
    }
    {
        QTemporaryDir migrated;
        const QString destination = migrated.path() + "/UnifiedData";
        QString migrationError;
        check(prepareDailyData(database.dataDirectory(), destination, &migrationError), "daily profile migration creates consistent snapshot");
        QFile snapshot(destination + "/notebook.sqlite3");
        check(snapshot.open(QIODevice::ReadOnly), "migrated database is present");
        const auto bytes = snapshot.readAll(); snapshot.close();
        check(!bytes.isEmpty() && prepareDailyData(database.dataDirectory(), destination, &migrationError), "existing destination is retained on repeated migration");
        snapshot.open(QIODevice::ReadOnly); check(snapshot.readAll() == bytes, "repeat migration does not overwrite current data"); snapshot.close();
    }
    std::cout << (ok ? "PASS" : "FAIL") << ": document workflows (Markdown, task anchors, folders, imports)\n";
    return ok;
}
