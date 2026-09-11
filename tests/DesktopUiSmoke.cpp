#include "Database.h"
#include "GlobalHotkey.h"
#include "MainWindow.h"
#include "StickyNoteWindow.h"
#include "NoteEditor.h"
#include "NotebookTree.h"
#include "NocturneStyle.h"
#include "NocturneDialogs.h"

#include <QApplication>
#include <QAction>
#include <QDir>
#include <QDialog>
#include <QDialogButtonBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFontInfo>
#include <QMouseEvent>
#include <QStackedWidget>
#include <QTextBlock>
#include <QTextLayout>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QLineEdit>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QStandardPaths>
#include <QSystemTrayIcon>
#include <QTextEdit>
#include <QTimer>
#include <QToolButton>
#include <QUuid>
#include <QWidget>

#include <cmath>
#include <iostream>
#include <memory>

bool runDocumentWorkflowTests(Database& database, MainWindow& window, const QString& outputDirectory);
bool runWorkspaceUiTests(Database& database, MainWindow& window, const QString& outputDirectory);

namespace {

bool check(bool condition, const char* message)
{
    if (!condition)
        std::cerr << "FAIL: " << message << '\n';
    return condition;
}

void pumpEvents()
{
    QApplication::processEvents(QEventLoop::AllEvents, 120);
}

void settle(int milliseconds)
{
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}

} // namespace

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QStandardPaths::setTestModeEnabled(true);
    QApplication::setOrganizationName(QStringLiteral("FeatherNoteTests"));
    QApplication::setApplicationName(
        QStringLiteral("DesktopUi-%1")
            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    QApplication::setQuitOnLastWindowClosed(false);

    QSettings shortcutSettings;
    shortcutSettings.setValue(
        GlobalHotkey::settingsKey(),
        GlobalHotkey::portableText(GlobalHotkey::defaultSequence()));
    shortcutSettings.sync();

    const QString outputDirectory = argc > 1
        ? QDir::cleanPath(QString::fromLocal8Bit(argv[1]))
        : QDir::current().filePath(QStringLiteral("ui-smoke"));
    bool ok = check(QDir().mkpath(outputDirectory), "UI screenshot directory creates");
    QString validationError;
    ok &= check(!GlobalHotkey::validate(
                    QKeySequence(QStringLiteral("N"), QKeySequence::PortableText),
                    &validationError),
                "global hotkey rejects an unmodified letter");
    ok &= check(GlobalHotkey::validate(
                    QKeySequence(QStringLiteral("Ctrl+Alt+F12"),
                                 QKeySequence::PortableText),
                    &validationError),
                "global hotkey accepts a supported single chord");

    auto database = std::make_unique<Database>();
    QString error;
    ok &= check(database->open(&error), "UI smoke database opens");
    if (!ok) {
        std::cerr << error.toStdString() << '\n';
        return 1;
    }
    const QString dataDirectory = database->dataDirectory();

    // Editorial fixtures live only in the UUID-scoped test database.
    const qint64 journalFolder = database->createFolder(QStringLiteral("随手记"), &error);
    const qint64 readingFolder = database->createFolder(QStringLiteral("阅读笔记"), &error);
    const QStringList titles = {QStringLiteral("留给明天的自己"), QStringLiteral("窗边，忽然想到"),
        QStringLiteral("九月的阅读航线"), QStringLiteral("潮汐之间")};
    const QStringList excerpts = {QStringLiteral("写一封短信，鼓励未来的我们。"),
        QStringLiteral("一些细小的瞬间，拼成了生活。"), QStringLiteral("记录这个月值得停靠的书与句子。"),
        QStringLiteral("潮起潮落，思绪也有自己的节奏。")};
    for (int i = 0; i < titles.size(); ++i)
        database->createNote(titles.at(i), QStringLiteral("<p>%1</p>").arg(excerpts.at(i)), excerpts.at(i),
            &error, i == 2 ? readingFolder : journalFolder);
    const QString demoHtml = QStringLiteral(
        "<p>白天来不及停留的念头，就在此刻慢慢展开。</p>"
        "<p>一段读到的文字，一个尚未成形的计划，<br>或是窗外的灯，和忽然安静下来的自己。</p>"
        "<h2>此刻，值得记下</h2>"
        "<ul><li>给新故事想一个开头</li><li>把今天的零碎灵感收进同一页</li>"
        "<li>为明天，留下一盏小小的灯</li></ul>"
        "<p>不必急着抵达。写下来，就是一次出发。</p>");
    QTextDocument demoDocument;
    demoDocument.setHtml(demoHtml);
    const qint64 demoId = database->createNote(QStringLiteral("今夜，让灵感靠岸"), demoHtml,
        demoDocument.toPlainText(), &error, journalFolder);
    database->createTodo(QStringLiteral("阅读 20 页"), &error);
    database->createTodo(QStringLiteral("整理散落的便签"), &error);
    const qint64 doneTodo = database->createTodo(QStringLiteral("记下今天的一件小事"), &error);
    database->updateTodoDone(doneTodo, true, &error);

    auto mainWindow = std::make_unique<MainWindow>(database.get());
    mainWindow->setProperty("suppressTrayNotifications", true);
    mainWindow->resize(1440, 900);
    mainWindow->show();
    pumpEvents();

    auto capture = [&](const QString& name, QWidget* widget) {
        pumpEvents();
        ok &= check(widget->grab().save(QDir(outputDirectory).filePath(name)), "redesign screenshot saves");
    };
    auto* editor = mainWindow->findChild<NoteEditor*>(QStringLiteral("noteEditor"));
    auto* noteList = mainWindow->findChild<NotebookTree*>(QStringLiteral("noteList"));
    auto* search = mainWindow->findChild<QLineEdit*>(QStringLiteral("searchEdit"));
    auto* focus = mainWindow->findChild<QToolButton*>(QStringLiteral("focusButton"));
    auto* todoToggle = mainWindow->findChild<QToolButton*>(QStringLiteral("todoToggleButton"));
    auto* navigation = mainWindow->findChild<QWidget*>(QStringLiteral("navigation"));
    auto* todoPane = mainWindow->findChild<QWidget*>(QStringLiteral("todoPane"));
    auto* formatBar = mainWindow->findChild<QWidget*>(QStringLiteral("formatBar"));
    ok &= check(editor && noteList && search && focus && todoToggle && navigation && todoPane && formatBar,
                "all redesign workflow controls exist");
    if (!ok) return 1;
    std::cout << "UI_FONT sans=" << NocturneUi::sansFamily().toStdString()
              << " serif=" << NocturneUi::serifFamily().toStdString()
              << " resolved=" << QFontInfo(editor->font()).family().toStdString()
              << " dpi=" << mainWindow->devicePixelRatioF() << '\n';
    capture(QStringLiteral("Nocturne-redesign-night.png"), mainWindow.get());

    // Theme changes must preserve authored HTML, selection, undo, and DB revision.
    QTextCursor cursor = editor->textCursor();
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(QStringLiteral("主题切换验证"));
    cursor.movePosition(QTextCursor::PreviousCharacter, QTextCursor::KeepAnchor, 6);
    editor->setTextCursor(cursor);
    const QString authoredHtml = editor->toHtml();
    const QString selectedText = cursor.selectedText();
    for (const QString& id : {QStringLiteral("harbor"), QStringLiteral("moonlight"), QStringLiteral("night")}) {
        auto* themeAction = mainWindow->findChild<QAction*>(QStringLiteral("theme_%1").arg(id));
        ok &= check(themeAction != nullptr, "theme action exists");
        if (themeAction) themeAction->trigger();
        pumpEvents();
        ok &= check(editor->toHtml() == authoredHtml && editor->textCursor().selectedText() == selectedText
                    && editor->document()->isUndoAvailable(), "theme preserves rich text, selection and undo");
        ok &= check(QSettings().value(QStringLiteral("appearance/theme")).toString() == id,
                    "theme choice persists");
    }
    editor->undo();
    settle(750);
    ok &= check(database->note(demoId)->plainText == demoDocument.toPlainText(),
                "autosave persists undo after theme switching");

    // Explicit legacy black text remains authored black, with readable view overlays.
    const QString beforeContrast = editor->toHtml();
    editor->setHtml(QStringLiteral("<p><span style='color:#111111;'>旧笔记黑字</span></p>"));
    pumpEvents();
    bool corrected = false;
    for (const auto& range : editor->document()->firstBlock().layout()->formats())
        corrected |= range.format.foreground().color() == NocturneUi::theme().text;
    ok &= check(corrected && editor->toHtml().contains(QStringLiteral("#111111")),
                "legacy colors are readable without rewriting their saved color");
    editor->setHtml(beforeContrast);
    settle(750);
    for (const QString& id : {QStringLiteral("harbor"), QStringLiteral("moonlight"), QStringLiteral("night")}) {
        mainWindow->findChild<QAction*>(QStringLiteral("theme_%1").arg(id))->trigger();
        capture(QStringLiteral("Nocturne-redesign-%1.png").arg(id), mainWindow.get());
    }
    const QString savedBeforeFocus = editor->toHtml();
    focus->click(); pumpEvents();
    ok &= check(!navigation->isVisible() && !todoPane->isVisible() && !formatBar->isVisible()
                && editor->isVisible(), "focus mode expands the writing surface");
    capture(QStringLiteral("Nocturne-redesign-focus.png"), mainWindow.get());
    focus->click(); pumpEvents();
    ok &= check(navigation->isVisible() && todoPane->isVisible() && editor->toHtml() == savedBeforeFocus,
                "leaving focus restores panels without content changes");
    mainWindow->resize(960, 680); pumpEvents();
    ok &= check(navigation->isVisible() && !todoPane->isVisible(), "narrow window automatically frees editor space");
    ok &= check(formatBar->childrenRect().right() <= formatBar->width(), "format tools fit narrow editor");
    capture(QStringLiteral("Nocturne-redesign-compact.png"), mainWindow.get());
    todoToggle->click(); pumpEvents();
    ok &= check(todoPane->isVisible() && !navigation->isVisible(), "narrow todo request swaps the side panel");
    todoToggle->click();
    mainWindow->resize(1440, 900);
    todoToggle->click(); pumpEvents();
    search->setText(QStringLiteral("不存在的航迹-7829")); settle(250);
    ok &= check(noteList->count() == 0 && mainWindow->findChild<QStackedWidget*>(
        QStringLiteral("documentStack"))->currentIndex() == 1, "empty search displays an actionable empty state");
    capture(QStringLiteral("Nocturne-redesign-empty.png"), mainWindow.get());
    search->clear(); settle(250);
    ok &= check(noteList->count() == 5 && editor->isVisible(), "clearing search restores all notes");

    // New note + rich text + folder change run through actual UI controls.
    mainWindow->findChild<QPushButton*>(QStringLiteral("primaryButton"))->click();
    mainWindow->findChild<QLineEdit*>(QStringLiteral("titleEdit"))->setText(QStringLiteral("交互回归笔记"));
    editor->insertPlainText(QStringLiteral("保存到本机的真实文字"));
    const QString enteredText = editor->toPlainText();
    settle(750);
    auto* folderCombo = mainWindow->findChild<QComboBox*>(QStringLiteral("noteFolderCombo"));
    folderCombo->setCurrentIndex(folderCombo->findData(readingFolder)); pumpEvents();
    const auto savedNotes = database->listNoteSummaries(QStringLiteral("交互回归笔记"), &error);
    ok &= check(savedNotes.size() == 1 && savedNotes.first().folderId == readingFolder
        && database->note(savedNotes.first().id)->plainText == enteredText,
        "create, edit, autosave and move-to-folder work through the UI");
    auto* todoInput = mainWindow->findChild<QLineEdit*>(QStringLiteral("todoInput"));
    auto* todoList = mainWindow->findChild<QListWidget*>(QStringLiteral("todoList"));
    todoInput->setText(QStringLiteral("通过界面添加的待办"));
    mainWindow->findChild<QPushButton*>(QStringLiteral("roundButton"))->click(); pumpEvents();
    QListWidgetItem* addedTodo = nullptr;
    for (int i = 0; i < todoList->count(); ++i)
        if (todoList->item(i)->text() == QStringLiteral("通过界面添加的待办")) addedTodo = todoList->item(i);
    ok &= check(addedTodo, "todo input creates a real row");
    if (addedTodo) {
        todoList->scrollToItem(addedTodo);
        const QRect rowRect = todoList->visualItemRect(addedTodo);
        const QPoint point(rowRect.left() + 12, rowRect.center().y());
        QMouseEvent press(QEvent::MouseButtonPress, point, todoList->viewport()->mapToGlobal(point),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(todoList->viewport(), &press);
        QMouseEvent release(QEvent::MouseButtonRelease, point, todoList->viewport()->mapToGlobal(point),
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(todoList->viewport(), &release); pumpEvents();
        bool done = false;
        for (const auto& todo : database->listTodos())
            if (todo.text == QStringLiteral("通过界面添加的待办")) done = todo.done;
        ok &= check(done, "clicking custom todo delegate persists completion");
    }

    // App-owned dialogs: close means cancel; input, color, file selection are themed.
    bool messageInspected = false;
    QTimer::singleShot(60, &app, [&] {
        auto* dialog = mainWindow->findChild<QDialog*>(QStringLiteral("nocturneMessageDialog"));
        if (!dialog) return;
        messageInspected = dialog->windowFlags().testFlag(Qt::FramelessWindowHint);
        capture(QStringLiteral("Nocturne-redesign-confirmation.png"), dialog);
        dialog->findChild<QToolButton*>(QStringLiteral("dialogCloseButton"))->click();
    });
    const auto cancelled = NocturneDialogs::question(mainWindow.get(), QStringLiteral("移到回收站"),
        QStringLiteral("将“潮汐之间”移到回收站？这篇笔记会从当前列表中移除。"));
    ok &= check(messageInspected && cancelled == QMessageBox::Cancel, "closing original confirmation never confirms deletion");
    QTimer::singleShot(60, &app, [&] {
        auto* dialog = mainWindow->findChild<QDialog*>(QStringLiteral("nocturneInputDialog"));
        if (!dialog) return;
        dialog->findChild<QLineEdit*>(QStringLiteral("dialogInput"))->setText(QStringLiteral("夜间手记"));
        capture(QStringLiteral("Nocturne-redesign-input.png"), dialog);
        dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
    });
    bool inputAccepted = false;
    ok &= check(NocturneDialogs::getText(mainWindow.get(), QStringLiteral("新建分组"), QStringLiteral("分组名称"),
        QLineEdit::Normal, QString(), &inputAccepted) == QStringLiteral("夜间手记") && inputAccepted,
        "original input dialog accepts a real value");
    bool invalidColorRejected = false;
    QTimer::singleShot(60, &app, [&] {
        auto* dialog = mainWindow->findChild<QDialog*>(QStringLiteral("nocturneColorDialog"));
        if (!dialog) return;
        auto* input = dialog->findChild<QLineEdit*>(QStringLiteral("colorHexInput"));
        auto* accept = dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok);
        input->setText(QStringLiteral("not-a-color"));
        invalidColorRejected = !accept->isEnabled();
        input->setText(QStringLiteral("#D8BD88"));
        capture(QStringLiteral("Nocturne-redesign-color.png"), dialog);
        accept->click();
    });
    const QColor picked = NocturneDialogs::getColor(Qt::white, mainWindow.get(), QStringLiteral("文字颜色"));
    ok &= check(invalidColorRejected && picked == QColor("#D8BD88"), "original palette validates and applies HEX colors");
    bool fileInspected = false;
    QTimer::singleShot(100, &app, [&] {
        auto* dialog = mainWindow->findChild<QFileDialog*>(QStringLiteral("nocturneFileDialog"));
        if (!dialog) return;
        fileInspected = dialog->testOption(QFileDialog::DontUseNativeDialog)
            && dialog->windowFlags().testFlag(Qt::FramelessWindowHint);
        capture(QStringLiteral("Nocturne-redesign-file.png"), dialog);
        dialog->reject();
    });
    NocturneDialogs::getOpenFileName(mainWindow.get(), QStringLiteral("导入文档"), dataDirectory,
        QStringLiteral("文档 (*.txt *.md *.html)"));
    ok &= check(fileInspected, "file picker uses app-owned chrome and no native Windows dialog");

    ok &= check(mainWindow->findChild<QAction*>(
                    QStringLiteral("manualBackupAction")) != nullptr,
                "manual backup action is available in the file menu");
    ok &= check(mainWindow->findChild<QAction*>(
                    QStringLiteral("openBackupDirectoryAction")) != nullptr,
                "backup recovery directory action is available in the file menu");
    QAction* collectAction = mainWindow->findChild<QAction*>(
        QStringLiteral("collectStickiesAction"));
    ok &= check(collectAction != nullptr,
                "collect-stickies action is available in the manage menu");
    QAction* hotkeyAction = mainWindow->findChild<QAction*>(
        QStringLiteral("hotkeySettingsAction"));
    ok &= check(hotkeyAction != nullptr,
                "global hotkey settings action is available in the manage menu");

    auto* stickyButton = mainWindow->findChild<QPushButton*>(
        QStringLiteral("secondaryButton"));
    ok &= check(stickyButton != nullptr, "desktop sticky button is discoverable");
    qint64 firstStickyOpenNs = 0;
    qint64 secondStickyOpenNs = 0;
    if (stickyButton) {
        QElapsedTimer openTimer;
        openTimer.start();
        stickyButton->click();
        pumpEvents();
        firstStickyOpenNs = openTimer.nsecsElapsed();
        openTimer.restart();
        stickyButton->click();
        pumpEvents();
        secondStickyOpenNs = openTimer.nsecsElapsed();
        std::cout << "UI_PERF first_sticky_ms=" << (firstStickyOpenNs / 1'000'000.0)
                  << " second_sticky_ms=" << (secondStickyOpenNs / 1'000'000.0)
                  << '\n';
        ok &= check(firstStickyOpenNs < 350'000'000
                        && secondStickyOpenNs < 350'000'000,
                    "resident sticky windows open within the 350 ms regression budget");
    }

    QList<StickyNoteWindow*> stickies;
    for (QWidget* topLevel : QApplication::topLevelWidgets()) {
        if (auto* sticky = qobject_cast<StickyNoteWindow*>(topLevel))
            stickies.append(sticky);
    }
    ok &= check(stickies.size() == 2, "two independent sticky windows open together");

    qint64 firstStickyId = 0;
    qint64 secondStickyId = 0;
    if (stickies.size() == 2) {
        auto* firstEditor = stickies.at(0)->findChild<QTextEdit*>(
            QStringLiteral("stickyEditor"));
        auto* secondEditor = stickies.at(1)->findChild<QTextEdit*>(
            QStringLiteral("stickyEditor"));
        ok &= check(firstEditor && secondEditor, "both sticky editors are available");
        if (firstEditor && secondEditor) {
            firstEditor->setPlainText(QStringLiteral(
                "潮汐关卡\n记录第一枚便签中的镜头与潮位变化。"));
            secondEditor->setPlainText(QStringLiteral(
                "灯塔角色\n记录第二枚便签中的对白与剪影方向。"));
            stickies.at(0)->flushSave();
            stickies.at(1)->flushSave();
            firstStickyId = stickies.at(0)->noteId();
            secondStickyId = stickies.at(1)->noteId();
            ok &= check(stickies.at(0)->noteId() > 0
                            && stickies.at(1)->noteId() > 0
                            && stickies.at(0)->noteId() != stickies.at(1)->noteId(),
                        "each sticky persists to a distinct database note");
        }

        auto* pinButton = stickies.at(0)->findChild<QToolButton*>(
            QStringLiteral("stickyPinButton"));
        ok &= check(pinButton && pinButton->isChecked()
                        && stickies.at(0)->isPinned(),
                    "new sticky starts pinned to the desktop");
        if (pinButton) {
            pinButton->click();
            pumpEvents();
            ok &= check(!pinButton->isChecked() && !stickies.at(0)->isPinned(),
                        "desktop pin can be disabled independently");
            pinButton->click();
            pumpEvents();
            ok &= check(pinButton->isChecked() && stickies.at(0)->isPinned(),
                        "desktop pin can be restored independently");
        }

        auto* secondPinButton = stickies.at(1)->findChild<QToolButton*>(
            QStringLiteral("stickyPinButton"));
        ok &= check(secondPinButton && secondPinButton->isChecked(),
                    "second sticky starts pinned independently");
        if (secondPinButton) {
            secondPinButton->click();
            pumpEvents();
            ok &= check(stickies.at(0)->isPinned() && !stickies.at(1)->isPinned(),
                        "unpinning one sticky does not affect the other");
        }

        auto* opacitySlider = stickies.at(0)->findChild<QSlider*>(
            QStringLiteral("opacitySlider"));
        ok &= check(opacitySlider != nullptr, "sticky opacity control is discoverable");
        if (opacitySlider) {
            opacitySlider->setValue(72);
            pumpEvents();
            ok &= check(std::abs(stickies.at(0)->windowOpacity() - 0.72) < 0.02,
                        "sticky opacity changes in real time");
        }
        auto* secondOpacitySlider = stickies.at(1)->findChild<QSlider*>(
            QStringLiteral("opacitySlider"));
        ok &= check(secondOpacitySlider && secondOpacitySlider->value() == 96,
                    "changing one sticky opacity leaves the other unchanged");

        stickies.at(0)->move(mainWindow->x() + 80, mainWindow->y() + 104);
        stickies.at(1)->move(mainWindow->x() + 520, mainWindow->y() + 164);
        pumpEvents();
    }

    bool collectDialogInspected = false;
    bool collectDialogScreenshotSaved = false;
    if (collectAction && firstStickyId > 0 && secondStickyId > 0) {
        QTimer::singleShot(60, &app, [&] {
            QDialog* dialog = mainWindow->findChild<QDialog*>(
                QStringLiteral("collectStickiesDialog"));
            if (!dialog)
                return;
            auto* sourceList = dialog->findChild<QListWidget*>(
                QStringLiteral("collectStickyList"));
            auto* titleEdit = dialog->findChild<QLineEdit*>(
                QStringLiteral("collectNoteTitle"));
            auto* acceptButton = dialog->findChild<QPushButton*>(
                QStringLiteral("collectStickiesAccept"));
            collectDialogInspected = sourceList && sourceList->count() == 2
                && titleEdit && acceptButton && acceptButton->isEnabled();
            collectDialogScreenshotSaved = dialog->grab().save(
                QDir(outputDirectory).filePath(
                    QStringLiteral("Nocturne-v015-collect-dialog.png")));
            if (titleEdit)
                titleEdit->setText(QStringLiteral("UI 测试合册"));
            if (acceptButton)
                acceptButton->click();
            else
                dialog->reject();
        });
        collectAction->trigger();
        pumpEvents();

        QString collectError;
        const QList<NoteSummary> matches = database->listNoteSummaries(
            QStringLiteral("UI 测试合册"), &collectError);
        qint64 collectedId = 0;
        for (const NoteSummary& summary : matches) {
            if (summary.title == QStringLiteral("UI 测试合册")
                && summary.kind == QStringLiteral("note")) {
                collectedId = summary.id;
                break;
            }
        }
        const QList<NoteSourceRecord> sources = database->noteSources(
            collectedId, &collectError);
        bool hasFirst = false;
        bool hasSecond = false;
        for (const NoteSourceRecord& source : sources) {
            hasFirst |= source.sourceNoteId == firstStickyId;
            hasSecond |= source.sourceNoteId == secondStickyId;
        }
        ok &= check(collectDialogInspected,
                    "collect dialog lists both stickies and enables collection");
        ok &= check(collectDialogScreenshotSaved,
                    "collect dialog screenshot saves for visual regression review");
        ok &= check(collectedId > 0 && sources.size() == 2 && hasFirst && hasSecond,
                    "collect dialog creates a regular note with both durable sources");
        ok &= check(database->note(firstStickyId, &collectError).has_value()
                        && database->note(secondStickyId, &collectError).has_value(),
                    "UI collection preserves both source stickies");
    }

#ifdef Q_OS_WIN
    QString reboundHotkeyPortable;
    if (QApplication::platformName() == QStringLiteral("windows")) {
    QWidget blockerWindow;
    blockerWindow.setObjectName(QStringLiteral("hotkeyConflictOwner"));
    blockerWindow.winId();
    std::unique_ptr<GlobalHotkey> conflictOwner;
    QKeySequence conflictSequence;
    for (int functionKey = 13; functionKey <= 24 && !conflictOwner; ++functionKey) {
        const QKeySequence candidate(
            QStringLiteral("Ctrl+Alt+Shift+F%1").arg(functionKey),
            QKeySequence::PortableText);
        auto probe = std::make_unique<GlobalHotkey>(
            blockerWindow.winId(), candidate, [] {});
        if (probe->isRegistered()) {
            conflictSequence = candidate;
            conflictOwner = std::move(probe);
        }
    }
    ok &= check(conflictOwner != nullptr,
                "test reserves a deterministic hotkey conflict candidate");

    bool hotkeyDialogInspected = false;
    bool conflictReported = false;
    bool oldSettingPreserved = false;
    bool hotkeyScreenshotSaved = false;
    const QString previousSetting = shortcutSettings.value(
        GlobalHotkey::settingsKey()).toString();
    if (hotkeyAction && conflictOwner) {
        QTimer::singleShot(60, &app, [&] {
            QDialog* dialog = mainWindow->findChild<QDialog*>(
                QStringLiteral("hotkeyDialog"));
            if (!dialog)
                return;
            auto* sequenceEdit = dialog->findChild<QKeySequenceEdit*>(
                QStringLiteral("hotkeySequenceEdit"));
            auto* validationLabel = dialog->findChild<QLabel*>(
                QStringLiteral("hotkeyValidationLabel"));
            auto* applyButton = dialog->findChild<QPushButton*>(
                QStringLiteral("hotkeyApplyButton"));
            hotkeyDialogInspected = sequenceEdit && validationLabel && applyButton;
            if (!hotkeyDialogInspected) {
                dialog->reject();
                return;
            }

            sequenceEdit->setKeySequence(conflictSequence);
            applyButton->click();
            conflictReported = dialog->isVisible()
                && validationLabel->text().contains(QStringLiteral("占用"));
            QSettings checkSettings;
            oldSettingPreserved = checkSettings.value(
                GlobalHotkey::settingsKey()).toString() == previousSetting;
            hotkeyScreenshotSaved = dialog->grab().save(
                QDir(outputDirectory).filePath(
                    QStringLiteral("Nocturne-v016-hotkey-conflict.png")));

            conflictOwner.reset();
            applyButton->click();
            if (dialog->isVisible())
                dialog->reject();
        });
        hotkeyAction->trigger();
        pumpEvents();

        QSettings appliedSettings;
        const QString expected = GlobalHotkey::portableText(conflictSequence);
        const QString applied = appliedSettings.value(
            GlobalHotkey::settingsKey()).toString();
        reboundHotkeyPortable = applied;
        auto* trayStickyAction = mainWindow->findChild<QAction*>(
            QStringLiteral("trayStickyAction"));
        ok &= check(hotkeyDialogInspected && conflictReported,
                    "occupied hotkey is reported without closing the settings dialog");
        ok &= check(oldSettingPreserved,
                    "occupied hotkey leaves the persisted setting unchanged");
        ok &= check(hotkeyScreenshotSaved,
                    "hotkey conflict dialog screenshot saves for visual review");
        ok &= check(applied == expected,
                    "same hotkey persists after the conflicting owner releases it");
        const bool trayLabelUpdated = !QSystemTrayIcon::isSystemTrayAvailable()
            || (trayStickyAction
                && trayStickyAction->text().contains(
                    GlobalHotkey::displayText(conflictSequence)));
        ok &= check(stickyButton
                        && stickyButton->toolTip().contains(
                            GlobalHotkey::displayText(conflictSequence))
                        && trayLabelUpdated,
                    "successful rebind updates main and tray shortcut labels");
    }
    } else {
        std::cout << "SKIP: Win32 global hotkey conflict requires the windows platform (offscreen UI tests remain enabled)\n";
    }
#endif

    ok &= check(mainWindow->grab().save(
                    QDir(outputDirectory).filePath(
                        QStringLiteral("Nocturne-v013-implementation-main.png"))),
                "main window screenshot saves");
    for (int index = 0; index < stickies.size(); ++index) {
        ok &= check(stickies.at(index)->grab().save(
                        QDir(outputDirectory).filePath(
                            QStringLiteral("Nocturne-v013-implementation-sticky-%1.png")
                                .arg(index + 1))),
                    "sticky window screenshot saves");
    }

    if (!stickies.isEmpty()) {
        const auto stickyText = stickies.first()->findChild<QTextEdit*>(QStringLiteral("stickyEditor"))->toPlainText();
        const qreal opacity = stickies.first()->windowOpacity();
        mainWindow->findChild<QAction*>(QStringLiteral("theme_moonlight"))->trigger();
        capture(QStringLiteral("Nocturne-redesign-sticky-moonlight.png"), stickies.first());
        ok &= check(stickies.first()->findChild<QTextEdit*>(QStringLiteral("stickyEditor"))->toPlainText() == stickyText
            && std::abs(stickies.first()->windowOpacity() - opacity) < 0.01,
            "open stickies change theme without changing contents or opacity");
        mainWindow->findChild<QAction*>(QStringLiteral("theme_night"))->trigger();
        capture(QStringLiteral("Nocturne-redesign-sticky.png"), stickies.first());
    }
    auto* maximize = mainWindow->findChild<QToolButton*>(QStringLiteral("windowMaximizeButton"));
    maximize->click(); pumpEvents();
    ok &= check(mainWindow->isMaximized(), "custom maximize button retains native window behavior");
    maximize->click(); pumpEvents();
    ok &= check(!mainWindow->isMaximized(), "custom restore button restores the window");

    for (StickyNoteWindow* sticky : stickies)
        delete sticky;
    stickies.clear();

    if (firstStickyId > 0 && secondStickyId > 0) {
        auto* restoredFirst = new StickyNoteWindow(database.get(), firstStickyId);
        auto* restoredSecond = new StickyNoteWindow(database.get(), secondStickyId);
        restoredFirst->summon();
        restoredSecond->summon();
        pumpEvents();
        auto* restoredFirstEditor = restoredFirst->findChild<QTextEdit*>(
            QStringLiteral("stickyEditor"));
        auto* restoredSecondEditor = restoredSecond->findChild<QTextEdit*>(
            QStringLiteral("stickyEditor"));
        auto* restoredFirstOpacity = restoredFirst->findChild<QSlider*>(
            QStringLiteral("opacitySlider"));
        auto* restoredSecondOpacity = restoredSecond->findChild<QSlider*>(
            QStringLiteral("opacitySlider"));
        ok &= check(restoredFirstEditor && restoredSecondEditor
                        && restoredFirstEditor->toPlainText().startsWith(
                            QStringLiteral("潮汐关卡"))
                        && restoredSecondEditor->toPlainText().startsWith(
                            QStringLiteral("灯塔角色")),
                    "both sticky contents restore by their own database ids");
        ok &= check(restoredFirstOpacity && restoredSecondOpacity
                        && restoredFirstOpacity->value() == 72
                        && restoredSecondOpacity->value() == 96,
                    "per-sticky opacity survives window recreation");
        ok &= check(restoredFirst->isPinned() && !restoredSecond->isPinned(),
                    "per-sticky pin state survives window recreation");
        delete restoredFirst;
        delete restoredSecond;
    }

    ok &= runDocumentWorkflowTests(*database, *mainWindow, outputDirectory);
    ok &= runWorkspaceUiTests(*database, *mainWindow, outputDirectory);
    mainWindow.reset();

#ifdef Q_OS_WIN
    if (!reboundHotkeyPortable.isEmpty()) {
        auto restartedWindow = std::make_unique<MainWindow>(database.get());
        auto* restartedStickyButton = restartedWindow->findChild<QPushButton*>(
            QStringLiteral("secondaryButton"));
        const QKeySequence persistedSequence(reboundHotkeyPortable,
                                             QKeySequence::PortableText);
        ok &= check(restartedStickyButton
                        && restartedStickyButton->toolTip().contains(
                            GlobalHotkey::displayText(persistedSequence)),
                    "persisted global hotkey loads into a recreated main window");
        restartedWindow.reset();
    }
#endif

    database.reset();
    QDir(dataDirectory).removeRecursively();

    if (ok) {
        std::cout << "PASS: desktop UI, stickies, collection and hotkey conflict smoke test\n";
    }
    return ok ? 0 : 1;
}
