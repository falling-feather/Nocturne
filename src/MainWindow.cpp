#include "MainWindow.h"

#include "Database.h"
#include "NoteEditor.h"
#include "StickyNoteWindow.h"

#include <QAbstractNativeEventFilter>
#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QCloseEvent>
#include <QColorDialog>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QImageReader>
#include <QImageWriter>
#include <QInputDialog>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QSystemTrayIcon>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextImageFormat>
#include <QTextList>
#include <QTextListFormat>
#include <QTime>
#include <QTimer>
#include <QToolButton>
#include <QUuid>
#include <QUrl>
#include <QVBoxLayout>
#include <QtMath>

#include <algorithm>
#include <functional>
#include <memory>

#ifdef Q_OS_WIN
#  include <windows.h>
#endif

namespace {
constexpr int kAutoSaveDelayMs = 650;
constexpr int kSearchDelayMs = 180;
constexpr int kMaxStoredImageSide = 1800;
constexpr int kNoteCacheMaxKiB = 24 * 1024;
constexpr int kHotkeyId = 0xF34A;

QString noteListText(const QString& title,
                     const QString& excerpt,
                     const QString& kind,
                     const QDateTime& updated)
{
    const QString safeTitle = title.trimmed().isEmpty() ? QStringLiteral("无标题笔记") : title.trimmed();
    QString safeExcerpt = excerpt.simplified();
    if (safeExcerpt.isEmpty())
        safeExcerpt = kind == QStringLiteral("sticky") ? QStringLiteral("快捷便签") : QStringLiteral("空白笔记");
    safeExcerpt = safeExcerpt.left(42);
    const QString time = updated.isValid()
        ? updated.toLocalTime().toString(QStringLiteral("MM-dd  HH:mm"))
        : QStringLiteral("刚刚");
    const QString badge = kind == QStringLiteral("sticky") ? QStringLiteral("便签 · ") : QString();
    return QStringLiteral("%1\n%2\n%3%4").arg(safeTitle, safeExcerpt, badge, time);
}

QByteArray htmlHash(const QString& html)
{
    return QCryptographicHash::hash(html.toUtf8(), QCryptographicHash::Sha256);
}

QString plainTextExcerpt(const QString& plainText)
{
    return plainText.simplified().left(180);
}

QIcon createAppIcon()
{
    QPixmap pixmap(128, 128);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(QStringLiteral("#4F6B5B")));
    painter.drawRoundedRect(QRectF(8, 8, 112, 112), 28, 28);

    QPainterPath feather;
    feather.moveTo(38, 92);
    feather.cubicTo(49, 58, 67, 34, 96, 27);
    feather.cubicTo(91, 57, 70, 78, 38, 92);
    painter.setBrush(QColor(QStringLiteral("#FFFDF7")));
    painter.drawPath(feather);
    painter.setPen(QPen(QColor(QStringLiteral("#D8E3DA")), 5, Qt::SolidLine, Qt::RoundCap));
    painter.drawLine(QPointF(34, 100), QPointF(81, 46));
    return QIcon(pixmap);
}

QString databaseErrorText(const QString& action, const QString& error)
{
    return error.isEmpty() ? action : QStringLiteral("%1：%2").arg(action, error);
}
}

class GlobalHotkey final : public QObject, public QAbstractNativeEventFilter
{
public:
    GlobalHotkey(WId windowId, std::function<void()> callback, QObject* parent)
        : QObject(parent)
        , m_windowId(windowId)
        , m_callback(std::move(callback))
    {
        qApp->installNativeEventFilter(this);
#ifdef Q_OS_WIN
        m_registered = RegisterHotKey(reinterpret_cast<HWND>(m_windowId),
                                      kHotkeyId,
                                      MOD_CONTROL | MOD_ALT | MOD_NOREPEAT,
                                      'N');
#endif
    }

    ~GlobalHotkey() override
    {
#ifdef Q_OS_WIN
        if (m_registered)
            UnregisterHotKey(reinterpret_cast<HWND>(m_windowId), kHotkeyId);
#endif
        if (qApp)
            qApp->removeNativeEventFilter(this);
    }

    bool isRegistered() const { return m_registered; }

    bool nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) override
    {
        Q_UNUSED(eventType)
        Q_UNUSED(result)
#ifdef Q_OS_WIN
        const MSG* nativeMessage = static_cast<const MSG*>(message);
        if (nativeMessage && nativeMessage->message == WM_HOTKEY
            && nativeMessage->wParam == kHotkeyId) {
            if (m_callback)
                m_callback();
            return true;
        }
#else
        Q_UNUSED(message)
#endif
        return false;
    }

private:
    WId m_windowId = 0;
    std::function<void()> m_callback;
    bool m_registered = false;
};

MainWindow::MainWindow(Database* database, QWidget* parent)
    : QMainWindow(parent)
    , m_database(database)
{
    m_noteCache.setMaxCost(kNoteCacheMaxKiB);
    setObjectName(QStringLiteral("mainWindow"));
    setWindowTitle(QStringLiteral("FeatherNote · 轻羽笔记"));
    setWindowIcon(createAppIcon());
    resize(1220, 780);
    setMinimumSize(980, 620);

    buildUi();
    buildMenus();
    applyTheme();
    connectSignals();

    m_stickyWindow = new StickyNoteWindow(m_database);
    connect(m_stickyWindow, &StickyNoteWindow::noteSaved, this,
            [this](qint64 noteId) {
                m_noteCache.remove(noteId);
                refreshNotes(noteId == m_currentNoteId ? noteId : m_currentNoteId);
            });
    buildTray();
    restoreWindowState();

    m_globalHotkey = new GlobalHotkey(winId(), [this] { summonSticky(); }, this);
    if (!m_globalHotkey->isRegistered()) {
        setStatusMessage(QStringLiteral("Ctrl+Alt+N 已被其他程序占用；仍可用界面按钮呼出便签"), true);
    }

    refreshFolders();
    ensureFirstNote();
    refreshNotes();
    refreshTodos();
}

MainWindow::~MainWindow()
{
    saveCurrentNote(true);
    if (m_stickyWindow)
        m_stickyWindow->flushSave();

    QSettings settings;
    settings.setValue(QStringLiteral("main/geometry"), saveGeometry());
}

void MainWindow::buildUi()
{
    auto* central = new QWidget(this);
    auto* rootLayout = new QHBoxLayout(central);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    auto* navigation = new QFrame(central);
    navigation->setObjectName(QStringLiteral("navigation"));
    navigation->setFixedWidth(270);
    auto* navigationLayout = new QVBoxLayout(navigation);
    navigationLayout->setContentsMargins(20, 20, 20, 18);
    navigationLayout->setSpacing(12);

    auto* brandRow = new QHBoxLayout;
    auto* brandIcon = new QLabel(QStringLiteral("羽"), navigation);
    brandIcon->setObjectName(QStringLiteral("brandIcon"));
    brandIcon->setAlignment(Qt::AlignCenter);
    brandIcon->setFixedSize(38, 38);
    auto* brandText = new QLabel(QStringLiteral("FeatherNote\n<small>轻羽笔记</small>"), navigation);
    brandText->setObjectName(QStringLiteral("brandText"));
    brandText->setTextFormat(Qt::RichText);
    brandRow->addWidget(brandIcon);
    brandRow->addWidget(brandText, 1);
    navigationLayout->addLayout(brandRow);

    m_searchEdit = new QLineEdit(navigation);
    m_searchEdit->setObjectName(QStringLiteral("searchEdit"));
    m_searchEdit->setPlaceholderText(QStringLiteral("搜索标题和正文…"));
    m_searchEdit->setClearButtonEnabled(true);
    navigationLayout->addWidget(m_searchEdit);

    auto* folderFilterRow = new QHBoxLayout;
    folderFilterRow->setSpacing(7);
    m_folderFilter = new QComboBox(navigation);
    m_folderFilter->setObjectName(QStringLiteral("folderFilter"));
    m_folderFilter->setToolTip(QStringLiteral("按分组筛选笔记"));
    m_folderManageButton = new QPushButton(QStringLiteral("管理"), navigation);
    m_folderManageButton->setObjectName(QStringLiteral("quietButton"));
    m_folderManageButton->setToolTip(QStringLiteral("新建、重命名或删除分组"));
    folderFilterRow->addWidget(m_folderFilter, 1);
    folderFilterRow->addWidget(m_folderManageButton);
    navigationLayout->addLayout(folderFilterRow);

    auto* noteButtonRow = new QHBoxLayout;
    noteButtonRow->setSpacing(8);
    m_newNoteButton = new QPushButton(QStringLiteral("＋ 新建"), navigation);
    m_newNoteButton->setObjectName(QStringLiteral("primaryButton"));
    m_stickyButton = new QPushButton(QStringLiteral("便签"), navigation);
    m_stickyButton->setToolTip(QStringLiteral("全局快捷键：Ctrl+Alt+N"));
    noteButtonRow->addWidget(m_newNoteButton, 1);
    noteButtonRow->addWidget(m_stickyButton);
    navigationLayout->addLayout(noteButtonRow);

    auto* notesCaption = new QLabel(QStringLiteral("我的笔记"), navigation);
    notesCaption->setObjectName(QStringLiteral("sectionCaption"));
    navigationLayout->addWidget(notesCaption);

    m_noteList = new QListWidget(navigation);
    m_noteList->setObjectName(QStringLiteral("noteList"));
    m_noteList->setFrameShape(QFrame::NoFrame);
    m_noteList->setSpacing(4);
    m_noteList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_noteList->setContextMenuPolicy(Qt::CustomContextMenu);
    navigationLayout->addWidget(m_noteList, 1);

    m_noteCountLabel = new QLabel(navigation);
    m_noteCountLabel->setObjectName(QStringLiteral("mutedLabel"));
    navigationLayout->addWidget(m_noteCountLabel);

    auto* editorPane = new QFrame(central);
    editorPane->setObjectName(QStringLiteral("editorPane"));
    auto* editorLayout = new QVBoxLayout(editorPane);
    editorLayout->setContentsMargins(28, 18, 22, 14);
    editorLayout->setSpacing(10);

    m_titleEdit = new QLineEdit(editorPane);
    m_titleEdit->setObjectName(QStringLiteral("titleEdit"));
    m_titleEdit->setPlaceholderText(QStringLiteral("无标题笔记"));
    editorLayout->addWidget(m_titleEdit);

    auto* separator = new QFrame(editorPane);
    separator->setObjectName(QStringLiteral("separator"));
    separator->setFrameShape(QFrame::HLine);
    separator->setFixedHeight(1);
    editorLayout->addWidget(separator);

    auto* toolbar = new QFrame(editorPane);
    toolbar->setObjectName(QStringLiteral("formatBar"));
    auto* toolbarLayout = new QHBoxLayout(toolbar);
    toolbarLayout->setContentsMargins(4, 2, 4, 2);
    toolbarLayout->setSpacing(3);

    auto makeToolButton = [toolbar, toolbarLayout](const QString& text, const QString& tooltip,
                                                   bool checkable = false) {
        auto* button = new QToolButton(toolbar);
        button->setText(text);
        button->setToolTip(tooltip);
        button->setCheckable(checkable);
        button->setAutoRaise(true);
        toolbarLayout->addWidget(button);
        return button;
    };

    m_boldButton = makeToolButton(QStringLiteral("B"), QStringLiteral("粗体 (Ctrl+B)"), true);
    QFont boldFont = m_boldButton->font();
    boldFont.setBold(true);
    m_boldButton->setFont(boldFont);
    m_italicButton = makeToolButton(QStringLiteral("I"), QStringLiteral("斜体 (Ctrl+I)"), true);
    QFont italicFont = m_italicButton->font();
    italicFont.setItalic(true);
    m_italicButton->setFont(italicFont);
    m_underlineButton = makeToolButton(QStringLiteral("U"), QStringLiteral("下划线 (Ctrl+U)"), true);
    QFont underlineFont = m_underlineButton->font();
    underlineFont.setUnderline(true);
    m_underlineButton->setFont(underlineFont);

    auto* headingButton = makeToolButton(QStringLiteral("H1"), QStringLiteral("一级标题"));
    auto* bodyButton = makeToolButton(QStringLiteral("正文"), QStringLiteral("恢复正文样式"));
    auto* bulletButton = makeToolButton(QStringLiteral("• 列表"), QStringLiteral("项目符号列表"));
    auto* numberedButton = makeToolButton(QStringLiteral("1. 列表"), QStringLiteral("编号列表"));
    auto* colorButton = makeToolButton(QStringLiteral("A"), QStringLiteral("文字颜色"));

    m_fontSizeCombo = new QComboBox(toolbar);
    m_fontSizeCombo->setToolTip(QStringLiteral("字号"));
    m_fontSizeCombo->setEditable(true);
    m_fontSizeCombo->setInsertPolicy(QComboBox::NoInsert);
    m_fontSizeCombo->addItems({QStringLiteral("10"), QStringLiteral("12"), QStringLiteral("14"),
                               QStringLiteral("16"), QStringLiteral("20"), QStringLiteral("24"),
                               QStringLiteral("32")});
    m_fontSizeCombo->setCurrentText(QStringLiteral("12"));
    m_fontSizeCombo->setFixedWidth(68);
    toolbarLayout->addWidget(m_fontSizeCombo);

    auto* folderLabel = new QLabel(QStringLiteral("分组"), toolbar);
    folderLabel->setObjectName(QStringLiteral("mutedLabel"));
    toolbarLayout->addWidget(folderLabel);
    m_noteFolderCombo = new QComboBox(toolbar);
    m_noteFolderCombo->setObjectName(QStringLiteral("noteFolderCombo"));
    m_noteFolderCombo->setToolTip(QStringLiteral("把当前笔记移动到分组"));
    m_noteFolderCombo->setMinimumWidth(118);
    m_noteFolderCombo->setMaximumWidth(170);
    toolbarLayout->addWidget(m_noteFolderCombo);

    toolbarLayout->addStretch(1);
    auto* imageButton = makeToolButton(QStringLiteral("＋ 图片"), QStringLiteral("插入图片，也可直接拖入或粘贴"));
    imageButton->setObjectName(QStringLiteral("imageButton"));
    editorLayout->addWidget(toolbar);

    m_editor = new NoteEditor(editorPane);
    m_editor->setObjectName(QStringLiteral("noteEditor"));
    m_editor->setFrameShape(QFrame::NoFrame);
    editorLayout->addWidget(m_editor, 1);

    m_saveStateLabel = new QLabel(QStringLiteral("就绪"), editorPane);
    m_saveStateLabel->setObjectName(QStringLiteral("saveState"));
    m_saveStateLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    editorLayout->addWidget(m_saveStateLabel);

    auto* todoPane = new QFrame(central);
    todoPane->setObjectName(QStringLiteral("todoPane"));
    todoPane->setFixedWidth(300);
    auto* todoLayout = new QVBoxLayout(todoPane);
    todoLayout->setContentsMargins(20, 24, 20, 18);
    todoLayout->setSpacing(10);

    auto* todoTitle = new QLabel(QStringLiteral("今日待办"), todoPane);
    todoTitle->setObjectName(QStringLiteral("panelTitle"));
    todoLayout->addWidget(todoTitle);
    auto* todoHint = new QLabel(QStringLiteral("把想法变成下一步行动"), todoPane);
    todoHint->setObjectName(QStringLiteral("mutedLabel"));
    todoLayout->addWidget(todoHint);

    auto* todoInputRow = new QHBoxLayout;
    todoInputRow->setSpacing(7);
    m_todoInput = new QLineEdit(todoPane);
    m_todoInput->setObjectName(QStringLiteral("todoInput"));
    m_todoInput->setPlaceholderText(QStringLiteral("添加一项待办…"));
    auto* addTodoButton = new QPushButton(QStringLiteral("＋"), todoPane);
    addTodoButton->setObjectName(QStringLiteral("roundButton"));
    addTodoButton->setFixedWidth(38);
    todoInputRow->addWidget(m_todoInput, 1);
    todoInputRow->addWidget(addTodoButton);
    todoLayout->addLayout(todoInputRow);

    m_todoList = new QListWidget(todoPane);
    m_todoList->setObjectName(QStringLiteral("todoList"));
    m_todoList->setFrameShape(QFrame::NoFrame);
    m_todoList->setSpacing(5);
    todoLayout->addWidget(m_todoList, 1);

    m_todoSummaryLabel = new QLabel(todoPane);
    m_todoSummaryLabel->setObjectName(QStringLiteral("mutedLabel"));
    todoLayout->addWidget(m_todoSummaryLabel);
    auto* clearTodoButton = new QPushButton(QStringLiteral("清理已完成"), todoPane);
    clearTodoButton->setObjectName(QStringLiteral("quietButton"));
    todoLayout->addWidget(clearTodoButton);

    rootLayout->addWidget(navigation);
    rootLayout->addWidget(editorPane, 1);
    rootLayout->addWidget(todoPane);
    setCentralWidget(central);

    m_saveTimer = new QTimer(this);
    m_saveTimer->setSingleShot(true);
    m_saveTimer->setInterval(kAutoSaveDelayMs);
    m_searchTimer = new QTimer(this);
    m_searchTimer->setSingleShot(true);
    m_searchTimer->setInterval(kSearchDelayMs);

    connect(headingButton, &QToolButton::clicked, this, &MainWindow::applyHeading);
    connect(bodyButton, &QToolButton::clicked, this, &MainWindow::applyBodyStyle);
    connect(bulletButton, &QToolButton::clicked, this, [this] { toggleList(false); });
    connect(numberedButton, &QToolButton::clicked, this, [this] { toggleList(true); });
    connect(colorButton, &QToolButton::clicked, this, &MainWindow::pickTextColor);
    connect(imageButton, &QToolButton::clicked, this, &MainWindow::chooseImages);
    connect(addTodoButton, &QPushButton::clicked, this, &MainWindow::addTodo);
    connect(clearTodoButton, &QPushButton::clicked, this, &MainWindow::clearCompletedTodos);
}

void MainWindow::buildMenus()
{
    auto* fileMenu = menuBar()->addMenu(QStringLiteral("文件(&F)"));
    QAction* newAction = fileMenu->addAction(QStringLiteral("新建笔记"));
    newAction->setShortcut(QKeySequence::New);
    QAction* importAction = fileMenu->addAction(QStringLiteral("导入文档…"));
    importAction->setShortcut(QKeySequence::Open);
    QAction* exportAction = fileMenu->addAction(QStringLiteral("导出当前笔记…"));
    exportAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+S")));
    fileMenu->addSeparator();
    QAction* renameAction = fileMenu->addAction(QStringLiteral("重命名当前笔记"));
    renameAction->setShortcut(QKeySequence(Qt::Key_F2));
    QAction* deleteAction = fileMenu->addAction(QStringLiteral("移到回收站"));
    deleteAction->setShortcut(QKeySequence::Delete);
    fileMenu->addSeparator();
    QAction* quitAction = fileMenu->addAction(QStringLiteral("退出 FeatherNote"));
    quitAction->setShortcut(QKeySequence::Quit);

    auto* insertMenu = menuBar()->addMenu(QStringLiteral("插入(&I)"));
    QAction* imageAction = insertMenu->addAction(QStringLiteral("图片…"));
    imageAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+I")));
    QAction* stickyAction = insertMenu->addAction(QStringLiteral("呼出快速便签"));
    stickyAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+N")));

    auto* manageMenu = menuBar()->addMenu(QStringLiteral("管理(&M)"));
    QAction* newFolderAction = manageMenu->addAction(QStringLiteral("新建分组…"));
    newFolderAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+G")));
    QAction* renameFolderAction = manageMenu->addAction(QStringLiteral("重命名当前分组…"));
    QAction* deleteFolderAction = manageMenu->addAction(QStringLiteral("删除当前分组…"));

    auto* helpMenu = menuBar()->addMenu(QStringLiteral("帮助(&H)"));
    QAction* shortcutHelp = helpMenu->addAction(QStringLiteral("快捷键说明"));
    connect(shortcutHelp, &QAction::triggered, this, [this] {
        QMessageBox::information(this,
                                 QStringLiteral("FeatherNote 快捷键"),
                                 QStringLiteral("Ctrl+Alt+N　全局呼出快速便签\n"
                                                "Ctrl+Shift+N　窗口内呼出快速便签\n"
                                                "Ctrl+N　　　新建笔记\n"
                                                "Ctrl+O　　　导入文档\n"
                                                "Ctrl+Shift+I　插入图片\n"
                                                "Ctrl+Shift+G　新建分组\n"
                                                "F2　　　　　重命名当前笔记\n"
                                                "Ctrl+B / I / U　文字格式\n"
                                                "Ctrl+Shift+S　导出当前笔记"));
    });

    connect(newAction, &QAction::triggered, this, &MainWindow::createNote);
    connect(importAction, &QAction::triggered, this, &MainWindow::importDocument);
    connect(exportAction, &QAction::triggered, this, &MainWindow::exportDocument);
    connect(renameAction, &QAction::triggered, this, &MainWindow::renameCurrentNote);
    connect(deleteAction, &QAction::triggered, this, &MainWindow::deleteCurrentNote);
    connect(quitAction, &QAction::triggered, this, &MainWindow::requestQuit);
    connect(imageAction, &QAction::triggered, this, &MainWindow::chooseImages);
    connect(stickyAction, &QAction::triggered, this, &MainWindow::summonSticky);
    connect(newFolderAction, &QAction::triggered, this, &MainWindow::createFolder);
    connect(renameFolderAction, &QAction::triggered, this, &MainWindow::renameSelectedFolder);
    connect(deleteFolderAction, &QAction::triggered, this, &MainWindow::deleteSelectedFolder);
}

void MainWindow::buildTray()
{
    if (!QSystemTrayIcon::isSystemTrayAvailable())
        return;

    m_trayIcon = new QSystemTrayIcon(createAppIcon(), this);
    m_trayIcon->setToolTip(QStringLiteral("FeatherNote · Ctrl+Alt+N 快速便签"));
    m_trayMenu = new QMenu(this);
    QAction* openAction = m_trayMenu->addAction(QStringLiteral("打开 FeatherNote"));
    QAction* stickyAction = m_trayMenu->addAction(QStringLiteral("新建快速便签　Ctrl+Alt+N"));
    m_trayMenu->addSeparator();
    QAction* quitAction = m_trayMenu->addAction(QStringLiteral("退出"));
    m_trayIcon->setContextMenu(m_trayMenu);

    connect(openAction, &QAction::triggered, this, &MainWindow::showMainWindow);
    connect(stickyAction, &QAction::triggered, this, &MainWindow::summonSticky);
    connect(quitAction, &QAction::triggered, this, &MainWindow::requestQuit);
    connect(m_trayIcon, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick)
                    showMainWindow();
            });
    m_trayIcon->show();
}

void MainWindow::applyTheme()
{
    setStyleSheet(QStringLiteral(R"(
        QMainWindow#mainWindow { background: #F7F5EF; }
        QMenuBar { background: #F7F5EF; color: #2C332F; padding: 3px 8px; }
        QMenuBar::item:selected { background: #E7ECE7; border-radius: 5px; }
        QMenu { background: #FFFDF9; color: #2C332F;
                border: 1px solid #D9DED8; padding: 6px; }
        QMenu::item { color: #2C332F; padding: 7px 28px 7px 12px; border-radius: 4px; }
        QMenu::item:selected { background: #E4ECE6; color: #213129; }
        QMenu::item:disabled { color: #9AA19D; }
        QMenu::separator { height: 1px; background: #D6D9D5; margin: 5px 8px; }
        QFrame#navigation { background: #EDEAE1; border-right: 1px solid #DCD8CF; }
        QLabel#brandIcon { background: #4F6B5B; color: white; border-radius: 10px;
                           font-size: 19px; font-weight: 700; }
        QLabel#brandText { color: #26322B; font-size: 15px; font-weight: 700; }
        QLabel#sectionCaption { color: #4D574F; font-size: 12px; font-weight: 700; margin-top: 5px; }
        QLabel#mutedLabel, QLabel#saveState { color: #7C847F; font-size: 11px; }
        QLabel#panelTitle { color: #29342E; font-size: 20px; font-weight: 700; }
        QLineEdit { background: #FFFDF9; color: #252C28; border: 1px solid #D7DCD6;
                    border-radius: 8px; padding: 8px 10px; selection-background-color: #70917D; }
        QLineEdit:focus { border: 1px solid #6F8F7C; }
        QLineEdit#searchEdit { background: rgba(255,255,255,0.68); }
        QLineEdit#titleEdit { background: transparent; border: 0; border-radius: 0;
                              padding: 4px 0; font-size: 24px; font-weight: 650; color: #232A26; }
        QPushButton { background: #FFFDF9; color: #344039; border: 1px solid #D2D8D2;
                      border-radius: 8px; padding: 7px 11px; }
        QPushButton:hover { background: #F5F8F5; border-color: #AAB8AF; }
        QPushButton:pressed { background: #E7EEE9; }
        QPushButton#primaryButton { background: #506E5D; color: white; border: 0; font-weight: 650; }
        QPushButton#primaryButton:hover { background: #466454; }
        QPushButton#roundButton { background: #506E5D; color: white; border: 0; font-size: 18px; }
        QPushButton#quietButton { background: transparent; border: 0; color: #718078; text-align: left; }
        QPushButton#quietButton:hover { color: #3F5E4D; }
        QListWidget#noteList { background: transparent; color: #303934; outline: none; }
        QListWidget#noteList::item { background: transparent; border-radius: 9px; padding: 8px 10px; }
        QListWidget#noteList::item:selected { background: #D8E2DB; color: #213129; }
        QListWidget#noteList::item:hover:!selected { background: rgba(255,255,255,0.55); }
        QFrame#editorPane { background: #FFFDF9; }
        QFrame#todoPane { background: #F3F1EA; border-left: 1px solid #E1DED6; }
        QFrame#separator { background: #E8E5DE; border: 0; }
        QFrame#formatBar { background: #F7F6F1; border: 1px solid #E7E5DE; border-radius: 9px; }
        QToolButton { color: #4B5650; border: 0; border-radius: 6px; padding: 6px 8px; }
        QToolButton:hover { background: #E7ECE8; }
        QToolButton:checked { background: #D5E1D9; color: #2F5440; }
        QToolButton#imageButton { background: #E2EBE5; color: #355744; font-weight: 600; }
        QComboBox { background: #FFFDF9; border: 1px solid #D9DED9; border-radius: 6px;
                    padding: 4px 7px; color: #3B4640; }
        QTextEdit#noteEditor { background: #FFFDF9; color: #242B27; padding: 8px 4px;
                               selection-background-color: #87A794; font-size: 12pt; }
        QListWidget#todoList { background: transparent; color: #313A35; outline: none; }
        QListWidget#todoList::item { background: #FFFDF9; border: 1px solid #E3E1DA;
                                     border-radius: 8px; padding: 8px 8px; }
        QListWidget#todoList::item:hover { border-color: #BCC9C0; }
        QScrollBar:vertical { background: transparent; width: 10px; margin: 2px; }
        QScrollBar::handle:vertical { background: #C7CCC8; min-height: 28px; border-radius: 4px; }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
    )"));
}

void MainWindow::connectSignals()
{
    connect(m_newNoteButton, &QPushButton::clicked, this, &MainWindow::createNote);
    connect(m_stickyButton, &QPushButton::clicked, this, &MainWindow::summonSticky);
    connect(m_saveTimer, &QTimer::timeout, this, [this] { saveCurrentNote(); });
    connect(m_searchTimer, &QTimer::timeout, this, [this] { refreshNotes(m_currentNoteId); });

    connect(m_searchEdit, &QLineEdit::textChanged, this, [this] { m_searchTimer->start(); });
    connect(m_folderFilter, &QComboBox::currentIndexChanged, this, [this] {
        if (!m_loadingFolders)
            refreshNotes(m_currentNoteId);
    });
    connect(m_noteFolderCombo, &QComboBox::currentIndexChanged,
            this, &MainWindow::moveCurrentNoteToSelectedFolder);
    connect(m_folderManageButton, &QPushButton::clicked, this, [this] {
        QMenu menu(this);
        QAction* createAction = menu.addAction(QStringLiteral("新建分组…"));
        QAction* renameAction = menu.addAction(QStringLiteral("重命名当前分组…"));
        QAction* deleteAction = menu.addAction(QStringLiteral("删除当前分组…"));
        const qint64 folderId = selectedFolderFilter() > 0
            ? selectedFolderFilter()
            : m_noteFolderCombo->currentData().toLongLong();
        renameAction->setEnabled(folderId > 0);
        deleteAction->setEnabled(folderId > 0);
        QAction* selected = menu.exec(
            m_folderManageButton->mapToGlobal(QPoint(0, m_folderManageButton->height())));
        if (selected == createAction)
            createFolder();
        else if (selected == renameAction)
            renameSelectedFolder();
        else if (selected == deleteAction)
            deleteSelectedFolder();
    });
    connect(m_noteList, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem* current, QListWidgetItem*) {
                if (!current)
                    return;
                const qint64 id = current->data(Qt::UserRole).toLongLong();
                if (id != m_currentNoteId) {
                    saveCurrentNote();
                    loadNote(id);
                }
            });
    connect(m_noteList, &QListWidget::customContextMenuRequested,
            this, &MainWindow::showNoteContextMenu);

    connect(m_titleEdit, &QLineEdit::textChanged, this, [this] { scheduleSave(); });
    connect(m_editor, &QTextEdit::textChanged, this, [this] { scheduleSave(); });
    connect(m_editor, &QTextEdit::currentCharFormatChanged, this,
            [this](const QTextCharFormat&) { updateFormatControls(); });
    connect(m_editor, &QTextEdit::cursorPositionChanged, this, &MainWindow::updateFormatControls);
    connect(m_editor, &NoteEditor::imagePasted, this,
            [this](const QImage& image) { insertImage(image, QStringLiteral("clipboard")); });
    connect(m_editor, &NoteEditor::imageFilesDropped, this, &MainWindow::insertImagesFromFiles);

    connect(m_boldButton, &QToolButton::clicked, this, &MainWindow::toggleBold);
    connect(m_italicButton, &QToolButton::clicked, this, &MainWindow::toggleItalic);
    connect(m_underlineButton, &QToolButton::clicked, this, &MainWindow::toggleUnderline);
    connect(m_fontSizeCombo, &QComboBox::currentTextChanged, this, &MainWindow::applyFontSize);

    connect(m_todoInput, &QLineEdit::returnPressed, this, &MainWindow::addTodo);
    connect(m_todoList, &QListWidget::itemChanged, this, [this](QListWidgetItem* item) {
        if (m_loadingTodos || !item)
            return;
        QString error;
        const qint64 id = item->data(Qt::UserRole).toLongLong();
        const bool done = item->checkState() == Qt::Checked;
        if (!m_database->updateTodoDone(id, done, &error)) {
            setStatusMessage(databaseErrorText(QStringLiteral("待办保存失败"), error), true);
            return;
        }
        QFont font = item->font();
        font.setStrikeOut(done);
        item->setFont(font);
        refreshTodos();
    });
}

void MainWindow::restoreWindowState()
{
    QSettings settings;
    const QByteArray geometry = settings.value(QStringLiteral("main/geometry")).toByteArray();
    if (!geometry.isEmpty())
        restoreGeometry(geometry);
}

void MainWindow::ensureFirstNote()
{
    QString error;
    if (!m_database->listNoteSummaries(QString(), &error).isEmpty())
        return;
    if (!error.isEmpty()) {
        setStatusMessage(databaseErrorText(QStringLiteral("读取笔记失败"), error), true);
        return;
    }

    const QString html = QStringLiteral(
        "<h1>欢迎使用 FeatherNote</h1>"
        "<p>这是第一版 <b>C++ 原生桌面原型</b>。它把常用记录能力收在一个很轻的工作流里：</p>"
        "<ul><li>正文支持富文本、列表，以及图片拖放和粘贴；</li>"
        "<li>右侧可以快速维护日常待办；</li>"
        "<li>按 <b>Ctrl+Alt+N</b>，随时呼出置顶便签；</li>"
        "<li>输入会在短暂停顿后自动保存到本机 SQLite。</li></ul>"
        "<p>现在就删掉这段文字，记下你的第一个游戏灵感吧。</p>");
    const QString plain = QStringLiteral(
        "欢迎使用 FeatherNote\n这是第一版 C++ 原生桌面原型。\n"
        "正文支持富文本、列表、图片拖放和粘贴；右侧可维护待办；Ctrl+Alt+N 呼出便签。\n");
    m_database->createNote(QStringLiteral("欢迎使用 FeatherNote"), html, plain, &error);
    if (!error.isEmpty())
        setStatusMessage(databaseErrorText(QStringLiteral("创建欢迎笔记失败"), error), true);
}

void MainWindow::refreshNotes(qint64 preferredId)
{
    if (m_dirty && !saveCurrentNote())
        return;

    QString error;
    const QList<NoteSummary> notes = m_database->listNoteSummaries(
        m_searchEdit->text().trimmed(), &error, selectedFolderFilter());
    if (!error.isEmpty()) {
        setStatusMessage(databaseErrorText(QStringLiteral("读取笔记失败"), error), true);
        return;
    }

    QSignalBlocker blocker(m_noteList);
    m_noteList->clear();
    int selectedRow = -1;
    for (int index = 0; index < notes.size(); ++index) {
        const NoteSummary& note = notes.at(index);
        auto* item = new QListWidgetItem(
            noteListText(note.title, note.excerpt, note.kind, note.updatedAt), m_noteList);
        item->setData(Qt::UserRole, note.id);
        item->setData(Qt::UserRole + 1, note.bodyRevision);
        item->setData(Qt::UserRole + 2, note.contentHash);
        item->setData(Qt::UserRole + 3, note.folderId);
        item->setData(Qt::UserRole + 4, note.kind);
        item->setSizeHint(QSize(0, 76));
        if (note.id == preferredId || (preferredId < 0 && note.id == m_currentNoteId))
            selectedRow = index;
    }
    m_noteCountLabel->setText(QStringLiteral("%1 篇 · 摘要加载 · 本地自动保存").arg(notes.size()));

    if (notes.isEmpty()) {
        m_currentNoteId = -1;
        m_currentFolderId = Database::UnfiledFolder;
        m_currentNoteKind = QStringLiteral("note");
        m_currentBodyRevision = 0;
        m_currentContentHash.clear();
        m_currentCreatedAt = {};
        m_loadingNote = true;
        m_titleEdit->clear();
        m_editor->setReadOnly(false);
        m_editor->clear();
        selectCurrentFolderInEditor();
        m_loadingNote = false;
        return;
    }

    if (selectedRow < 0)
        selectedRow = 0;
    m_noteList->setCurrentRow(selectedRow);
    const qint64 selectedId = m_noteList->currentItem()->data(Qt::UserRole).toLongLong();
    const int selectedRevision = m_noteList->currentItem()->data(Qt::UserRole + 1).toInt();
    if (selectedId != m_currentNoteId || selectedRevision != m_currentBodyRevision)
        loadNote(selectedId);
}

void MainWindow::loadNote(qint64 noteId)
{
    NoteRecord loaded;
    bool cacheHit = false;
    const int expectedRevision = summaryRevision(noteId);
    if (const NoteRecord* cached = m_noteCache.object(noteId);
        cached && (expectedRevision <= 0 || cached->bodyRevision == expectedRevision)) {
        loaded = *cached;
        cacheHit = true;
    } else {
        QString error;
        const std::optional<NoteRecord> note = m_database->note(noteId, &error);
        if (!note.has_value()) {
            setStatusMessage(databaseErrorText(QStringLiteral("无法打开笔记"), error), true);
            return;
        }
        loaded = *note;
        cacheNote(loaded);
    }

    m_loadingNote = true;
    m_saveTimer->stop();
    m_currentNoteId = noteId;
    m_currentFolderId = loaded.folderId;
    m_currentNoteKind = loaded.kind;
    m_currentBodyRevision = loaded.bodyRevision;
    m_currentContentHash = loaded.contentHash;
    m_currentCreatedAt = loaded.createdAt;
    m_titleEdit->setText(loaded.title);
    selectCurrentFolderInEditor();
    m_editor->setHtml(loaded.html);
    m_editor->setReadOnly(loaded.kind == QStringLiteral("sticky"));
    m_editor->document()->setModified(false);
    m_dirty = false;
    m_loadingNote = false;
    setStatusMessage(QStringLiteral("%1 · %2")
                         .arg(cacheHit ? QStringLiteral("已从缓存载入") : QStringLiteral("已按需载入"),
                              loaded.updatedAt.toLocalTime().toString(QStringLiteral("MM-dd HH:mm"))));
    updateFormatControls();
}

bool MainWindow::saveCurrentNote(bool force)
{
    Q_UNUSED(force)
    if (m_currentNoteId < 0 || m_loadingNote)
        return true;
    if (!m_dirty)
        return true;

    QString title = m_titleEdit->text().trimmed();
    if (title.isEmpty())
        title = QStringLiteral("无标题笔记");

    const QString html = m_editor->toHtml();
    const QString plainText = m_editor->toPlainText();
    const QByteArray hash = htmlHash(html);
    const bool bodyChanged = hash != m_currentContentHash;
    const bool stickyMetadataOnly = m_currentNoteKind == QStringLiteral("sticky")
        && m_editor->isReadOnly();
    const bool metadataOnly = stickyMetadataOnly || !bodyChanged;

    QString error;
    const bool saved = metadataOnly
        ? m_database->renameNote(m_currentNoteId, title, &error)
        : m_database->updateNote(m_currentNoteId, title, html, plainText, &error);
    if (!saved) {
        setStatusMessage(databaseErrorText(QStringLiteral("自动保存失败"), error), true);
        m_saveTimer->start(1600);
        return false;
    }

    m_dirty = false;
    if (!metadataOnly)
        ++m_currentBodyRevision;
    if (!stickyMetadataOnly)
        m_currentContentHash = hash;
    m_editor->document()->setModified(false);
    const QDateTime updatedAt = QDateTime::currentDateTimeUtc();

    NoteRecord cached;
    cached.id = m_currentNoteId;
    cached.folderId = m_currentFolderId;
    cached.title = title;
    cached.html = html;
    cached.plainText = plainText;
    cached.excerpt = plainTextExcerpt(plainText);
    cached.kind = m_currentNoteKind;
    cached.bodyRevision = m_currentBodyRevision;
    cached.contentHash = m_currentContentHash;
    cached.createdAt = m_currentCreatedAt;
    cached.updatedAt = updatedAt;
    if (stickyMetadataOnly) {
        m_noteCache.remove(m_currentNoteId);
        QString reloadError;
        if (const std::optional<NoteRecord> latest = m_database->note(m_currentNoteId, &reloadError);
            latest.has_value()) {
            cached = *latest;
            m_currentBodyRevision = latest->bodyRevision;
            m_currentContentHash = latest->contentHash;
            cacheNote(*latest);
        }
    } else {
        cacheNote(cached);
    }

    setStatusMessage(QStringLiteral("已自动保存 · %1")
                         .arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss"))));

    for (int row = 0; row < m_noteList->count(); ++row) {
        QListWidgetItem* item = m_noteList->item(row);
        if (item->data(Qt::UserRole).toLongLong() == m_currentNoteId) {
            item->setText(noteListText(title, cached.excerpt, cached.kind, updatedAt));
            item->setData(Qt::UserRole + 1, cached.bodyRevision);
            item->setData(Qt::UserRole + 2, cached.contentHash);
            item->setData(Qt::UserRole + 3, cached.folderId);
            item->setData(Qt::UserRole + 4, cached.kind);
            break;
        }
    }
    return true;
}

void MainWindow::scheduleSave()
{
    if (m_loadingNote || m_currentNoteId < 0)
        return;
    m_dirty = true;
    m_saveStateLabel->setStyleSheet(QString());
    m_saveStateLabel->setText(QStringLiteral("正在编辑…"));
    m_saveTimer->start();
}

void MainWindow::createNote()
{
    saveCurrentNote(true);
    QString error;
    const qint64 filterFolder = selectedFolderFilter();
    const qint64 destinationFolder = filterFolder > 0
        ? filterFolder
        : Database::UnfiledFolder;
    const qint64 id = m_database->createNote(QStringLiteral("新笔记"),
                                             QStringLiteral("<p><br></p>"),
                                             QString(),
                                             &error,
                                             destinationFolder);
    if (id <= 0) {
        setStatusMessage(databaseErrorText(QStringLiteral("新建笔记失败"), error), true);
        return;
    }
    m_searchEdit->clear();
    refreshNotes(id);
    m_titleEdit->selectAll();
    m_titleEdit->setFocus();
}

void MainWindow::renameCurrentNote()
{
    if (m_currentNoteId < 0)
        return;
    m_titleEdit->setFocus(Qt::ShortcutFocusReason);
    m_titleEdit->selectAll();
    setStatusMessage(QStringLiteral("输入新名称后会自动保存"));
}

void MainWindow::showNoteContextMenu(const QPoint& position)
{
    QListWidgetItem* item = m_noteList->itemAt(position);
    if (!item)
        return;
    m_noteList->setCurrentItem(item);

    QMenu menu(this);
    QAction* renameAction = menu.addAction(QStringLiteral("重命名"));
    QAction* deleteAction = menu.addAction(QStringLiteral("移到回收站"));
    QAction* selected = menu.exec(m_noteList->viewport()->mapToGlobal(position));
    if (selected == renameAction)
        renameCurrentNote();
    else if (selected == deleteAction)
        deleteCurrentNote();
}

void MainWindow::deleteCurrentNote()
{
    if (m_currentNoteId < 0)
        return;
    if (QMessageBox::question(this,
                              QStringLiteral("移到回收站"),
                              QStringLiteral("将当前笔记移到数据库回收站？\n原型暂未提供回收站界面。"),
                              QMessageBox::Yes | QMessageBox::Cancel,
                              QMessageBox::Cancel) != QMessageBox::Yes) {
        return;
    }

    QString error;
    if (!m_database->softDeleteNote(m_currentNoteId, &error)) {
        setStatusMessage(databaseErrorText(QStringLiteral("删除失败"), error), true);
        return;
    }
    m_noteCache.remove(m_currentNoteId);
    m_currentNoteId = -1;
    refreshNotes();
    if (m_noteList->count() == 0)
        createNote();
}

void MainWindow::importDocument()
{
    const QString path = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("导入文档"),
        QString(),
        QStringLiteral("支持的文档 (*.md *.markdown *.html *.htm *.txt);;Markdown (*.md *.markdown);;HTML (*.html *.htm);;纯文本 (*.txt)"));
    if (path.isEmpty())
        return;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        setStatusMessage(QStringLiteral("无法读取：%1").arg(file.errorString()), true);
        return;
    }
    const QByteArray data = file.readAll();
    const QString suffix = QFileInfo(path).suffix().toLower();

    saveCurrentNote(true);
    QString error;
    const qint64 id = m_database->createNote(QFileInfo(path).completeBaseName(),
                                             QStringLiteral("<p><br></p>"),
                                             QString(),
                                             &error,
                                             selectedFolderFilter() > 0
                                                 ? selectedFolderFilter()
                                                 : Database::UnfiledFolder);
    if (id <= 0) {
        setStatusMessage(databaseErrorText(QStringLiteral("导入失败"), error), true);
        return;
    }
    m_searchEdit->clear();
    refreshNotes(id);

    m_loadingNote = true;
    const QString text = QString::fromUtf8(data);
    if (suffix == QStringLiteral("md") || suffix == QStringLiteral("markdown"))
        m_editor->setMarkdown(text);
    else if (suffix == QStringLiteral("html") || suffix == QStringLiteral("htm"))
        m_editor->setHtml(text);
    else
        m_editor->setPlainText(text);
    m_titleEdit->setText(QFileInfo(path).completeBaseName());
    m_loadingNote = false;
    m_dirty = true;
    saveCurrentNote(true);
    setStatusMessage(QStringLiteral("已导入 %1").arg(QFileInfo(path).fileName()));
}

void MainWindow::exportDocument()
{
    if (m_currentNoteId < 0)
        return;
    saveCurrentNote(true);
    QString suggested = m_titleEdit->text().trimmed();
    if (suggested.isEmpty())
        suggested = QStringLiteral("FeatherNote-笔记");

    QString selectedFilter;
    QString path = QFileDialog::getSaveFileName(
        this,
        QStringLiteral("导出当前笔记"),
        suggested + QStringLiteral(".html"),
        QStringLiteral("HTML 网页 (*.html);;Markdown (*.md);;纯文本 (*.txt)"),
        &selectedFilter);
    if (path.isEmpty())
        return;

    QByteArray output;
    if (selectedFilter.startsWith(QStringLiteral("Markdown"))) {
        if (QFileInfo(path).suffix().isEmpty())
            path += QStringLiteral(".md");
        output = m_editor->toMarkdown().toUtf8();
    } else if (selectedFilter.startsWith(QStringLiteral("纯文本"))) {
        if (QFileInfo(path).suffix().isEmpty())
            path += QStringLiteral(".txt");
        output = m_editor->toPlainText().toUtf8();
    } else {
        if (QFileInfo(path).suffix().isEmpty())
            path += QStringLiteral(".html");
        output = m_editor->toHtml().toUtf8();
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(output) != output.size()) {
        setStatusMessage(QStringLiteral("导出失败：%1").arg(file.errorString()), true);
        return;
    }
    file.close();
    setStatusMessage(QStringLiteral("已导出到 %1").arg(QDir::toNativeSeparators(path)));
}

qint64 MainWindow::selectedFolderFilter() const
{
    if (!m_folderFilter || m_folderFilter->currentIndex() < 0)
        return Database::AllFolders;
    return m_folderFilter->currentData().toLongLong();
}

void MainWindow::refreshFolders(qint64 preferredFilter)
{
    QString error;
    const QList<FolderRecord> folders = m_database->listFolders(&error);
    if (!error.isEmpty()) {
        setStatusMessage(databaseErrorText(QStringLiteral("读取分组失败"), error), true);
        return;
    }

    m_loadingFolders = true;
    const QSignalBlocker filterBlocker(m_folderFilter);
    const QSignalBlocker noteFolderBlocker(m_noteFolderCombo);

    m_folderFilter->clear();
    m_folderFilter->addItem(QStringLiteral("全部笔记"), Database::AllFolders);
    m_folderFilter->addItem(QStringLiteral("未分组"), Database::UnfiledFolder);

    m_noteFolderCombo->clear();
    m_noteFolderCombo->addItem(QStringLiteral("未分组"), Database::UnfiledFolder);

    for (const FolderRecord& folder : folders) {
        m_folderFilter->addItem(folder.name, folder.id);
        m_noteFolderCombo->addItem(folder.name, folder.id);
    }

    int filterIndex = m_folderFilter->findData(preferredFilter);
    if (filterIndex < 0)
        filterIndex = 0;
    m_folderFilter->setCurrentIndex(filterIndex);
    selectCurrentFolderInEditor();
    m_loadingFolders = false;
}

void MainWindow::createFolder()
{
    bool accepted = false;
    const QString name = QInputDialog::getText(this,
                                               QStringLiteral("新建分组"),
                                               QStringLiteral("分组名称"),
                                               QLineEdit::Normal,
                                               QString(),
                                               &accepted).simplified();
    if (!accepted || name.isEmpty())
        return;

    QString error;
    const qint64 id = m_database->createFolder(name, &error);
    if (id <= 0) {
        setStatusMessage(databaseErrorText(QStringLiteral("新建分组失败"), error), true);
        return;
    }
    const qint64 filter = selectedFolderFilter();
    refreshFolders(filter);
    setStatusMessage(QStringLiteral("已创建分组“%1”").arg(name));
}

void MainWindow::renameSelectedFolder()
{
    const qint64 filter = selectedFolderFilter();
    const qint64 folderId = filter > 0
        ? filter
        : m_noteFolderCombo->currentData().toLongLong();
    if (folderId <= 0)
        return;

    const int filterIndex = m_folderFilter->findData(folderId);
    const int noteIndex = m_noteFolderCombo->findData(folderId);
    const QString currentName = filterIndex >= 0
        ? m_folderFilter->itemText(filterIndex)
        : m_noteFolderCombo->itemText(noteIndex);

    bool accepted = false;
    const QString name = QInputDialog::getText(this,
                                               QStringLiteral("重命名分组"),
                                               QStringLiteral("分组名称"),
                                               QLineEdit::Normal,
                                               currentName,
                                               &accepted).simplified();
    if (!accepted || name.isEmpty() || name == currentName)
        return;

    QString error;
    if (!m_database->renameFolder(folderId, name, &error)) {
        setStatusMessage(databaseErrorText(QStringLiteral("重命名分组失败"), error), true);
        return;
    }
    refreshFolders(filter);
    refreshNotes(m_currentNoteId);
    setStatusMessage(QStringLiteral("分组已重命名为“%1”").arg(name));
}

void MainWindow::deleteSelectedFolder()
{
    const qint64 filter = selectedFolderFilter();
    const qint64 folderId = filter > 0
        ? filter
        : m_noteFolderCombo->currentData().toLongLong();
    if (folderId <= 0)
        return;

    const int filterIndex = m_folderFilter->findData(folderId);
    const int noteIndex = m_noteFolderCombo->findData(folderId);
    const QString name = filterIndex >= 0
        ? m_folderFilter->itemText(filterIndex)
        : m_noteFolderCombo->itemText(noteIndex);
    if (QMessageBox::question(
            this,
            QStringLiteral("删除分组"),
            QStringLiteral("删除分组“%1”？\n其中的笔记会移到“未分组”，不会被删除。")
                .arg(name),
            QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel) != QMessageBox::Yes) {
        return;
    }

    QString error;
    if (!m_database->deleteFolder(folderId, &error)) {
        setStatusMessage(databaseErrorText(QStringLiteral("删除分组失败"), error), true);
        return;
    }
    if (m_currentFolderId == folderId)
        m_currentFolderId = Database::UnfiledFolder;
    m_noteCache.clear();
    refreshFolders(filter == folderId ? Database::UnfiledFolder : filter);
    refreshNotes(m_currentNoteId);
    setStatusMessage(QStringLiteral("分组已删除，笔记已移到“未分组”"));
}

void MainWindow::moveCurrentNoteToSelectedFolder()
{
    if (m_loadingFolders || m_loadingNote || m_currentNoteId < 0
        || m_noteFolderCombo->currentIndex() < 0) {
        return;
    }

    const qint64 folderId = m_noteFolderCombo->currentData().toLongLong();
    if (folderId == m_currentFolderId)
        return;
    if (!saveCurrentNote(true)) {
        selectCurrentFolderInEditor();
        return;
    }

    QString error;
    if (!m_database->moveNoteToFolder(m_currentNoteId, folderId, &error)) {
        selectCurrentFolderInEditor();
        setStatusMessage(databaseErrorText(QStringLiteral("移动笔记失败"), error), true);
        return;
    }

    m_currentFolderId = folderId;
    if (NoteRecord* cached = m_noteCache.object(m_currentNoteId))
        cached->folderId = folderId;
    refreshNotes(m_currentNoteId);
    setStatusMessage(folderId > 0 ? QStringLiteral("笔记已移动到所选分组")
                                  : QStringLiteral("笔记已移到“未分组”"));
}

void MainWindow::refreshTodos()
{
    QString error;
    const QList<TodoRecord> todos = m_database->listTodos(&error);
    if (!error.isEmpty()) {
        setStatusMessage(databaseErrorText(QStringLiteral("读取待办失败"), error), true);
        return;
    }

    m_loadingTodos = true;
    m_todoList->clear();
    int completed = 0;
    for (const TodoRecord& todo : todos) {
        auto* item = new QListWidgetItem(todo.text, m_todoList);
        item->setData(Qt::UserRole, todo.id);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(todo.done ? Qt::Checked : Qt::Unchecked);
        item->setSizeHint(QSize(0, 44));
        QFont font = item->font();
        font.setStrikeOut(todo.done);
        item->setFont(font);
        if (todo.done)
            ++completed;
    }
    m_loadingTodos = false;
    m_todoSummaryLabel->setText(QStringLiteral("%1 项 · 已完成 %2 项").arg(todos.size()).arg(completed));
}

void MainWindow::addTodo()
{
    const QString text = m_todoInput->text().trimmed();
    if (text.isEmpty())
        return;
    QString error;
    if (m_database->createTodo(text, &error) <= 0) {
        setStatusMessage(databaseErrorText(QStringLiteral("添加待办失败"), error), true);
        return;
    }
    m_todoInput->clear();
    refreshTodos();
}

void MainWindow::clearCompletedTodos()
{
    QString error;
    if (!m_database->deleteCompletedTodos(&error)) {
        setStatusMessage(databaseErrorText(QStringLiteral("清理待办失败"), error), true);
        return;
    }
    refreshTodos();
}

void MainWindow::toggleBold()
{
    QTextCharFormat format;
    format.setFontWeight(m_editor->fontWeight() == QFont::Bold ? QFont::Normal : QFont::Bold);
    m_editor->mergeCurrentCharFormat(format);
}

void MainWindow::toggleItalic()
{
    QTextCharFormat format;
    format.setFontItalic(!m_editor->fontItalic());
    m_editor->mergeCurrentCharFormat(format);
}

void MainWindow::toggleUnderline()
{
    QTextCharFormat format;
    format.setFontUnderline(!m_editor->fontUnderline());
    m_editor->mergeCurrentCharFormat(format);
}

void MainWindow::applyHeading()
{
    QTextCursor cursor = m_editor->textCursor();
    if (!cursor.hasSelection())
        cursor.select(QTextCursor::BlockUnderCursor);
    QTextCharFormat format;
    format.setFontPointSize(22);
    format.setFontWeight(QFont::Bold);
    cursor.mergeCharFormat(format);
    m_editor->setTextCursor(cursor);
}

void MainWindow::applyBodyStyle()
{
    QTextCursor cursor = m_editor->textCursor();
    if (!cursor.hasSelection())
        cursor.select(QTextCursor::BlockUnderCursor);
    QTextCharFormat format;
    format.setFontPointSize(12);
    format.setFontWeight(QFont::Normal);
    format.setFontItalic(false);
    format.setFontUnderline(false);
    cursor.mergeCharFormat(format);
    m_editor->setTextCursor(cursor);
}

void MainWindow::toggleList(bool numbered)
{
    QTextCursor cursor = m_editor->textCursor();
    cursor.beginEditBlock();
    if (cursor.currentList()) {
        QTextBlockFormat blockFormat = cursor.blockFormat();
        blockFormat.setObjectIndex(-1);
        cursor.setBlockFormat(blockFormat);
    } else {
        QTextListFormat listFormat;
        listFormat.setStyle(numbered ? QTextListFormat::ListDecimal : QTextListFormat::ListDisc);
        listFormat.setIndent(cursor.blockFormat().indent() + 1);
        cursor.createList(listFormat);
    }
    cursor.endEditBlock();
    m_editor->setTextCursor(cursor);
}

void MainWindow::applyFontSize(const QString& text)
{
    bool ok = false;
    const qreal size = text.toDouble(&ok);
    if (!ok || size < 6 || size > 96 || !m_fontSizeCombo->hasFocus())
        return;
    QTextCharFormat format;
    format.setFontPointSize(size);
    m_editor->mergeCurrentCharFormat(format);
    m_editor->setFocus();
}

void MainWindow::pickTextColor()
{
    const QColor selected = QColorDialog::getColor(m_editor->textColor(), this, QStringLiteral("选择文字颜色"));
    if (!selected.isValid())
        return;
    QTextCharFormat format;
    format.setForeground(selected);
    m_editor->mergeCurrentCharFormat(format);
}

void MainWindow::chooseImages()
{
    const QStringList files = QFileDialog::getOpenFileNames(
        this,
        QStringLiteral("插入图片"),
        QString(),
        QStringLiteral("图片 (*.png *.jpg *.jpeg *.bmp *.gif *.webp)"));
    insertImagesFromFiles(files);
}

void MainWindow::insertImagesFromFiles(const QStringList& files)
{
    for (const QString& file : files) {
        QImageReader reader(file);
        reader.setAutoTransform(true);
        const QSize sourceSize = reader.size();
        if (sourceSize.isValid()
            && (sourceSize.width() > kMaxStoredImageSide
                || sourceSize.height() > kMaxStoredImageSide)) {
            reader.setScaledSize(sourceSize.scaled(kMaxStoredImageSide,
                                                   kMaxStoredImageSide,
                                                   Qt::KeepAspectRatio));
        }
        QImage image = reader.read();
        if (image.isNull()) {
            setStatusMessage(QStringLiteral("无法读取图片 %1：%2")
                                 .arg(QFileInfo(file).fileName(), reader.errorString()),
                             true);
            continue;
        }
        insertImage(image, QFileInfo(file).completeBaseName());
    }
}

void MainWindow::insertImage(const QImage& original, const QString& sourceName)
{
    if (original.isNull() || m_currentNoteId < 0)
        return;

    QImage image = original;
    if (image.width() > kMaxStoredImageSide || image.height() > kMaxStoredImageSide) {
        image = image.scaled(kMaxStoredImageSide,
                             kMaxStoredImageSide,
                             Qt::KeepAspectRatio,
                             Qt::SmoothTransformation);
    }

    QString error;
    const QString attachmentPath = saveImageAttachment(image, sourceName, &error);
    if (attachmentPath.isEmpty()) {
        setStatusMessage(databaseErrorText(QStringLiteral("图片保存失败"), error), true);
        return;
    }

    const QUrl imageUrl = QUrl::fromLocalFile(attachmentPath);
    m_editor->document()->addResource(QTextDocument::ImageResource, imageUrl, image);

    const int availableWidth = std::max(260, m_editor->viewport()->width() - 50);
    const qreal scale = std::min<qreal>(1.0, static_cast<qreal>(availableWidth) / image.width());
    QTextImageFormat imageFormat;
    imageFormat.setName(imageUrl.toString());
    imageFormat.setWidth(image.width() * scale);
    imageFormat.setHeight(image.height() * scale);

    QTextCursor cursor = m_editor->textCursor();
    if (!cursor.atBlockStart())
        cursor.insertBlock();
    cursor.insertImage(imageFormat);
    cursor.insertBlock();
    m_editor->setTextCursor(cursor);
    m_editor->setFocus();
    setStatusMessage(QStringLiteral("已插入图片 · %1 × %2").arg(image.width()).arg(image.height()));
}

QString MainWindow::saveImageAttachment(const QImage& image,
                                        const QString& sourceName,
                                        QString* error)
{
    QDir dataDir(m_database->dataDirectory());
    if (!dataDir.mkpath(QStringLiteral("attachments"))) {
        if (error)
            *error = QStringLiteral("无法创建附件目录");
        return QString();
    }
    if (!dataDir.cd(QStringLiteral("attachments"))) {
        if (error)
            *error = QStringLiteral("无法进入附件目录");
        return QString();
    }

    QString base = sourceName;
    base.replace(QRegularExpression(QStringLiteral("[^a-zA-Z0-9_-]+")), QStringLiteral("-"));
    if (base.isEmpty())
        base = QStringLiteral("image");
    const QString name = QStringLiteral("%1-%2.png")
                             .arg(base.left(28), QUuid::createUuid().toString(QUuid::WithoutBraces));
    const QString path = dataDir.filePath(name);
    QImageWriter writer(path, "PNG");
    writer.setCompression(6);
    if (!writer.write(image)) {
        if (error)
            *error = writer.errorString();
        return QString();
    }
    return path;
}

void MainWindow::cacheNote(const NoteRecord& note)
{
    const qsizetype characters = note.html.size() + note.plainText.size()
        + note.title.size() + note.excerpt.size();
    const int costKiB = std::max(1, static_cast<int>((characters * sizeof(QChar) + 1023) / 1024));
    m_noteCache.remove(note.id);
    m_noteCache.insert(note.id, new NoteRecord(note), costKiB);
}

int MainWindow::summaryRevision(qint64 noteId) const
{
    for (int row = 0; row < m_noteList->count(); ++row) {
        const QListWidgetItem* item = m_noteList->item(row);
        if (item->data(Qt::UserRole).toLongLong() == noteId)
            return item->data(Qt::UserRole + 1).toInt();
    }
    return 0;
}

void MainWindow::selectCurrentFolderInEditor()
{
    if (!m_noteFolderCombo)
        return;
    const QSignalBlocker blocker(m_noteFolderCombo);
    int index = m_noteFolderCombo->findData(m_currentFolderId);
    if (index < 0)
        index = m_noteFolderCombo->findData(Database::UnfiledFolder);
    m_noteFolderCombo->setCurrentIndex(index);
}

void MainWindow::summonSticky()
{
    saveCurrentNote(true);
    if (m_stickyWindow) {
        m_stickyWindow->reloadFromDatabase();
        m_stickyWindow->summon();
    }
}

void MainWindow::showMainWindow()
{
    showNormal();
    raise();
    activateWindow();
}

void MainWindow::requestQuit()
{
    m_quitting = true;
    saveCurrentNote(true);
    if (m_stickyWindow)
        m_stickyWindow->flushSave();
    if (m_trayIcon)
        m_trayIcon->hide();
    QSettings settings;
    settings.setValue(QStringLiteral("main/geometry"), saveGeometry());
    qApp->quit();
}

void MainWindow::updateFormatControls()
{
    if (!m_editor)
        return;
    const QTextCharFormat format = m_editor->currentCharFormat();
    const QSignalBlocker boldBlocker(m_boldButton);
    const QSignalBlocker italicBlocker(m_italicButton);
    const QSignalBlocker underlineBlocker(m_underlineButton);
    const QSignalBlocker sizeBlocker(m_fontSizeCombo);
    m_boldButton->setChecked(format.fontWeight() >= QFont::Bold);
    m_italicButton->setChecked(format.fontItalic());
    m_underlineButton->setChecked(format.fontUnderline());
    const qreal size = format.fontPointSize() > 0 ? format.fontPointSize() : 12;
    m_fontSizeCombo->setCurrentText(QString::number(size, 'f', size == qRound(size) ? 0 : 1));
}

void MainWindow::setStatusMessage(const QString& message, bool warning)
{
    if (!m_saveStateLabel)
        return;
    m_saveStateLabel->setText(message);
    m_saveStateLabel->setStyleSheet(warning ? QStringLiteral("color: #B55345;") : QString());
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    saveCurrentNote(true);
    QSettings settings;
    settings.setValue(QStringLiteral("main/geometry"), saveGeometry());

    if (m_quitting || !m_trayIcon || !m_trayIcon->isVisible()) {
        m_quitting = true;
        event->accept();
        qApp->quit();
        return;
    }

    hide();
    event->ignore();
    if (!m_trayHintShown) {
        m_trayIcon->showMessage(QStringLiteral("FeatherNote 仍在后台"),
                                QStringLiteral("按 Ctrl+Alt+N 可随时呼出快速便签。"),
                                QSystemTrayIcon::Information,
                                2500);
        m_trayHintShown = true;
    }
}
