#include "Database.h"
#include "WorkspaceStore.h"
#include "SourceFile.h"
#include "DocumentImporter.h"
#include "MainWindow.h"
#include "NoteEditor.h"
#include "NotebookTree.h"
#include "StickyNoteWindow.h"
#include "AiExchange.h"
#include "VoiceAudio.h"
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QFileDialog>
#include <QJsonDocument>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTimer>
#include <QToolButton>
#include <functional>
#include <iostream>

namespace
{
void settle(int ms = 260)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}
}
bool runWorkspaceUiTests(Database& database, MainWindow& window, const QString& outputDirectory)
{
    bool ok = true;
    QString error;
    WorkspaceStore store(database);
    auto check = [&](bool condition, const char* message)
    {
        if (!condition)
        {
            ok = false;
            std::cerr << "FAIL WORKSPACE UI: " << message << '\n';
        }
    };
    auto* search = window.findChild<QLineEdit*>("searchEdit");
    auto* filter = window.findChild<QComboBox*>("folderFilter");
    auto* editor = window.findChild<NoteEditor*>("noteEditor");
    auto* source = window.findChild<QPlainTextEdit*>("sourceEditor");
    auto* body = window.findChild<QStackedWidget*>("bodyStack");
    auto open = [&](const QString& title)
    {
        filter->setCurrentIndex(0);
        search->setText(title);
        settle(400);
    };
    auto modal = [&](const QString& name, const std::function<void()>& launch,
                     const std::function<void(QDialog*)>& inspect)
    {
        bool seen = false;
        QTimer driver, timeout;
        timeout.setSingleShot(true);
        QObject::connect(&driver, &QTimer::timeout, &window,
            [&]
            {
                auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                if (dialog && dialog->objectName() == name && !seen)
                {
                    seen = true;
                    inspect(dialog);
                }
            });
        QObject::connect(&timeout, &QTimer::timeout, &window,
            [&]
            {
                check(false, "modal workflow timeout");
                if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget()))
                    dialog->reject();
            });
        driver.start(30);
        timeout.start(6000);
        launch();
        driver.stop();
        timeout.stop();
        check(seen, "expected original dialog opens");
    };
    std::cerr << "WORKSPACE UI stage: source safety\n";
    QTemporaryDir files;
    const QString path = files.filePath("project.md");
    const QByteArray original = QByteArray::fromHex("efbbbf")
        + "---\r\ncustom: exact\r\n---\r\n# Source\r\n\r\n![image](images/a.png)\r\n\r\nalpha\r\n";
    QFile file(path);
    check(file.open(QIODevice::WriteOnly), "source fixture opens");
    file.write(original);
    file.close();
    QString html, plain;
    QByteArray bytes;
    check(DocumentImporter::readDocument(path, &html, &plain, &bytes, &error), "source fixture parses");
    bool skipped = false;
    const auto raw = SourceFile::read(path);
    const auto linkedId = database.linkNote(
        raw->path, raw->hash, "md", QStringLiteral("外部源码回归"), html, plain, 0, &skipped, &error, &bytes);
    check(linkedId > 0, "linked note creates");
    open(QStringLiteral("外部源码回归"));
    check(body->currentIndex() == 1 && !source->isReadOnly(),
        "external documents open in editable source mode");
    source->moveCursor(QTextCursor::End);
    source->insertPlainText("new line\n");
    settle(1400);
    auto saved = SourceFile::read(path, &error);
    check(saved && saved->bytes.startsWith(original) && saved->bytes.endsWith("new line\r\n"),
        "real autosave preserves BOM, CRLF, front matter and relative images");
    const QByteArray external = original + "external author\r\n";
    check(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "external author opens source");
    file.write(external);
    file.close();
    source->insertPlainText("my unsaved text\n");
    settle(1400);
    check(SourceFile::read(path)->bytes == external, "main window does not overwrite changed external file");
    check(source->toPlainText().contains("my unsaved text") && !store.drafts().isEmpty(),
        "conflicting edits remain visible and in recovery drafts");
    modal(
        "sourceConflictDialog", [&] { window.findChild<QToolButton*>("compareSourceButton")->click(); },
        [&](QDialog* dialog) { dialog->findChild<QPushButton*>("sourceAdoptExternalButton")->click(); });
    check(source->toPlainText().contains("external author")
            && !source->toPlainText().contains("my unsaved text"),
        "adopting external content is explicit");
    check(!store.drafts().isEmpty(), "adoption retains the user's earlier draft");
    const auto internalId = database.createNote(QStringLiteral("检索与航迹回归"),
        "<h1>Plan</h1><p><b>alpha</b> alpha</p>", "Plan\nalpha alpha", &error);
    open(QStringLiteral("检索与航迹回归"));
    check(body->currentIndex() == 0, "internal note uses rich editor");
    std::cerr << "WORKSPACE UI stage: find, history, recovery\n";
    window.findChild<QAction*>("replaceNoteAction")->trigger();
    window.findChild<QLineEdit*>("findQuery")->setText("alpha");
    window.findChild<QLineEdit*>("replaceText")->setText("beta");
    window.findChild<QPushButton*>("replaceAllButton")->click();
    check(editor->toPlainText().count("beta") == 2 && !editor->toPlainText().contains("alpha"),
        "replace all affects this document");
    editor->undo();
    check(editor->toPlainText().count("alpha") == 2, "replace all undoes in one step");
    window.findChild<QFrame*>("findBar")->hide();
    settle(1200);
    modal(
        "historyDialog", [&] { window.findChild<QAction*>("historyAction")->trigger(); },
        [&](QDialog* dialog)
        {
            check(dialog->findChild<QListWidget*>("versionList")->count() > 0,
                "history exposes actual saved states");
            check(dialog->grab().save(QDir(outputDirectory).filePath("Nocturne-history.png")),
                "history screenshot saves");
            dialog->accept();
        });
    const auto removed = database.createNote(
        QStringLiteral("待恢复样例"), "<p>recover this body</p>", "recover this body", &error);
    database.softDeleteNote(removed, &error);
    modal(
        "recoveryDialog", [&] { window.findChild<QAction*>("recoveryAction")->trigger(); },
        [&](QDialog* dialog)
        {
            auto* list = dialog->findChild<QListWidget*>("recoveryList");
            for (int i = 0; i < list->count(); ++i)
                if (list->item(i)->data(Qt::UserRole).toLongLong() == removed)
                    list->setCurrentRow(i);
            dialog->findChild<QPushButton*>("recoverNoteButton")->click();
        });
    check(database.note(removed) && database.note(removed)->plainText == "recover this body",
        "recovery restores selected note without replacing the library");
    open(QStringLiteral("检索与航迹回归"));
    auto snapshot = store.snapshot(internalId);
    const auto templateId = store.saveTemplate(QStringLiteral("复盘模板"), *snapshot, &error);
    check(templateId > 0, "template fixture saves");
    modal(
        "templateDialog", [&] { window.findChild<QAction*>("templatesAction")->trigger(); },
        [&](QDialog* dialog)
        {
            dialog->findChild<QListWidget*>("templateList")->setCurrentRow(0);
            dialog->findChild<QPushButton*>("useTemplateButton")->click();
        });
    check(database.listNoteSummaries(QStringLiteral("复盘模板")).size() == 1,
        "template creates a separate note");
    open(QStringLiteral("检索与航迹回归"));
    QTextCursor selection = editor->document()->find("alpha");
    editor->setTextCursor(selection);
    editor->captureRequested();
    settle(250);
    StickyNoteWindow* captured = nullptr;
    for (auto* widget : QApplication::topLevelWidgets())
        if (auto* sticky = qobject_cast<StickyNoteWindow*>(widget);
            sticky && store.captureSource(sticky->noteId()))
            captured = sticky;
    check(captured != nullptr, "selection creates a sourced sticky");
    if (captured)
    {
        auto* sourceAction = captured->findChild<QAction*>("stickySourceAction");
        check(sourceAction != nullptr, "sticky has a source return action");
        if (sourceAction)
            sourceAction->trigger();
        delete captured;
    }
    check(editor->toPlainText().contains("alpha"), "capture retains source note content");
    std::cerr << "WORKSPACE UI stage: MCP scope\n";
    QTemporaryDir exchangeRoot;
    auto handoff = AiExchange::create(database, { internalId }, exchangeRoot.path(), false, true);
    check(handoff.error.isEmpty() && handoff.noteCount == 1, "handoff includes selected note only");
    const auto session = AiExchange::readSession(handoff.manifestPath);
    const auto entry = session.value("notes").toArray().first().toObject();
    const QJsonObject idArg { { "id", QString::number(internalId) } };
    check(!AiExchange::callTool(handoff.manifestPath, "read_note", idArg).value("isError").toBool(),
        "MCP reads allowed note");
    check(AiExchange::callTool(handoff.manifestPath, "read_note", { { "id", QString::number(linkedId) } })
              .value("isError")
              .toBool(),
        "MCP rejects another project's note");
    check(AiExchange::callTool(handoff.manifestPath, "propose_edit",
              { { "id", QString::number(internalId) }, { "expected_hash", "stale" },
                  { "markdown", "not applied" } })
              .value("isError")
              .toBool(),
        "MCP rejects stale proposal version");
    const auto before = database.note(internalId)->contentHash;
    check(!AiExchange::callTool(handoff.manifestPath, "propose_edit",
              { { "id", QString::number(internalId) }, { "expected_hash", entry.value("exportHash") },
                  { "markdown", "# Updated\n\nReviewed AI content" } })
              .value("isError")
              .toBool(),
        "MCP queues a valid proposal");
    check(database.note(internalId)->contentHash == before, "proposing edits does not write notes");
    QSettings().setValue("ai/lastSession", handoff.manifestPath);
    modal(
        "aiHandoffDialog", [&] { window.findChild<QAction*>("aiHandoffAction")->trigger(); },
        [&](QDialog* dialog)
        {
            dialog->findChild<QListWidget*>("proposalList")->setCurrentRow(0);
            dialog->findChild<QPushButton*>("applyProposalButton")->click();
            dialog->accept();
        });
    check(database.note(internalId)->plainText.contains("Reviewed AI content"),
        "confirmed proposal follows the normal save path");
    QProcess mcp;
    mcp.start(QDir(QCoreApplication::applicationDirPath()).filePath("Nocturne.exe"),
        { "--mcp", "--session", handoff.manifestPath });
    check(mcp.waitForStarted(5000), "real MCP subprocess starts");
    auto request = [&](int requestId, const QString& method, const QJsonObject& params)
    {
        mcp.write(QJsonDocument(QJsonObject { { "jsonrpc", "2.0" }, { "id", requestId }, { "method", method },
                                    { "params", params } })
                      .toJson(QJsonDocument::Compact)
            + '\n');
        mcp.waitForBytesWritten(1000);
        QByteArray response;
        for (int attempt = 0; attempt < 20 && !response.contains('\n'); ++attempt)
        {
            mcp.waitForReadyRead(250);
            response += mcp.readAllStandardOutput();
        }
        return QJsonDocument::fromJson(response.trimmed()).object();
    };
    const auto initialized = request(1, "initialize",
        { { "protocolVersion", "2025-11-25" }, { "capabilities", QJsonObject {} },
            { "clientInfo", QJsonObject { { "name", "NocturneTest" }, { "version", "1" } } } });
    check(initialized.value("result").toObject().value("serverInfo").toObject().value("name").toString()
            == "Nocturne",
        "MCP initialize returns a real protocol response");
    check(request(2, "tools/list", {}).value("result").toObject().value("tools").toArray().size() == 6,
        "MCP exposes its bounded tool set");
    check(AiExchange::setActive(handoff.manifestPath, false, &error), "user can revoke sharing");
    check(mcp.waitForFinished(3500), "revoking sharing releases the MCP subprocess");
    if (mcp.state() != QProcess::NotRunning)
    {
        mcp.kill();
        mcp.waitForFinished();
    }
    check(AiExchange::callTool(handoff.manifestPath, "read_note", idArg).value("isError").toBool(),
        "revoked sharing rejects further reads");
    const auto limitedId = database.createNote(QStringLiteral("选区共享回归"),
        "<p>TOP_SECRET</p><p>PUBLIC_SECTION</p><p>BOTTOM_SECRET</p>",
        "TOP_SECRET\nPUBLIC_SECTION\nBOTTOM_SECRET", &error);
    open(QStringLiteral("选区共享回归"));
    const auto range = editor->document()->find("PUBLIC_SECTION");
    HandoffSelection limited;
    limited.noteId = limitedId;
    limited.start = range.selectionStart();
    limited.end = range.selectionEnd();
    limited.markdown = "PUBLIC_SECTION";
    limited.noteHash = database.note(limitedId)->contentHash;
    const auto selectedHandoff
        = AiExchange::create(database, { limitedId }, exchangeRoot.path(), false, true, limited);
    check(selectedHandoff.error.isEmpty(), "selected-range handoff creates");
    const auto selectedSession = AiExchange::readSession(selectedHandoff.manifestPath);
    const auto selectedEntry = selectedSession.value("notes").toArray().first().toObject();
    const auto selectedRead = AiExchange::callTool(
        selectedHandoff.manifestPath, "read_note", { { "id", QString::number(limitedId) } });
    const auto selectedText
        = selectedRead.value("content").toArray().first().toObject().value("text").toString();
    check(selectedText.contains("PUBLIC_SECTION") && !selectedText.contains("TOP_SECRET")
            && !selectedText.contains("BOTTOM_SECRET") && selectedSession.value("todos").toArray().isEmpty(),
        "selection sharing excludes neighboring text and unselected todos");
    AiExchange::callTool(selectedHandoff.manifestPath, "propose_edit",
        { { "id", QString::number(limitedId) }, { "expected_hash", selectedEntry.value("exportHash") },
            { "markdown", "REVISED_SECTION" } });
    QSettings().setValue("ai/lastSession", selectedHandoff.manifestPath);
    modal(
        "aiHandoffDialog", [&] { window.findChild<QAction*>("aiHandoffAction")->trigger(); },
        [&](QDialog* dialog)
        {
            dialog->findChild<QListWidget*>("proposalList")->setCurrentRow(0);
            dialog->findChild<QPushButton*>("applyProposalButton")->click();
            dialog->accept();
        });
    const auto limitedAfter = database.note(limitedId)->plainText;
    check(limitedAfter.contains("TOP_SECRET") && limitedAfter.contains("BOTTOM_SECRET")
            && limitedAfter.contains("REVISED_SECTION") && !limitedAfter.contains("PUBLIC_SECTION"),
        "confirmed selection proposal preserves surrounding paragraphs");
    AiExchange::setActive(selectedHandoff.manifestPath, false);
    std::cerr << "WORKSPACE UI stage: voice review (microphone remains closed)\n";
    check(VoiceAudio::transcript("log\n{\"text\":\"sample text\",\"tokens\":[]}\n") == "sample text",
        "transcription parser uses structured engine result");
    modal(
        "voiceDialog", [&] { window.findChild<QAction*>("voiceInputAction")->trigger(); },
        [&](QDialog* dialog)
        {
            check(dialog->findChild<QLabel*>("voiceStatus")->text().contains(QStringLiteral("尚未录音")),
                "opening voice UI does not open microphone");
            dialog->findChild<QPlainTextEdit*>("voiceTranscript")
                ->setPlainText(QStringLiteral("这是一段经过校对的语音输入示例。"));
            check(dialog->grab().save(QDir(outputDirectory).filePath("Nocturne-voice.png")),
                "voice UI screenshot saves without recording");
            dialog->findChild<QPushButton*>("newTranscriptNoteButton")->click();
        });
    check(!database.listNoteSummaries(QStringLiteral("经过校对的语音输入示例")).isEmpty(),
        "reviewed transcript can create a note");
    const QString runtimeRoot = qEnvironmentVariable("NOCTURNE_TEST_VOICE_ROOT");
    const QString sample = qEnvironmentVariable("NOCTURNE_TEST_VOICE_SAMPLE");
    if (!runtimeRoot.isEmpty() && QFileInfo::exists(sample))
    {
        std::cerr << "WORKSPACE UI stage: real offline voice pipeline\n";
        const QString destination = QDir(database.dataDirectory()).filePath("voice");
        for (const auto& relative :
            QStringList { "installed.json", "runtime/sherpa-onnx.exe", "runtime/onnxruntime.dll",
                "runtime/onnxruntime_providers_shared.dll", "model/model.int8.onnx", "model/tokens.txt" })
        {
            const QString target = QDir(destination).filePath(relative);
            QDir().mkpath(QFileInfo(target).absolutePath());
            check(QFile::copy(QDir(runtimeRoot).filePath(relative), target),
                "isolated voice runtime file copies");
        }
        bool picker = false, started = false, recognized = false;
        QString lastVoiceStatus;
        QTimer driver, timeout;
        timeout.setSingleShot(true);
        QObject::connect(&driver, &QTimer::timeout, &window,
            [&]
            {
                auto* modal = QApplication::activeModalWidget();
                if (auto* chooser = qobject_cast<QFileDialog*>(modal))
                {
                    if (!picker)
                    {
                        picker = true;
                        chooser->setDirectory(QFileInfo(sample).absolutePath());
                    }
                    // QFileSystemModel loads asynchronously; reassert the explicit file
                    // rather than accepting an empty first-frame selection.
                    chooser->selectFile(QFileInfo(sample).fileName());
                    if (auto* filename = chooser->findChild<QLineEdit*>("fileNameEdit"))
                        filename->setText(sample);
                    QMetaObject::invokeMethod(chooser, "accept", Qt::DirectConnection);
                }
                else if (auto* voice = qobject_cast<QDialog*>(modal);
                    voice && voice->objectName() == "voiceDialog")
                {
                    const QString currentStatus = voice->findChild<QLabel*>("voiceStatus")->text();
                    if (currentStatus != lastVoiceStatus)
                    {
                        lastVoiceStatus = currentStatus;
                        std::cerr << "VOICE STATUS: " << currentStatus.toStdString() << '\n';
                    }
                    if (!started)
                    {
                        started = true;
                        QTimer::singleShot(0, voice,
                            [voice] { voice->findChild<QPushButton*>("chooseAudioButton")->click(); });
                        return;
                    }
                    if (voice->findChild<QPlainTextEdit*>("voiceTranscript")
                            ->toPlainText()
                            .contains(QStringLiteral("研究")))
                    {
                        recognized = true;
                        voice->findChild<QPushButton*>("newTranscriptNoteButton")->click();
                    }
                }
            });
        QObject::connect(&timeout, &QTimer::timeout, &window,
            [&]
            {
                check(false, "offline voice workflow timeout");
                if (QApplication::activeModalWidget())
                    std::cerr << "VOICE MODAL: "
                              << QApplication::activeModalWidget()->objectName().toStdString() << '\n';
                if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget()))
                    dialog->reject();
                if (auto* voice = window.findChild<QDialog*>("voiceDialog"))
                    voice->reject();
            });
        driver.start(50);
        timeout.start(20000);
        window.findChild<QAction*>("voiceInputAction")->trigger();
        driver.stop();
        timeout.stop();
        check(picker && recognized,
            "audio picker, native decoding, optional engine and transcript insertion work end to end");
    }
    std::cout << (ok ? "PASS" : "FAIL")
              << ": source conflicts, recovery, templates, find, capture, MCP, voice insertion\n";
    return ok;
}
