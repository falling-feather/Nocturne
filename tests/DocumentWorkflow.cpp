#include "Database.h"
#include "ProfileMigration.h"
#include "DocumentImporter.h"
#include "MainWindow.h"
#include "NoteEditor.h"
#include "NotebookTree.h"
#include "NocturneStyle.h"
#include "MathSupport.h"
#include "WorkspaceStore.h"
#include "DocumentOutline.h"
#include "FolderComboBox.h"
#include <QScrollBar>
#include <QClipboard>
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
    {
        QTemporaryDir scope;
        QDir().mkpath(scope.filePath("project/docs"));
        QDir().mkpath(scope.filePath("project/data"));
        auto writeScoped = [&](const QString& path, const QByteArray& text) {
            QFile file(scope.filePath(path)); check(file.open(QIODevice::WriteOnly), "scope fixture opens");
            file.write(text);
        };
        writeScoped("project/docs/allowed.md", "# permitted");
        writeScoped("project/data/OUTSIDE_REFRESH.txt", "generated data must remain outside");
        writeScoped("OUTSIDE_REFRESH_ROOT.md", "workspace file must remain outside");
        const auto container = database.ensureLinkedFolder(scope.path(), "scope container", 0, &error);
        const auto project = database.ensureLinkedFolder(scope.filePath("project"), "project", container, &error);
        DocumentImporter initial({scope.filePath("project/docs")}, project, nullptr, true);
        initial.runNow();
        check(initial.errors.isEmpty() && initial.imported == 1, "explicit link registers only the selected docs root");
        WorkspaceStore store(database);
        const auto roots = store.linkedFolderRoots(container);
        check(roots.size() == 1 && roots.first().second == scope.filePath("project/docs"), "container resolves to its docs scan root");
        writeScoped("project/docs/new.md", "# newly discovered");
        if (roots.size() == 1) {
            DocumentImporter refresh({roots.first().second}, project, nullptr, true);
            refresh.setRefreshMode(); refresh.runNow();
            check(refresh.errors.isEmpty() && refresh.imported == 1, "refresh discovers a new in-scope document");
            check(database.listNoteSummaries("OUTSIDE_REFRESH").isEmpty(), "refresh does not import workspace files or generated project data");
            check(!store.refreshRootPaths().contains(scope.path()), "refresh never grants permission to the parent container");
        }
    }
    NoteEditor scratch;
    scratch.insertMarkdownText(QStringLiteral("文字 $\\frac{a_i}{\\sqrt{b}}$ 结束\n\n$$\n\\begin{pmatrix}1 & 2 \\\\ 3 & 4\\end{pmatrix}\n$$"));
    check(scratch.toHtml().contains(QStringLiteral("nocturne-math:")), "LaTeX becomes a rendered object with retained source");
    check(scratch.markdownForExport().contains(QStringLiteral("$\\frac{a_i}{\\sqrt{b}}$"))
        && scratch.markdownForExport().contains(QStringLiteral("\\begin{pmatrix}")), "math source survives Markdown export");
    NoteEditor reopenedMath;
    reopenedMath.setHtml(scratch.toHtml());
    check(reopenedMath.markdownForExport().contains(QStringLiteral("\\frac{a_i}{\\sqrt{b}}")), "math survives HTML persistence");
    scratch.clear();
    for (const QString& math : {QStringLiteral("\\varphi_i=\\frac{\\pi\\theta_i}{180}"),
            QStringLiteral("\\sum_{i=1}^{n}x_i^2"), QStringLiteral("\\sqrt{x^2+y^2}"),
            QStringLiteral("\\begin{pmatrix}1 & 2 \\\\ 3 & 4\\end{pmatrix}"),
            QStringLiteral("f(x)=\\begin{cases}x^2 & x>0 \\\\ 0 & x\\le0\\end{cases}"),
            QStringLiteral("\\begin{aligned}x&=1\\\\y&=2\\end{aligned}")}) {
        const auto result = MathSupport::render({math, true}, QColor("#c9d5e1"));
        if (!result.error.isEmpty()) std::cerr << "MATH ERROR: " << result.error.toStdString() << '\n';
        check(result.error.isEmpty() && !result.image.isNull(), "fractions, indices, sums, roots, matrices, cases and alignment render");
    }
    check(!MathSupport::render({QStringLiteral("\\unknownNocturneCommand{x}"), false}, Qt::white).error.isEmpty(), "unsupported LaTeX reports a recoverable error");
    check(!MathSupport::render({QStringLiteral("\\newcommand{\\a}{\\a}\\a"), false}, Qt::white).error.isEmpty(), "recursive macro definitions are rejected");
    const QString codeMath = QStringLiteral("`$x_i$`\n\n```tex\n$$\\frac{1}{2}$$\n```\n\n价格 \\$5 和 $10，保持原样。");
    scratch.insertMarkdownText(codeMath);
    check(!scratch.toHtml().contains("nocturne-math:"), "code and escaped currency remain literal");
    scratch.clear();
    scratch.insertMarkdownText(QStringLiteral("\\(x_i\\) 与 \\[\\frac{1}{2}\\]"));
    check(scratch.toHtml().count("nocturne-math:") == 2, "backslash delimiters work in Markdown");
    const QString webMath = QStringLiteral("<p>公式 <span class=\"katex\"><span class=\"katex-mathml\"><math><semantics><mi>x</mi><annotation encoding=\"application/x-tex\">x_i=\\frac{1}{2}</annotation></semantics></math></span><span class=\"katex-html\">DUPLICATE</span></span> 结束</p>");
    scratch.setHtml(webMath);
    check(scratch.toHtml().count("nocturne-math:") == 1 && !scratch.toPlainText().contains("DUPLICATE")
        && scratch.toPlainText().contains("\\frac{1}{2}"), "browser math uses TeX annotation exactly once");
    scratch.selectAll(); scratch.copy();
    check(QApplication::clipboard()->text().contains("\\frac{1}{2}"), "copy retains formula source");
    scratch.setHtml("<p>value $x_i$ end</p>");
    check(scratch.toHtml().contains("nocturne-math:"), "existing notes with delimited formulas render on open");
    scratch.clear(); scratch.insertFormula(QStringLiteral("\\frac{1}{2}"), false);
    scratch.undo(); check(scratch.toPlainText().isEmpty(), "formula insertion undoes in one step");
    scratch.redo(); check(scratch.toPlainText().contains("\\frac{1}{2}"), "formula insertion redoes with source");
    scratch.clear(); scratch.insertFormula("x^2", true);
    QTextCursor formulaSelection(scratch.document()); formulaSelection.setPosition(0); formulaSelection.setPosition(1, QTextCursor::KeepAnchor);
    scratch.setTextCursor(formulaSelection); scratch.applyAlignment(Qt::AlignRight);
    const int mathBlocks = scratch.document()->blockCount();
    scratch.insertFormula("y^2", true);
    check(scratch.document()->blockCount() == mathBlocks, "editing a formula does not insert extra blank paragraphs");
    reopenedMath.setHtml(scratch.toHtml());
    check(reopenedMath.document()->begin().blockFormat().alignment().testFlag(Qt::AlignRight), "reloading formulas preserves authored paragraph alignment");
    {
    NoteEditor scratch;
    scratch.setPlainText(QStringLiteral("说明与公式：\\alpha_i + \\frac{1}{2}。\n\\varphi_i=\\frac{\\pi\\theta_i}{180}\n\\begin{equation}E=mc^2\\end{equation}\n\\begin{align*}x&=1\\\\y&=2\\end{align*}\n$z_i$\n结尾正文"));
    QTextCursor bulkLayout(scratch.document()); bulkLayout.select(QTextCursor::Document);
    QTextBlockFormat rightAligned; rightAligned.setAlignment(Qt::AlignRight); bulkLayout.mergeBlockFormat(rightAligned);
    scratch.document()->clearUndoRedoStacks();
    const QString beforeBulkMath = scratch.toHtml();
    const auto bulk = MathSupport::convertAllMath(*scratch.document());
    if (bulk.converted != 5 || bulk.skipped) std::cerr << "BULK MATH converted=" << bulk.converted << " skipped=" << bulk.skipped << " errors=" << bulk.errors.join("; ").toStdString() << '\n';
    check(bulk.converted == 5 && bulk.skipped == 0 && scratch.toHtml().count("nocturne-math:") == 5,
        "whole note converts inline bare TeX, bare equations, environments and delimited math");
    check(scratch.toPlainText().contains(QStringLiteral("说明与公式：")) && scratch.toPlainText().endsWith(QStringLiteral("结尾正文"))
            && scratch.document()->begin().blockFormat().alignment().testFlag(Qt::AlignRight),
        "bulk math preserves surrounding prose and authored alignment");
    const QString afterBulkMath = scratch.toHtml();
    scratch.undo(); check(scratch.toHtml() == beforeBulkMath, "whole-note conversion has one exact undo step");
    scratch.redo(); check(scratch.toHtml() == afterBulkMath, "whole-note conversion redoes exactly");
    const auto repeatedMath = MathSupport::convertAllMath(*scratch.document());
    check(repeatedMath.converted == 0 && repeatedMath.alreadyFormatted == 5 && scratch.toHtml() == afterBulkMath,
        "bulk math is idempotent for existing formula objects");
    const QString protectedMath = QStringLiteral("`\\alpha_i`\n  ~~~tex\n\\frac{1}{2}\n  ~~~\n```cpp\n$skip$\n```\n    \\theta\n价格 $5 与 $10，路径 C:\\theta\\notes.md\n[文档](https://example.test/\\theta)\n普通标题与文件 x_i.md\n不完整 \\frac{1}{\n$\\unknownNocturneCommand{x}$");
    scratch.clear(); scratch.setCurrentCharFormat(QTextCharFormat());
    scratch.setPlainText(protectedMath); scratch.document()->clearUndoRedoStacks();
    const auto rejectedMath = MathSupport::convertAllMath(*scratch.document());
    check(rejectedMath.converted == 0 && scratch.toPlainText() == protectedMath && !scratch.document()->isUndoAvailable(),
        "bulk math preserves code, currency, paths, prose and invalid formulas without empty undo entries");
    scratch.setHtml("<p><code>\\alpha_i</code></p><p>\\beta_i</p>");
    const auto richCodeConversion = MathSupport::convertAllMath(*scratch.document());
    if (richCodeConversion.converted != 1) std::cerr << "CODE MATH count=" << richCodeConversion.converted << " html=" << scratch.toHtml().toStdString() << '\n';
    check(richCodeConversion.converted == 1,
        "rich-text code formatting is excluded from bulk conversion");
    NoteEditor reloadedCode; reloadedCode.setHtml(scratch.toHtml());
    check(MathSupport::convertAllMath(*reloadedCode.document()).converted == 0,
        "HTML code exclusion survives saving and reopening");
    scratch.setHtml("<p><a href=\"nocturne-todo:bulk-fixture\">\\gamma_i</a> 与 <img src=\"unrelated-image.png\" /></p>");
    check(MathSupport::convertAllMath(*scratch.document()).converted == 1
            && scratch.toHtml().contains("unrelated-image.png") && scratch.locateTodo("bulk-fixture"),
        "bulk conversion preserves todo anchors and unrelated images");
    scratch.clear(); scratch.setCurrentCharFormat(QTextCharFormat());
    scratch.setPlainText("Let \\alpha_i denote the angle; then \\beta=2.");
    check(MathSupport::convertAllMath(*scratch.document()).converted == 2
            && scratch.toPlainText().contains("Let ") && scratch.toPlainText().contains(" denote the angle; then "),
        "bare inline math excludes surrounding English prose");
    QTextDocument sourceMath;
    const QString sourceBefore = "---\ntitle: keep\n---\n\n\\frac{1}{2}\n\n$z_i$\n\n![keep](img/a.png)\n";
    sourceMath.setPlainText(sourceBefore); sourceMath.clearUndoRedoStacks();
    const auto normalizedMath = MathSupport::convertAllMath(sourceMath, MathSupport::ConversionTarget::MarkdownSource);
    check(normalizedMath.converted == 1 && normalizedMath.alreadyFormatted == 1
            && sourceMath.toPlainText() == "---\ntitle: keep\n---\n\n$$\n\\frac{1}{2}\n$$\n\n$z_i$\n\n![keep](img/a.png)\n",
        "source conversion inserts only math delimiters and preserves all other Markdown bytes");
    check(MathSupport::convertAllMath(sourceMath, MathSupport::ConversionTarget::MarkdownSource).converted == 0,
        "source conversion does not duplicate delimiters");
    sourceMath.undo(); check(sourceMath.toPlainText() == sourceBefore, "source delimiter changes undo together");
    sourceMath.setPlainText(QStringLiteral("价格 $5 与 $10，角度 \\theta_i。共享路径 \\\\server\\theta\\notes.md"));
    const auto mixedCurrency = MathSupport::convertAllMath(sourceMath, MathSupport::ConversionTarget::MarkdownSource);
    check(mixedCurrency.converted == 1 && sourceMath.toPlainText().contains(QStringLiteral("$5 与 $10"))
            && sourceMath.toPlainText().endsWith("\\\\server\\theta\\notes.md"), "currency does not hide later math and network paths stay literal");
    sourceMath.setPlainText("\\theta_i\n\ntext");
    MathSupport::convertAllMath(sourceMath, MathSupport::ConversionTarget::MarkdownSource);
    check(sourceMath.toPlainText().startsWith("$$\n\\theta_i\n$$"), "a standalone formula on the first line is a display block");
    sourceMath.setPlainText("$\\theta_i\n\n\\alpha_i");
    const auto unclosedMath = MathSupport::convertAllMath(sourceMath, MathSupport::ConversionTarget::MarkdownSource);
    check(unclosedMath.skipped == 1 && unclosedMath.converted == 1 && !unclosedMath.errors.isEmpty()
            && sourceMath.toPlainText().startsWith("$\\theta_i\n"), "unclosed delimiters are reported and do not hide later complete formulas");
    }
    scratch.setHtml("<h1>一级标题</h1><p>正文</p><h3>跳级标题</h3><h2>同名标题</h2><h2>同名标题</h2>");
    DocumentOutline outline(&scratch); outline.resize(220, 400); outline.show(); outline.rebuild(true);
    auto* headingTree = outline.findChild<QTreeWidget*>("headingTree");
    check(outline.headingCount() == 4 && headingTree->topLevelItemCount() == 1
        && headingTree->topLevelItem(0)->childCount() == 3, "outline nests skipped and repeated heading levels correctly");
    auto* repeated = headingTree->topLevelItem(0)->child(2);
    headingTree->itemClicked(repeated, 0);
    check(scratch.textCursor().block().blockNumber() == 4, "outline jumps to the exact repeated heading");
    auto cursorBeforeHeadings = QTextCursor(scratch.document()); cursorBeforeHeadings.insertText("新增 ");
    headingTree->itemClicked(repeated, 0);
    check(scratch.textCursor().block().text() == QStringLiteral("同名标题"), "outline anchors track edits before headings");
    headingTree->topLevelItem(0)->setExpanded(false); outline.rebuild();
    check(!headingTree->topLevelItem(0)->isExpanded(), "outline rebuild preserves collapsed sections");
    scratch.setPlainText("new note"); outline.rebuild(true);
    check(outline.headingCount() == 0 && headingTree->topLevelItemCount() == 0, "new note clears old outline");
    outline.hide();
    FolderComboBox choices;
    choices.setFolders(database.listFolders(), true); choices.show(); choices.showPopup(); settle(30);
    auto* choiceTree = choices.findChild<QTreeWidget*>("folderChoiceTree");
    QTreeWidgetItem* rootChoice = nullptr;
    for (int i = 0; i < choiceTree->topLevelItemCount(); ++i)
        if (choiceTree->topLevelItem(i)->data(0, Qt::UserRole).toLongLong() == root) rootChoice = choiceTree->topLevelItem(i);
    check(rootChoice && !rootChoice->isExpanded() && rootChoice->childCount() >= 2, "folder chooser starts at collapsed first-level folders");
    if (rootChoice) {
        const QPoint arrow = choiceTree->visualItemRect(rootChoice).topLeft() + QPoint(-10, 16);
        QMouseEvent press(QEvent::MouseButtonPress, arrow, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QMouseEvent release(QEvent::MouseButtonRelease, arrow, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(choiceTree->viewport(), &press); QApplication::sendEvent(choiceTree->viewport(), &release);
        check(rootChoice->isExpanded() && choices.findChild<QFrame*>("folderTreePopup")->isVisible()
            && choices.currentData().toLongLong() == Database::AllFolders, "folder arrow expands without selecting or closing");
        auto* childChoice = rootChoice->child(0); choiceTree->setCurrentItem(childChoice); choiceTree->itemClicked(childChoice, 0);
        check(choices.currentData().toLongLong() == childChoice->data(0, Qt::UserRole).toLongLong(), "folder selection returns the exact child id");
        choices.showPopup(); check(!rootChoice->isExpanded(), "folder popup resets expansion on reopening"); choices.hidePopup();
    }
    choices.hide();
    QSettings().remove("navigation/expandedFoldersV2");
    NotebookTree compactTree; compactTree.rebuildFolders(database.listFolders());
    compactTree.addNote("fixture", childA); compactTree.finishRebuild(false); compactTree.setCurrentRow(0, false);
    bool closed = true;
    for (int i = 0; i < compactTree.topLevelItemCount(); ++i) closed &= !compactTree.topLevelItem(i)->isExpanded();
    check(closed, "restoring the current note does not expand top-level navigation folders");
    compactTree.rebuildFolders(database.listFolders()); compactTree.addNote("fixture", childA); compactTree.finishRebuild(true);
    compactTree.rebuildFolders(database.listFolders()); compactTree.addNote("fixture", childA); compactTree.finishRebuild(false);
    closed = true;
    for (int i = 0; i < compactTree.topLevelItemCount(); ++i) closed &= !compactTree.topLevelItem(i)->isExpanded();
    check(closed, "temporary search expansion does not overwrite normal navigation state");
    scratch.clear();
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
    auto* imageTools = window.findChild<QToolButton*>(QStringLiteral("imageToolsButton"));
    auto* alignLeft = window.findChild<QToolButton*>(QStringLiteral("alignLeftButton"));
    auto* alignCenter = window.findChild<QToolButton*>(QStringLiteral("alignCenterButton"));
    auto* alignRight = window.findChild<QToolButton*>(QStringLiteral("alignRightButton"));
    check(imageTools && imageTools->menu() && imageTools->menu()->actions().size() >= 6,
        "image scaling and image alignment menu is present");
    check(alignLeft && alignCenter && alignRight,
        "top toolbar exposes left, center, and right text alignment actions");

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
        QTemporaryDir mediaSource;
        QImage panorama(2000, 240, QImage::Format_RGB32); panorama.fill(Qt::darkCyan);
        const QString panoramaPath = mediaSource.path() + QStringLiteral("/panorama.png");
        panorama.save(panoramaPath);
        const QString panoramaUrl = QUrl::fromLocalFile(panoramaPath).toString();
        media.document()->addResource(QTextDocument::ImageResource, QUrl(panoramaUrl), panorama);
        media.setHtml(QStringLiteral("<p style='line-height:160%'><img src='%1' width='2000' height='900'></p><p>After image</p>").arg(panoramaUrl));
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
        check(!QApplication::activeModalWidget(), "double click closes without a delayed caption editor");
        imageMouse(QEvent::MouseButtonPress, Qt::LeftButton);
        imageMouse(QEvent::MouseButtonRelease, Qt::NoButton);
        settle(QApplication::doubleClickInterval() + 180);
        check(media.hasImageAtCursor() && !QApplication::activeModalWidget(), "single click selects the image without opening a dialog");
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
        media.setHtml(QStringLiteral("<p><img src='%1' width='2000' height='240'></p><p>正文段落</p>").arg(panoramaUrl));
        QTextCursor selectedImage(media.document()); selectedImage.setPosition(0);
        selectedImage.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
        media.setTextCursor(selectedImage);
        const qreal fitWidth = media.intrinsicSize(media.document(), 0, selectedImage.charFormat()).width();
        check(media.hasImageAtCursor(), "image selection is available to layout controls");
        check(media.setCurrentImageScale(150), "image scale control accepts a larger display size");
        auto scaledImage = media.textCursor().charFormat().toImageFormat();
        check(scaledImage.width() > fitWidth && qAbs(scaledImage.height() / scaledImage.width() - 0.12) < 0.001,
              "image scaling changes width while preserving source ratio");
        check(media.toHtml().contains(QStringLiteral("nocturne-size")), "manual image size is persisted in HTML");
        const auto resizedHtml = media.toHtml();
        media.setHtml(resizedHtml);
        QTextCursor reloadedImage(media.document()); reloadedImage.setPosition(0);
        reloadedImage.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
        media.setTextCursor(reloadedImage);
        const qreal reloadedWidth = media.intrinsicSize(media.document(), 0, reloadedImage.charFormat()).width();
        check(qAbs(reloadedWidth - scaledImage.width()) <= 0.1,
              "image display size survives HTML reload");
        check(media.setCurrentImageScale(50), "image scale control accepts a smaller display size");
        const qreal smallerWidth = media.textCursor().charFormat().toImageFormat().width();
        check(smallerWidth < scaledImage.width(), "image can be reduced without changing the attachment");
        media.undo();
        check(media.textCursor().charFormat().toImageFormat().width() == scaledImage.width(), "image size change is undoable");
        media.setCurrentImageScale(80);
        media.setTextCursor(reloadedImage);
        const qreal dragStartWidth = media.textCursor().charFormat().toImageFormat().width();
        const QRectF imageRect = media.currentImageRect();
        const QPoint dragFrom(qRound(imageRect.right()) - 3, qRound(imageRect.center().y()));
        const QPoint dragTo = dragFrom + QPoint(36, 0);
        QMouseEvent resizePress(QEvent::MouseButtonPress, dragFrom, dragFrom, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QMouseEvent resizeMove(QEvent::MouseMove, dragTo, dragTo, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QMouseEvent resizeRelease(QEvent::MouseButtonRelease, dragTo, dragTo, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(media.viewport(), &resizePress);
        QApplication::sendEvent(media.viewport(), &resizeMove);
        QApplication::sendEvent(media.viewport(), &resizeRelease);
        check(media.textCursor().charFormat().toImageFormat().width() > dragStartWidth, "dragging image edge changes display width");
        media.applyAlignment(Qt::AlignRight);
        check(media.document()->firstBlock().blockFormat().alignment().testFlag(Qt::AlignRight), "image paragraph can align right");
        media.applyAlignment(Qt::AlignHCenter);
        check(media.document()->firstBlock().blockFormat().alignment().testFlag(Qt::AlignHCenter), "image paragraph can align center");
        media.setPlainText(QStringLiteral("第一段\n第二段\n第三段"));
        media.selectAll(); media.applyAlignment(Qt::AlignHCenter);
        bool allCentered = true;
        for (auto block = media.document()->begin(); block.isValid(); block = block.next())
            allCentered &= block.blockFormat().alignment().testFlag(Qt::AlignHCenter);
        check(allCentered, "multi-paragraph text alignment applies to every selected paragraph");
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
    {
        QTemporaryDir linkedRoot;
        const QString sourcePath = linkedRoot.path() + QStringLiteral("/linked.md");
        QFile source(sourcePath); source.open(QIODevice::WriteOnly); source.write("# Linked\n\nfirst"); source.close();
        QString linkedHtml, linkedPlain, linkError; QByteArray sourceBytes;
        check(DocumentImporter::readDocument(sourcePath, &linkedHtml, &linkedPlain, &sourceBytes, &linkError), "linked Markdown source can be parsed without copying it");
        bool skipped = false;
        const qint64 linkedId = database.linkNote(QFileInfo(sourcePath).canonicalFilePath(),
            QCryptographicHash::hash(sourceBytes, QCryptographicHash::Sha256), "md", "linked",
            linkedHtml, linkedPlain, Database::UnfiledFolder, &skipped, &linkError);
        check(linkedId > 0 && !skipped && database.linkedSource(linkedId).has_value(), "linked source stores a path relation");
        const auto linked = database.linkedSource(linkedId);
        check(linked && linked->sourcePath == QFileInfo(sourcePath).canonicalFilePath(), "linked source path remains canonical");
        source.open(QIODevice::WriteOnly); source.resize(0); source.write("# Linked\n\nupdated"); source.close();
        check(DocumentImporter::readDocument(sourcePath, &linkedHtml, &linkedPlain, &sourceBytes, &linkError), "changed linked source can be reread");
        skipped = false;
        const qint64 updatedId = database.linkNote(QFileInfo(sourcePath).canonicalFilePath(),
            QCryptographicHash::hash(sourceBytes, QCryptographicHash::Sha256), "md", "linked",
            linkedHtml, linkedPlain, Database::UnfiledFolder, &skipped, &linkError);
        check(updatedId == linkedId && !skipped && database.note(linkedId)->plainText.contains("updated"), "rescan updates the same linked note");
    }
    {
        const QString demo = QStringLiteral("# 测向约束的几何表示\n\n设第 $i$ 个检测点为 $S_i=(x_i,y_i)$，将角度换算为弧度：\n\n$$\n\\varphi_i=\\frac{\\pi\\theta_i}{180}\n$$\n\n## 边界向量\n\n$$\nv_i=\\begin{pmatrix}\\cos(\\varphi_i-\\alpha)\\\\\\sin(\\varphi_i-\\alpha)\\end{pmatrix}\n$$\n\n### 误差与范围\n\n允许误差 $\\alpha=\\frac{\\pi}{180}$，范围为 $[\\varphi_i-\\alpha,\\varphi_i+\\alpha]$。\n\n## 汇总\n\n$$\nE=\\sum_{i=1}^{n}\\sqrt{x_i^2+y_i^2}\n$$");
        NoteEditor sample; sample.insertMarkdownText(demo);
        const auto sampleId = database.createNote(QStringLiteral("公式与大纲回归"), sample.toHtml(), sample.toPlainText(), &error);
        check(sampleId > 0, "math example saves in isolated test database");
        window.findChild<QComboBox*>(QStringLiteral("folderFilter"))->setCurrentIndex(0);
        window.findChild<QLineEdit*>(QStringLiteral("searchEdit"))->setText(QStringLiteral("公式与大纲回归")); settle();
        auto* body = window.findChild<NoteEditor*>(QStringLiteral("noteEditor"));
        auto* toggle = window.findChild<QToolButton*>(QStringLiteral("outlineButton"));
        toggle->click(); settle(200);
        auto* panel = window.findChild<DocumentOutline*>(QStringLiteral("documentOutline"));
        check(panel->isVisible() && panel->headingCount() == 4, "opening a note connects its heading outline");
        check(body->markdownForExport().contains("\\varphi_i"), "loaded note retains LaTeX source");
        window.resize(1500, 960); settle(100);
        check(window.grab().save(QDir(outputDirectory).filePath(QStringLiteral("Nocturne-math-outline.png"))), "math and outline screenshot saves");
        auto* folder = static_cast<FolderComboBox*>(window.findChild<QComboBox*>(QStringLiteral("noteFolderCombo")));
        folder->showPopup(); settle(50);
        check(folder->findChild<QFrame*>(QStringLiteral("folderTreePopup"))->grab().save(QDir(outputDirectory).filePath(QStringLiteral("Nocturne-folder-tree.png"))), "folder chooser screenshot saves");
        folder->hidePopup(); toggle->click();
    }
    std::cout << (ok ? "PASS" : "FAIL") << ": document workflows (Markdown, task anchors, folders, imports)\n";
    return ok;
}
