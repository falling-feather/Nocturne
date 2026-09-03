#include "Database.h"
#include "MainWindow.h"
#include "StickyNoteWindow.h"

#include <QApplication>
#include <QAction>
#include <QDir>
#include <QDialog>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QStandardPaths>
#include <QTextEdit>
#include <QTimer>
#include <QToolButton>
#include <QUuid>

#include <cmath>
#include <iostream>
#include <memory>

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

    const QString outputDirectory = argc > 1
        ? QDir::cleanPath(QString::fromLocal8Bit(argv[1]))
        : QDir::current().filePath(QStringLiteral("ui-smoke"));
    bool ok = check(QDir().mkpath(outputDirectory), "UI screenshot directory creates");

    auto database = std::make_unique<Database>();
    QString error;
    ok &= check(database->open(&error), "UI smoke database opens");
    if (!ok) {
        std::cerr << error.toStdString() << '\n';
        return 1;
    }
    const QString dataDirectory = database->dataDirectory();

    auto mainWindow = std::make_unique<MainWindow>(database.get());
    mainWindow->show();
    pumpEvents();

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

    mainWindow.reset();
    database.reset();
    QDir(dataDirectory).removeRecursively();

    if (ok) {
        std::cout << "PASS: desktop UI, multiple stickies, pin and opacity smoke test\n";
    }
    return ok ? 0 : 1;
}
