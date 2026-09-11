#include "MainWindow.h"
#include "VoiceAudio.h"
#include "NoteEditor.h"
#include "NocturneDialogs.h"
#include "NocturneStyle.h"
#include <QAction>
#include <QCheckBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QStackedWidget>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

void MainWindow::showVoiceInput()
{
    NocturneDialog dialog(this);
    dialog.setObjectName("voiceDialog");
    dialog.setWindowTitle(QStringLiteral("语音输入与转写 · 夜航"));
    dialog.resize(760, 640);
    auto* layout = new QVBoxLayout(dialog.body());
    layout->setContentsMargins(24, 20, 24, 20);
    auto* description
        = new QLabel(QStringLiteral("中文离线转写 · 在本机处理，先校对再写入笔记。\n首次使用安装约 36 MiB "
                                    "的可选语音组件，平时不加载模型。"),
            dialog.body());
    description->setWordWrap(true);
    layout->addWidget(description);
    auto* inputRow = new QHBoxLayout;
    auto* device = new NocturneComboBox(dialog.body());
    device->setObjectName("voiceDevice");
    device->addItem(QStringLiteral("系统默认麦克风"), -1);
    const auto devices = VoiceRecorder::devices();
    for (int i = 0; i < devices.size(); ++i)
        device->addItem(devices[i], i);
    inputRow->addWidget(device, 1);
    auto* record = new QPushButton(QStringLiteral("开始录音"), dialog.body());
    record->setObjectName("recordVoiceButton");
    auto* choose = new QPushButton(QStringLiteral("选择已有录音"), dialog.body());
    choose->setObjectName("chooseAudioButton");
    inputRow->addWidget(record);
    inputRow->addWidget(choose);
    layout->addLayout(inputRow);
    auto* level = new QProgressBar(dialog.body());
    level->setRange(0, 100);
    level->setValue(0);
    level->setTextVisible(false);
    level->setFixedHeight(6);
    layout->addWidget(level);
    auto* status = new QLabel(QStringLiteral("尚未录音。麦克风只在点击“开始录音”后开启。"), dialog.body());
    status->setObjectName("voiceStatus");
    status->setWordWrap(true);
    layout->addWidget(status);
    auto* result = new QPlainTextEdit(dialog.body());
    result->setObjectName("voiceTranscript");
    result->setPlaceholderText(QStringLiteral("转写结果会显示在这里，可先修改错字、专有名词和标点。"));
    layout->addWidget(result, 1);
    auto* fileRow = new QHBoxLayout;
    auto* transcribe = new QPushButton(QStringLiteral("转写 / 重试"), dialog.body());
    transcribe->setObjectName("transcribeAudioButton");
    auto* cancel = new QPushButton(QStringLiteral("取消当前任务"), dialog.body());
    cancel->setObjectName("cancelVoiceButton");
    auto* install = new QPushButton(QStringLiteral("安装语音组件"), dialog.body());
    install->setObjectName("installVoiceButton");
    auto* pending = new QPushButton(QStringLiteral("未完成录音"), dialog.body());
    pending->setObjectName("pendingRecordingsButton");
    fileRow->addWidget(transcribe);
    fileRow->addWidget(cancel);
    fileRow->addWidget(install);
    fileRow->addWidget(pending);
    layout->addLayout(fileRow);
    auto* preserve = new QCheckBox(QStringLiteral("插入文字后保留本次录音文件"), dialog.body());
    layout->addWidget(preserve);
    auto* actions = new QHBoxLayout;
    auto* insert = new QPushButton(QStringLiteral("插入到当前光标"), dialog.body());
    insert->setObjectName("insertTranscriptButton");
    auto* create = new QPushButton(QStringLiteral("新建语音笔记"), dialog.body());
    create->setObjectName("newTranscriptNoteButton");
    auto* close = new QPushButton(QStringLiteral("关闭"), dialog.body());
    actions->addWidget(insert);
    actions->addWidget(create);
    actions->addStretch();
    actions->addWidget(close);
    layout->addLayout(actions);
    const QString root = QDir(m_database->dataDirectory()).filePath("voice");
    const QString recordings = QDir(root).filePath("recordings");
    const QString jobs = QDir(root).filePath("jobs");
    QDir().mkpath(recordings);
    QDir().mkpath(jobs);
    auto ready = [&]
    {
        return QFileInfo::exists(QDir(root).filePath("installed.json"))
            && QFileInfo::exists(QDir(root).filePath("runtime/sherpa-onnx.exe"))
            && QFileInfo::exists(QDir(root).filePath("model/model.int8.onnx"))
            && QFileInfo::exists(QDir(root).filePath("model/tokens.txt"));
    };
    enum class Stage
    {
        Idle,
        Installing,
        Preparing,
        Recognizing
    };
    Stage stage = Stage::Idle;
    QString inputFile, preparedFile, ownRecording;
    QByteArray output;
    bool closing = false, cancelled = false;
    VoiceRecorder recorder;
    QProcess worker;
    worker.setProcessChannelMode(QProcess::MergedChannels);
#ifdef Q_OS_WIN
    worker.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args)
        { args->flags |= CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS; });
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job)
    {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
        {
            CloseHandle(job);
            job = nullptr;
        }
    }
    connect(&worker, &QProcess::started, &dialog,
        [&]
        {
            if (!job)
                return;
            HANDLE process
                = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, DWORD(worker.processId()));
            if (process)
            {
                const bool assigned = AssignProcessToJobObject(job, process);
                CloseHandle(process);
                if (!assigned && stage == Stage::Installing)
                {
                    cancelled = true;
                    worker.kill();
                    status->setText(QStringLiteral("无法建立可取消的安装任务，安装未继续。"));
                }
            }
        });
#endif
    auto update = [&]
    {
        const bool idle = stage == Stage::Idle && !recorder.isRecording();
        record->setEnabled(stage == Stage::Idle);
        record->setText(recorder.isRecording() ? QStringLiteral("停止并转写")
                : recorder.seconds() > 0       ? QStringLiteral("重试保存录音")
                                               : QStringLiteral("开始录音"));
        choose->setEnabled(idle);
        device->setEnabled(idle);
        transcribe->setEnabled(idle && !inputFile.isEmpty() && ready());
        install->setEnabled(idle && !ready());
        cancel->setEnabled(!idle);
        pending->setEnabled(idle);
        insert->setEnabled(idle && !result->toPlainText().trimmed().isEmpty() && m_currentNoteId > 0
            && m_currentNoteKind != "sticky");
        create->setEnabled(idle && !result->toPlainText().trimmed().isEmpty());
    };
    auto startRecognition = [&]
    {
        stage = Stage::Recognizing;
        output.clear();
        cancelled = false;
        worker.setWorkingDirectory(root);
        QStringList args { "--zipformer2-ctc-model=./model/model.int8.onnx", "--tokens=./model/tokens.txt",
            "--provider=cpu", "--num-threads=2", "--enable-endpoint=false", "--print-args=false",
            QDir(root).relativeFilePath(preparedFile) };
        status->setText(QStringLiteral("正在本机转写，可随时取消…"));
        worker.start(QDir(root).filePath("runtime/sherpa-onnx.exe"), args);
        update();
    };
    auto begin = [&]
    {
        if (stage != Stage::Idle || inputFile.isEmpty())
            return;
        if (!ready())
        {
            status->setText(QStringLiteral("录音已保留，请先安装语音组件，然后点击“转写 / 重试”。"));
            update();
            return;
        }
        preparedFile = QDir(jobs).filePath(QUuid::createUuid().toString(QUuid::Id128) + ".wav");
        stage = Stage::Preparing;
        output.clear();
        cancelled = false;
        status->setText(QStringLiteral("正在准备录音…"));
        worker.setWorkingDirectory(root);
        worker.start(QDir(QCoreApplication::applicationDirPath()).filePath("NocturneAudio.exe"),
            { "--prepare-audio", inputFile, preparedFile });
        update();
    };
    auto stopRecording = [&]
    {
        if (!recorder.isRecording() && recorder.seconds() <= 0)
            return;
        const QString file = QDir(recordings)
                                 .filePath(QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss") + "-"
                                     + QUuid::createUuid().toString(QUuid::Id128).left(8) + ".wav");
        QString error;
        if (recorder.stopToFile(file, &error))
        {
            inputFile = file;
            ownRecording = file;
            status->setText(QStringLiteral("录音已保存，可重试转写：%1").arg(QFileInfo(file).fileName()));
        }
        else
            status->setText(error);
        level->setValue(0);
        update();
    };
    connect(record, &QPushButton::clicked, &dialog,
        [&]
        {
            if (recorder.isRecording() || recorder.seconds() > 0)
            {
                stopRecording();
                begin();
            }
            else
            {
                QString error;
                if (!recorder.start(device->currentData().toInt(), &error))
                    status->setText(error);
                else
                {
                    result->clear();
                    inputFile.clear();
                    ownRecording.clear();
                    status->setText(QStringLiteral("正在录音…"));
                }
                update();
            }
        });
    connect(&recorder, &VoiceRecorder::levelChanged, level, &QProgressBar::setValue);
    connect(&recorder, &VoiceRecorder::recordingError, &dialog,
        [&](const QString& message)
        {
            stopRecording();
            status->setText(message + QStringLiteral(" 可重试转写。"));
        });
    connect(&recorder, &VoiceRecorder::recordingLimitReached, &dialog,
        [&]
        {
            stopRecording();
            status->setText(QStringLiteral("已达到本次录音容量上限，录音已保存。"));
        });
    QTimer timer;
    timer.setInterval(1000);
    connect(&timer, &QTimer::timeout, &dialog,
        [&]
        {
            if (recorder.isRecording())
            {
                const int seconds = int(recorder.seconds());
                status->setText(QStringLiteral("正在录音 · %1:%2")
                        .arg(seconds / 60, 2, 10, QChar('0'))
                        .arg(seconds % 60, 2, 10, QChar('0')));
                if (seconds >= 1800)
                {
                    stopRecording();
                    status->setText(QStringLiteral("录音已达到 30 分钟，已保存。"));
                }
            }
        });
    timer.start();
    connect(choose, &QPushButton::clicked, &dialog,
        [&]
        {
            const QString file = NocturneDialogs::getOpenFileName(&dialog, QStringLiteral("选择录音文件"),
                QString(), QStringLiteral("录音 (*.wav *.mp3 *.m4a *.aac *.wma *.flac)"));
            if (file.isEmpty())
                return;
            inputFile = file;
            ownRecording.clear();
            result->clear();
            begin();
        });
    connect(transcribe, &QPushButton::clicked, &dialog, begin);
    connect(install, &QPushButton::clicked, &dialog,
        [&]
        {
#ifdef Q_OS_WIN
            if (!job)
            {
                status->setText(QStringLiteral("无法建立可取消的后台任务，请重新打开语音窗口后再试。"));
                return;
            }
#endif
            stage = Stage::Installing;
            output.clear();
            cancelled = false;
            status->setText(QStringLiteral("正在下载并校验语音组件；中断后可重试续传。"));
            worker.setWorkingDirectory(root);
            worker.start("powershell.exe",
                { "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                    QDir(QCoreApplication::applicationDirPath()).filePath("install-voice.ps1"),
                    "-VoiceDirectory", root });
            update();
        });
    connect(&worker, &QProcess::readyReadStandardOutput, &dialog,
        [&]
        {
            output += worker.readAllStandardOutput();
            if (output.size() > 2 * 1024 * 1024)
                output = output.right(2 * 1024 * 1024);
        });
    connect(&worker, &QProcess::errorOccurred, &dialog,
        [&](QProcess::ProcessError error)
        {
            if (closing)
                return;
            if (error == QProcess::FailedToStart)
            {
                stage = Stage::Idle;
                status->setText(QStringLiteral(
                    "组件无法启动：%1\n录音仍保留，可重试安装或检查 Windows 媒体组件 / VC++ 运行库。")
                        .arg(worker.errorString()));
                update();
            }
        });
    connect(&worker, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), &dialog,
        [&](int code, QProcess::ExitStatus exit)
        {
            output += worker.readAllStandardOutput();
            if (closing)
                return;
            const auto completed = stage;
            stage = Stage::Idle;
            if (cancelled)
            {
                status->setText(QStringLiteral("已取消；原始录音保留，可稍后重试。"));
                update();
                return;
            }
            if (code != 0 || exit != QProcess::NormalExit)
            {
                status->setText(
                    QStringLiteral("任务未完成，录音保留。\n%1").arg(QString::fromUtf8(output.right(650))));
                update();
                return;
            }
            if (completed == Stage::Preparing)
            {
                startRecognition();
                return;
            }
            if (completed == Stage::Installing)
            {
                status->setText(ready()
                        ? QStringLiteral("中文离线语音组件已就绪。可以开始录音或重试已有录音。")
                        : QStringLiteral("组件不完整，请重试安装。"));
            }
            if (completed == Stage::Recognizing)
            {
                const QString text = VoiceAudio::transcript(output);
                if (text.isEmpty())
                    status->setText(QStringLiteral("未识别到文字，可检查录音或重试。"));
                else
                {
                    result->setPlainText(text);
                    status->setText(QStringLiteral("转写完成，请校对后插入。"));
                }
                if (!preparedFile.isEmpty()
                    && QFileInfo(preparedFile).absolutePath() == QDir(jobs).absolutePath())
                    QFile::remove(preparedFile);
                preparedFile.clear();
            }
            update();
        });
    auto cancelWorker = [&]
    {
        if (worker.state() != QProcess::NotRunning)
        {
            cancelled = true;
#ifdef Q_OS_WIN
            if (job)
                TerminateJobObject(job, 1);
#endif
            worker.kill();
        }
    };
    connect(cancel, &QPushButton::clicked, &dialog,
        [&]
        {
            if (recorder.isRecording())
            {
                stopRecording();
                return;
            }
            cancelWorker();
        });
    connect(pending, &QPushButton::clicked, &dialog,
        [&]
        {
            const QString file = NocturneDialogs::getOpenFileName(
                &dialog, QStringLiteral("选择未完成录音"), recordings, QStringLiteral("录音 (*.wav)"));
            if (!file.isEmpty())
            {
                inputFile = file;
                ownRecording = file;
                result->clear();
                begin();
            }
        });
    auto finishRecording = [&]
    {
        if (!preserve->isChecked() && !ownRecording.isEmpty()
            && QFileInfo(ownRecording).absolutePath() == QDir(recordings).absolutePath())
            QFile::remove(ownRecording);
    };
    connect(insert, &QPushButton::clicked, &dialog,
        [&]
        {
            const auto text = result->toPlainText().trimmed();
            if (text.isEmpty())
                return;
            if (m_linkedSource)
            {
                m_sourceEditor->insertPlainText(text);
            }
            else
            {
                m_editor->insertPlainText(text);
            }
            scheduleSave();
            if (!saveCurrentNote())
            {
                status->setText(QStringLiteral("文字已放入编辑器，保存尚未完成；录音仍保留。"));
                return;
            }
            finishRecording();
            dialog.accept();
        });
    connect(create, &QPushButton::clicked, &dialog,
        [&]
        {
            const auto text = result->toPlainText().trimmed();
            if (text.isEmpty() || !saveCurrentNote())
                return;
            QString error;
            NoteEditor content;
            content.setPlainText(text);
            const auto id = m_database->createNote(
                QStringLiteral("语音笔记 %1").arg(QDateTime::currentDateTime().toString("MM-dd HH:mm")),
                content.toHtml(), text, &error, m_currentFolderId);
            if (!id)
            {
                status->setText(error);
                return;
            }
            finishRecording();
            dialog.accept();
            openNoteById(id);
        });
    connect(result, &QPlainTextEdit::textChanged, &dialog, update);
    connect(close, &QPushButton::clicked, &dialog, &QDialog::reject);
    update();
    while (true)
    {
        dialog.exec();
        if (recorder.isRecording() || recorder.seconds() > 0)
            stopRecording();
        if (recorder.seconds() <= 0)
            break;
        if (NocturneDialogs::question(&dialog, QStringLiteral("录音尚未保存"),
                QStringLiteral("录音文件保存失败。关闭将放弃这段尚未保存的声音，仍要关闭吗？"))
            == QMessageBox::Yes)
            break;
    }
    closing = true;
    timer.stop();
    worker.disconnect(&dialog);
    cancelWorker();
    if (worker.state() != QProcess::NotRunning)
        worker.waitForFinished(3000);
#ifdef Q_OS_WIN
    if (job)
        CloseHandle(job);
#endif
}
