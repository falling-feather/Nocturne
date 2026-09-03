#include "MainWindow.h"

#include "BackupManager.h"
#include "Branding.h"
#include "Database.h"
#include "NoteEditor.h"
#include "NocturneStyle.h"
#include "StickyNoteWindow.h"
#include "WindowChrome.h"

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
#include <QDesktopServices>
#include <QDir>
#include <QEvent>
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
#include <QPalette>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QSize>
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
#include <QThread>
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
    : QMainWindow(parent, Qt::Window | Qt::FramelessWindowHint)
    , m_database(database)
{
    m_noteCache.setMaxCost(kNoteCacheMaxKiB);
    setObjectName(QStringLiteral("mainWindow"));
    setWindowTitle(NocturneBrand::chineseName());
    setWindowIcon(NocturneBrand::appIcon());
    resize(1280, 800);
    setMinimumSize(1040, 640);

    buildUi();
    buildMenus();
    applyTheme();
    connectSignals();

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
    QTimer::singleShot(0, this, &MainWindow::restorePinnedStickies);
    scheduleAutomaticBackup();
}

MainWindow::~MainWindow()
{
    saveCurrentNote(true);
    for (StickyNoteWindow* window : m_stickyWindows) {
        if (window)
            window->flushSave();
    }

    QSettings settings;
    settings.setValue(QStringLiteral("main/geometry"), saveGeometry());

    if (m_backupThread) {
        m_backupThread->wait();
        delete m_backupThread;
        m_backupThread = nullptr;
    }
}

void MainWindow::buildUi()
{
    auto* central = new NocturneBackdrop(this);
    central->setObjectName(QStringLiteral("windowShell"));
    auto* shellLayout = new QVBoxLayout(central);
    shellLayout->setContentsMargins(0, 0, 0, 0);
    shellLayout->setSpacing(0);

    auto* titleBar = new WindowDragArea(central, true);
    titleBar->setObjectName(QStringLiteral("appTitleBar"));
    titleBar->setFixedHeight(58);
    auto* titleLayout = new QHBoxLayout(titleBar);
    titleLayout->setContentsMargins(18, 0, 0, 0);
    titleLayout->setSpacing(10);

    auto* titleIcon = new QLabel(titleBar);
    titleIcon->setObjectName(QStringLiteral("titleBrandIcon"));
    titleIcon->setFixedSize(34, 34);
    titleIcon->setAlignment(Qt::AlignCenter);
    titleIcon->setPixmap(NocturneBrand::appIcon().pixmap(QSize(33, 33)));
    auto* titleName = new QLabel(NocturneBrand::chineseName(), titleBar);
    titleName->setObjectName(QStringLiteral("titleBrandName"));
    QFont titleBrandFont(NocturneUi::serifFamily(), 15);
    titleBrandFont.setWeight(QFont::DemiBold);
    titleName->setFont(titleBrandFont);
    auto* titleEnglish = new QLabel(NocturneBrand::englishName().toUpper(), titleBar);
    titleEnglish->setObjectName(QStringLiteral("titleBrandEnglish"));

    m_appMenuBar = new QMenuBar(titleBar);
    m_appMenuBar->setObjectName(QStringLiteral("appMenuBar"));
    m_appMenuBar->setNativeMenuBar(false);
    m_appMenuBar->setProperty("windowChromeInteractive", true);

    auto makeWindowButton = [titleBar](const QString& text,
                                       const QString& objectName,
                                       const QString& tooltip) {
        auto* button = new QToolButton(titleBar);
        button->setText(text);
        button->setObjectName(objectName);
        button->setToolTip(tooltip);
        button->setAutoRaise(true);
        button->setFocusPolicy(Qt::NoFocus);
        button->setFixedSize(48, 58);
        button->setProperty("windowChromeInteractive", true);
        return button;
    };
    auto* minimizeButton = makeWindowButton(QStringLiteral("−"),
                                            QStringLiteral("windowMinimizeButton"),
                                            QStringLiteral("最小化"));
    m_maximizeButton = makeWindowButton(QStringLiteral("□"),
                                        QStringLiteral("windowMaximizeButton"),
                                        QStringLiteral("最大化"));
    auto* closeButton = makeWindowButton(QStringLiteral("×"),
                                         QStringLiteral("windowCloseButton"),
                                         QStringLiteral("关闭到系统托盘"));

    titleLayout->addWidget(titleIcon);
    titleLayout->addWidget(titleName);
    titleLayout->addWidget(titleEnglish);
    titleLayout->addSpacing(14);
    titleLayout->addWidget(m_appMenuBar);
    titleLayout->addStretch(1);
    titleLayout->addWidget(minimizeButton);
    titleLayout->addWidget(m_maximizeButton);
    titleLayout->addWidget(closeButton);
    shellLayout->addWidget(titleBar);

    auto* content = new QFrame(central);
    content->setObjectName(QStringLiteral("appContent"));
    auto* rootLayout = new QHBoxLayout(content);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    auto* navigation = new NocturneGlassPanel(false, content);
    navigation->setObjectName(QStringLiteral("navigation"));
    navigation->setFixedWidth(286);
    auto* navigationLayout = new QVBoxLayout(navigation);
    navigationLayout->setContentsMargins(20, 21, 20, 15);
    navigationLayout->setSpacing(10);

    auto* brandMotto = new QLabel(NocturneBrand::motto(), navigation);
    brandMotto->setObjectName(QStringLiteral("brandMotto"));
    brandMotto->setFont(QFont(NocturneUi::serifFamily(), 10));
    navigationLayout->addWidget(brandMotto);

    auto* brandDivider = new QFrame(navigation);
    brandDivider->setObjectName(QStringLiteral("brandDivider"));
    brandDivider->setFrameShape(QFrame::HLine);
    brandDivider->setFixedHeight(1);
    navigationLayout->addWidget(brandDivider);

    m_searchEdit = new QLineEdit(navigation);
    m_searchEdit->setObjectName(QStringLiteral("searchEdit"));
    m_searchEdit->setPlaceholderText(QStringLiteral("寻一段文字…"));
    m_searchEdit->setClearButtonEnabled(true);
    m_searchEdit->setMinimumHeight(42);
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
    m_newNoteButton = new QPushButton(QStringLiteral("＋  新笺"), navigation);
    m_newNoteButton->setObjectName(QStringLiteral("primaryButton"));
    m_newNoteButton->setMinimumHeight(40);
    m_stickyButton = new QPushButton(QStringLiteral("便签"), navigation);
    m_stickyButton->setObjectName(QStringLiteral("secondaryButton"));
    m_stickyButton->setMinimumHeight(40);
    m_stickyButton->setToolTip(QStringLiteral("全局快捷键：Ctrl+Alt+N"));
    noteButtonRow->addWidget(m_newNoteButton, 1);
    noteButtonRow->addWidget(m_stickyButton);
    navigationLayout->addLayout(noteButtonRow);

    auto* notesCaption = new QLabel(QStringLiteral("笔记航册"), navigation);
    notesCaption->setObjectName(QStringLiteral("sectionCaption"));
    notesCaption->setFont(QFont(NocturneUi::serifFamily(), 12));
    navigationLayout->addWidget(notesCaption);

    m_noteList = new QListWidget(navigation);
    m_noteList->setObjectName(QStringLiteral("noteList"));
    m_noteList->setFrameShape(QFrame::NoFrame);
    m_noteList->setSpacing(0);
    m_noteList->setUniformItemSizes(true);
    m_noteList->setMouseTracking(true);
    m_noteList->setItemDelegate(new NocturneNoteDelegate(m_noteList));
    m_noteList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_noteList->setContextMenuPolicy(Qt::CustomContextMenu);
    navigationLayout->addWidget(m_noteList, 1);

    m_noteCountLabel = new QLabel(navigation);
    m_noteCountLabel->setObjectName(QStringLiteral("mutedLabel"));
    navigationLayout->addWidget(m_noteCountLabel);

    auto* editorPane = new NocturnePaperPanel(content);
    editorPane->setObjectName(QStringLiteral("editorPane"));
    auto* editorLayout = new QVBoxLayout(editorPane);
    editorLayout->setContentsMargins(40, 31, 38, 20);
    editorLayout->setSpacing(12);

    m_titleEdit = new QLineEdit(editorPane);
    m_titleEdit->setObjectName(QStringLiteral("titleEdit"));
    m_titleEdit->setPlaceholderText(QStringLiteral("未题笔记"));
    QFont editorTitleFont(NocturneUi::serifFamily(), 25);
    editorTitleFont.setWeight(QFont::DemiBold);
    m_titleEdit->setFont(editorTitleFont);
    editorLayout->addWidget(m_titleEdit);

    auto* separator = new QFrame(editorPane);
    separator->setObjectName(QStringLiteral("separator"));
    separator->setFrameShape(QFrame::HLine);
    separator->setFixedHeight(1);
    editorLayout->addWidget(separator);

    auto* toolbar = new QFrame(editorPane);
    toolbar->setObjectName(QStringLiteral("formatBar"));
    toolbar->setMinimumHeight(44);
    auto* formatRow = new QHBoxLayout(toolbar);
    formatRow->setContentsMargins(7, 4, 7, 4);
    formatRow->setSpacing(2);

    auto makeToolButton = [toolbar, formatRow](const QString& text, const QString& tooltip,
                                              bool checkable = false) {
        auto* button = new QToolButton(toolbar);
        button->setText(text);
        button->setToolTip(tooltip);
        button->setCheckable(checkable);
        button->setAutoRaise(true);
        button->setFixedSize(31, 31);
        formatRow->addWidget(button);
        return button;
    };

    auto addToolSeparator = [toolbar, formatRow] {
        auto* line = new QFrame(toolbar);
        line->setObjectName(QStringLiteral("toolSeparator"));
        line->setFrameShape(QFrame::VLine);
        line->setFixedSize(1, 20);
        formatRow->addSpacing(3);
        formatRow->addWidget(line);
        formatRow->addSpacing(3);
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

    addToolSeparator();
    auto* headingButton = makeToolButton(QStringLiteral("H1"), QStringLiteral("一级标题"));
    auto* bodyButton = makeToolButton(QStringLiteral("¶"), QStringLiteral("恢复正文样式"));
    addToolSeparator();
    auto* bulletButton = makeToolButton(QStringLiteral("•≡"), QStringLiteral("项目符号列表"));
    auto* numberedButton = makeToolButton(QStringLiteral("1."), QStringLiteral("编号列表"));
    auto* colorButton = makeToolButton(QStringLiteral("A"), QStringLiteral("文字颜色"));

    m_fontSizeCombo = new QComboBox(toolbar);
    m_fontSizeCombo->setToolTip(QStringLiteral("字号"));
    m_fontSizeCombo->setEditable(true);
    m_fontSizeCombo->setInsertPolicy(QComboBox::NoInsert);
    m_fontSizeCombo->addItems({QStringLiteral("10"), QStringLiteral("12"), QStringLiteral("14"),
                               QStringLiteral("16"), QStringLiteral("20"), QStringLiteral("24"),
                               QStringLiteral("32")});
    m_fontSizeCombo->setCurrentText(QStringLiteral("12"));
    m_fontSizeCombo->setFixedWidth(56);
    formatRow->addWidget(m_fontSizeCombo);

    addToolSeparator();
    auto* imageButton = makeToolButton(QStringLiteral("▧"), QStringLiteral("插入图片，也可直接拖入或粘贴"));
    imageButton->setObjectName(QStringLiteral("imageButton"));

    formatRow->addStretch(1);
    m_noteFolderCombo = new QComboBox(toolbar);
    m_noteFolderCombo->setObjectName(QStringLiteral("noteFolderCombo"));
    m_noteFolderCombo->setToolTip(QStringLiteral("把当前笔记移动到分组"));
    m_noteFolderCombo->setFixedWidth(142);
    formatRow->addWidget(m_noteFolderCombo);
    editorLayout->addWidget(toolbar);

    m_editor = new NoteEditor(editorPane);
    m_editor->setObjectName(QStringLiteral("noteEditor"));
    m_editor->setFrameShape(QFrame::NoFrame);
    m_editor->setFont(QFont(NocturneUi::sansFamily(), 11));
    editorLayout->addWidget(m_editor, 1);

    m_saveStateLabel = new QLabel(QStringLiteral("就绪"), editorPane);
    m_saveStateLabel->setObjectName(QStringLiteral("saveState"));
    m_saveStateLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    editorLayout->addWidget(m_saveStateLabel);

    auto* todoPane = new NocturneGlassPanel(true, content);
    todoPane->setObjectName(QStringLiteral("todoPane"));
    todoPane->setFixedWidth(302);
    auto* todoLayout = new QVBoxLayout(todoPane);
    todoLayout->setContentsMargins(21, 25, 21, 17);
    todoLayout->setSpacing(9);

    auto* todoTitle = new QLabel(QStringLiteral("今日待办"), todoPane);
    todoTitle->setObjectName(QStringLiteral("panelTitle"));
    QFont todoTitleFont(NocturneUi::serifFamily(), 20);
    todoTitleFont.setWeight(QFont::DemiBold);
    todoTitle->setFont(todoTitleFont);
    todoLayout->addWidget(todoTitle);
    auto* todoHint = new QLabel(QStringLiteral("把散念系成下一步行动"), todoPane);
    todoHint->setObjectName(QStringLiteral("mutedLabel"));
    todoLayout->addWidget(todoHint);

    auto* todoInputRow = new QHBoxLayout;
    todoInputRow->setSpacing(7);
    m_todoInput = new QLineEdit(todoPane);
    m_todoInput->setObjectName(QStringLiteral("todoInput"));
    m_todoInput->setPlaceholderText(QStringLiteral("添加一项待办…"));
    auto* addTodoButton = new QPushButton(QStringLiteral("＋"), todoPane);
    addTodoButton->setObjectName(QStringLiteral("roundButton"));
    addTodoButton->setFixedSize(42, 42);
    todoInputRow->addWidget(m_todoInput, 1);
    todoInputRow->addWidget(addTodoButton);
    todoLayout->addLayout(todoInputRow);

    m_todoList = new QListWidget(todoPane);
    m_todoList->setObjectName(QStringLiteral("todoList"));
    m_todoList->setFrameShape(QFrame::NoFrame);
    m_todoList->setSpacing(0);
    m_todoList->setMouseTracking(true);
    m_todoList->setItemDelegate(new NocturneTodoDelegate(m_todoList));
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
    shellLayout->addWidget(content, 1);
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
    connect(minimizeButton, &QToolButton::clicked, this, &MainWindow::showMinimized);
    connect(m_maximizeButton, &QToolButton::clicked, this, &MainWindow::toggleMaximized);
    connect(closeButton, &QToolButton::clicked, this, &MainWindow::close);
    updateWindowChrome();
}

void MainWindow::buildMenus()
{
    auto* fileMenu = m_appMenuBar->addMenu(QStringLiteral("文件(&F)"));
    QAction* newAction = fileMenu->addAction(QStringLiteral("新建笔记"));
    newAction->setShortcut(QKeySequence::New);
    QAction* importAction = fileMenu->addAction(QStringLiteral("导入文档…"));
    importAction->setShortcut(QKeySequence::Open);
    QAction* exportAction = fileMenu->addAction(QStringLiteral("导出当前笔记…"));
    exportAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+S")));
    fileMenu->addSeparator();
    QAction* backupAction = fileMenu->addAction(QStringLiteral("立即备份本地资料"));
    backupAction->setObjectName(QStringLiteral("manualBackupAction"));
    QAction* openBackupAction = fileMenu->addAction(QStringLiteral("打开备份目录"));
    openBackupAction->setObjectName(QStringLiteral("openBackupDirectoryAction"));
    fileMenu->addSeparator();
    QAction* renameAction = fileMenu->addAction(QStringLiteral("重命名当前笔记"));
    renameAction->setShortcut(QKeySequence(Qt::Key_F2));
    QAction* deleteAction = fileMenu->addAction(QStringLiteral("移到回收站"));
    deleteAction->setShortcut(QKeySequence::Delete);
    fileMenu->addSeparator();
    QAction* quitAction = fileMenu->addAction(QStringLiteral("退出夜航"));
    quitAction->setShortcut(QKeySequence::Quit);

    auto* insertMenu = m_appMenuBar->addMenu(QStringLiteral("插入(&I)"));
    QAction* imageAction = insertMenu->addAction(QStringLiteral("图片…"));
    imageAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+I")));
    QAction* stickyAction = insertMenu->addAction(QStringLiteral("新建桌面便签"));
    stickyAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+N")));

    auto* manageMenu = m_appMenuBar->addMenu(QStringLiteral("管理(&M)"));
    QAction* newFolderAction = manageMenu->addAction(QStringLiteral("新建分组…"));
    newFolderAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+G")));
    QAction* renameFolderAction = manageMenu->addAction(QStringLiteral("重命名当前分组…"));
    QAction* deleteFolderAction = manageMenu->addAction(QStringLiteral("删除当前分组…"));

    auto* helpMenu = m_appMenuBar->addMenu(QStringLiteral("帮助(&H)"));
    QAction* shortcutHelp = helpMenu->addAction(QStringLiteral("快捷键说明"));
    connect(shortcutHelp, &QAction::triggered, this, [this] {
        QMessageBox::information(this,
                                 QStringLiteral("夜航快捷键"),
                                 QStringLiteral("Ctrl+Alt+N　全局新建桌面便签\n"
                                                "Ctrl+Shift+N　窗口内新建桌面便签\n"
                                                "Ctrl+N　　　新建笔记\n"
                                                "Ctrl+O　　　导入文档\n"
                                                "Ctrl+Shift+I　插入图片\n"
                                                "Ctrl+Shift+G　新建分组\n"
                                                "F2　　　　　重命名当前笔记\n"
                                                "Ctrl+B / I / U　文字格式\n"
                                                "Ctrl+Shift+S　导出当前笔记"));
    });
    QAction* backupHelp = helpMenu->addAction(QStringLiteral("备份与恢复说明"));
    connect(backupHelp, &QAction::triggered, this, [this] {
        QMessageBox::information(
            this,
            QStringLiteral("夜航备份与恢复"),
            QStringLiteral("夜航每天最多自动备份一次，并分别保留最近 7 份自动备份和 10 份手动备份。\n\n"
                           "每份备份都包含 notebook.sqlite3、attachments 文件夹和 backup.json 校验清单。\n\n"
                           "恢复前请先完全退出夜航，再把所选备份中的数据库与附件复制回夜航数据目录。"
                           "建议先保留当前数据；也可以从“文件 → 打开备份目录”检查备份。"));
    });

    connect(newAction, &QAction::triggered, this, &MainWindow::createNote);
    connect(importAction, &QAction::triggered, this, &MainWindow::importDocument);
    connect(exportAction, &QAction::triggered, this, &MainWindow::exportDocument);
    connect(backupAction, &QAction::triggered, this, [this] { startBackup(false); });
    connect(openBackupAction, &QAction::triggered, this, &MainWindow::openBackupDirectory);
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

    m_trayIcon = new QSystemTrayIcon(NocturneBrand::appIcon(), this);
    m_trayIcon->setToolTip(QStringLiteral("夜航 · Nocturne · Ctrl+Alt+N 新建桌面便签"));
    m_trayMenu = new QMenu(this);
    m_trayMenu->setObjectName(QStringLiteral("trayMenu"));
    QAction* openAction = m_trayMenu->addAction(QStringLiteral("打开夜航"));
    QAction* stickyAction = m_trayMenu->addAction(QStringLiteral("新建桌面便签　Ctrl+Alt+N"));
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
    QPalette palette;
    palette.setColor(QPalette::Window, QColor(QStringLiteral("#071422")));
    palette.setColor(QPalette::WindowText, QColor(QStringLiteral("#E8D9B7")));
    palette.setColor(QPalette::Base, QColor(QStringLiteral("#F6E8CB")));
    palette.setColor(QPalette::AlternateBase, QColor(QStringLiteral("#EED9B5")));
    palette.setColor(QPalette::Text, QColor(QStringLiteral("#263244")));
    palette.setColor(QPalette::Button, QColor(QStringLiteral("#F3E2C3")));
    palette.setColor(QPalette::ButtonText, QColor(QStringLiteral("#243248")));
    palette.setColor(QPalette::Highlight, QColor(QStringLiteral("#A84E3D")));
    palette.setColor(QPalette::HighlightedText, QColor(QStringLiteral("#FFF5DE")));
    palette.setColor(QPalette::Disabled, QPalette::Text, QColor(QStringLiteral("#8C887D")));
    palette.setColor(QPalette::Disabled, QPalette::WindowText, QColor(QStringLiteral("#8C887D")));
    qApp->setPalette(palette);
    qApp->setFont(QFont(NocturneUi::sansFamily(), 10));

    // Menus and combo popups are separate top-level windows. Give every text and
    // background state an explicit color so Windows dark mode cannot recreate the
    // former white-on-white prompt bug.
    qApp->setStyleSheet(QStringLiteral(R"(
        QWidget { font-family: "%1"; selection-background-color: #A84E3D;
                  selection-color: #FFF4DA; }
        QMainWindow#mainWindow { background: #071422; }
        QWidget#windowShell, QFrame#appContent { background: transparent; }
        QDialog, QMessageBox { background: #F5E7CD; color: #253144; }

        QFrame#appTitleBar {
            background: rgba(4, 16, 30, 218);
            border-bottom: 1px solid rgba(220, 186, 115, 62);
        }
        QLabel#titleBrandIcon, QLabel#brandIcon { background: transparent; border: 0; }
        QLabel#titleBrandName { color: #F0CD86; }
        QLabel#titleBrandEnglish { color: #9BA9BA; font-size: 9px; font-weight: 700; letter-spacing: 1px; }
        QMenuBar { background: transparent; color: #E4D1AA; padding: 0; spacing: 2px; }
        QMenuBar#appMenuBar { background: transparent; padding: 0; }
        QMenuBar::item { background: transparent; color: #E4D1AA;
                         padding: 8px 12px; border-radius: 0; }
        QMenuBar::item:selected, QMenuBar::item:pressed {
            background: rgba(86, 111, 133, 92); color: #FFF0C7;
        }
        QMenu { background: #0C2034; color: #E8D7B5;
                border: 1px solid #4A5662; padding: 4px; }
        QMenu::item { background: transparent; color: #E8D7B5;
                      padding: 8px 34px 8px 13px; border-radius: 0; }
        QMenu::item:selected { background: #A24C3C; color: #FFF5DE; }
        QMenu::item:disabled { background: transparent; color: #6F7C8A; }
        QMenu::separator { height: 1px; background: #34475A; margin: 6px 9px; }
        QToolTip { background: #0B1D31; color: #F5DFC0; border: 1px solid #5B6570;
                   padding: 6px 8px; }

        QFrame#navigation, QFrame#editorPane, QFrame#todoPane { background: transparent; border: 0; }
        QLabel#brandName { color: #F1CE85; }
        QLabel#brandEnglish { color: #91A0B2; font-size: 9px; font-weight: 700; letter-spacing: 1px; }
        QLabel#brandMotto { color: #CCB98F; padding: 1px 0 5px 1px; }
        QFrame#brandDivider { background: rgba(205, 178, 119, 52); border: 0; }
        QLabel#sectionCaption { color: #DCC28A; font-weight: 600; margin-top: 4px; }
        QLabel#mutedLabel { color: #766E61; font-size: 10px; }
        QFrame#navigation QLabel#mutedLabel, QFrame#todoPane QLabel#mutedLabel {
            color: #92A0AE;
        }
        QLabel#panelTitle { color: #F0CF8A; }
        QLabel#saveState { color: #756B5C; font-size: 10px; }

        QLineEdit { background: #F3E4CA; color: #243147;
                    border: 0; border-bottom: 1px solid rgba(137, 105, 66, 118);
                    border-radius: 0; padding: 8px 10px; }
        QLineEdit:focus { border-bottom: 2px solid #A84E3D; }
        QLineEdit:disabled { background: #E9DDC7; color: #8C877C; }
        QLineEdit#searchEdit, QLineEdit#todoInput {
            background: rgba(17, 39, 58, 210); color: #F0E0BD;
            border: 0; border-bottom: 1px solid rgba(198, 170, 111, 118);
            padding: 8px 10px;
        }
        QLineEdit#searchEdit:focus, QLineEdit#todoInput:focus {
            background: rgba(23, 47, 68, 230); border-bottom: 2px solid #D5B66F;
        }
        QLineEdit#titleEdit { background: transparent; border: 0; border-radius: 0;
                              padding: 0 0 5px 0; color: #252B30; }

        QPushButton { background: #F0DFC1; color: #263248;
                      border: 1px solid rgba(128, 96, 58, 92);
                      border-radius: 1px; padding: 7px 11px; }
        QPushButton:hover { background: #FFF0D2; border-color: #A98555; }
        QPushButton:pressed { background: #E6D2AE; }
        QPushButton:disabled { background: #E6DBC7; color: #928C82; border-color: #CEC0A9; }
        QPushButton#primaryButton {
            background: #9F493B;
            color: #FFF1D3; border: 1px solid rgba(224, 174, 112, 82); font-weight: 650;
        }
        QPushButton#primaryButton:hover { background: #B55A45; border-color: #D6B16F; }
        QPushButton#secondaryButton { background: rgba(21, 43, 63, 210); color: #E2C68A;
                                      border-color: rgba(210, 179, 113, 80); }
        QPushButton#secondaryButton:hover { background: rgba(38, 66, 87, 230); border-color: #CDAE6D; }
        QPushButton#roundButton { background: #A74E3D; color: #FFF1D4; border: 0;
                                  border-radius: 0; font-size: 19px; }
        QPushButton#roundButton:hover { background: #B85A45; }
        QPushButton#quietButton { background: transparent; border: 0; color: #7C6E57; text-align: left; }
        QPushButton#quietButton:hover { color: #A84E3D; }
        QFrame#navigation QPushButton#quietButton, QFrame#todoPane QPushButton#quietButton {
            color: #AEB9C2;
        }
        QFrame#navigation QPushButton#quietButton:hover, QFrame#todoPane QPushButton#quietButton:hover {
            color: #F0CF89;
        }

        QListWidget#noteList, QListWidget#todoList {
            background: transparent; color: #DFD7C2; border: 0; outline: none;
        }
        QListWidget#noteList::item, QListWidget#todoList::item {
            background: transparent; border: 0; padding: 0;
        }

        QFrame#separator { background: rgba(126, 88, 47, 66); border: 0; }
        QFrame#formatBar { background: rgba(229, 210, 177, 70);
                           border: 0; border-top: 1px solid rgba(123, 87, 49, 62);
                           border-bottom: 1px solid rgba(123, 87, 49, 62);
                           border-radius: 0; }
        QFrame#toolSeparator { background: rgba(111, 81, 47, 58); border: 0; }
        QToolButton { color: #4B4D4D; background: transparent; border: 0;
                      border-radius: 0; padding: 0; }
        QToolButton:hover { background: rgba(188, 155, 101, 72); color: #1D2B3B; }
        QToolButton:checked { background: rgba(183, 137, 75, 112); color: #15263A; }
        QToolButton:disabled { color: #A49A8A; }
        QToolButton#imageButton { color: #9F473A; font-size: 17px; font-weight: 700; }
        QToolButton#imageButton:hover { background: rgba(168, 78, 61, 40); color: #8E3E34; }
        QToolButton#windowMinimizeButton, QToolButton#windowMaximizeButton,
        QToolButton#windowCloseButton { background: transparent; color: #D2D9DE; border: 0;
                                        border-radius: 0; padding: 0; font-size: 17px; }
        QToolButton#windowMinimizeButton:hover, QToolButton#windowMaximizeButton:hover {
            background: rgba(70, 96, 118, 100); color: #FFF0C7;
        }
        QToolButton#windowCloseButton:hover { background: #A74842; color: #FFFFFF; }
        QToolButton#windowMinimizeButton:pressed, QToolButton#windowMaximizeButton:pressed {
            background: rgba(85, 111, 132, 150);
        }
        QToolButton#windowCloseButton:pressed { background: #83342F; }

        QComboBox { background: rgba(246, 231, 202, 220); color: #293347;
                    border: 0; border-bottom: 1px solid rgba(127, 94, 54, 96); border-radius: 0;
                    padding: 5px 22px 5px 8px; }
        QComboBox:focus { border-bottom: 2px solid #A84E3D; }
        QComboBox::drop-down { border: 0; width: 20px; }
        QComboBox QAbstractItemView { background: #0C2034; color: #E8D7B5;
                                      border: 1px solid #4A5662; outline: none;
                                      selection-background-color: #A24C3C;
                                      selection-color: #FFF5DE; }
        QFrame#navigation QComboBox { background: rgba(20, 41, 61, 214); color: #E5D3AE;
                                      border-bottom: 1px solid rgba(198, 169, 108, 90); }
        QFrame#navigation QComboBox:focus { border-bottom: 2px solid #D5B66F; }

        QTextEdit#noteEditor { background: #F4E8D0; color: #303339; border: 0;
                               padding: 8px 3px; selection-background-color: #A84E3D;
                               selection-color: #FFF4DA; font-size: 11pt; line-height: 1.55; }

        QScrollBar:vertical { background: transparent; width: 9px; margin: 2px; }
        QScrollBar::handle:vertical { background: rgba(116, 93, 62, 92);
                                      min-height: 28px; border-radius: 0; }
        QFrame#navigation QScrollBar::handle:vertical,
        QFrame#todoPane QScrollBar::handle:vertical { background: rgba(126, 147, 164, 90); }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
    )").arg(NocturneUi::sansFamily()));

    if (m_searchEdit) {
        QPalette searchPalette = m_searchEdit->palette();
        searchPalette.setColor(QPalette::PlaceholderText, QColor(QStringLiteral("#8F9EAC")));
        m_searchEdit->setPalette(searchPalette);
    }
    if (m_todoInput) {
        QPalette todoPalette = m_todoInput->palette();
        todoPalette.setColor(QPalette::PlaceholderText, QColor(QStringLiteral("#8F9EAC")));
        m_todoInput->setPalette(todoPalette);
    }
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
        "<h1>欢迎登上夜航</h1>"
        "<p><i>所见所思，杂而成章。</i></p>"
        "<p>夜航是一款 <b>C++ 原生桌面笔记原型</b>。它把常用记录能力收在一个轻巧的工作流里：</p>"
        "<ul><li>正文支持富文本、列表，以及图片拖放和粘贴；</li>"
        "<li>右侧可以快速维护日常待办；</li>"
        "<li>按 <b>Ctrl+Alt+N</b>，随时呼出置顶便签；</li>"
        "<li>输入会在短暂停顿后自动保存到本机 SQLite。</li></ul>"
        "<p>现在就删掉这段文字，记下你的第一个游戏灵感吧。</p>");
    const QString plain = QStringLiteral(
        "欢迎登上夜航\n所见所思，杂而成章。\n这是 C++ 原生桌面笔记原型。\n"
        "正文支持富文本、列表、图片拖放和粘贴；右侧可维护待办；Ctrl+Alt+N 呼出便签。\n");
    m_database->createNote(QStringLiteral("欢迎登上夜航"), html, plain, &error);
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
        const QString safeTitle = note.title.trimmed().isEmpty()
            ? QStringLiteral("无标题笔记")
            : note.title.trimmed();
        const int folderIndex = m_noteFolderCombo->findData(note.folderId);
        const QString groupName = folderIndex >= 0
            ? m_noteFolderCombo->itemText(folderIndex)
            : QStringLiteral("未分组");
        const QString time = note.updatedAt.isValid()
            ? note.updatedAt.toLocalTime().toString(QStringLiteral("MM-dd  HH:mm"))
            : QStringLiteral("刚刚");
        auto* item = new QListWidgetItem(
            noteListText(note.title, note.excerpt, note.kind, note.updatedAt), m_noteList);
        item->setData(Qt::UserRole, note.id);
        item->setData(Qt::UserRole + 1, note.bodyRevision);
        item->setData(Qt::UserRole + 2, note.contentHash);
        item->setData(Qt::UserRole + 3, note.folderId);
        item->setData(Qt::UserRole + 4, note.kind);
        item->setData(NocturneUi::NoteTitleRole, safeTitle);
        item->setData(NocturneUi::NoteExcerptRole, note.excerpt);
        item->setData(NocturneUi::NoteGroupRole, groupName);
        item->setData(NocturneUi::NoteTimeRole, time);
        item->setData(NocturneUi::NoteKindRole, note.kind);
        item->setSizeHint(QSize(0, 80));
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
            const int folderIndex = m_noteFolderCombo->findData(cached.folderId);
            item->setData(NocturneUi::NoteTitleRole, title);
            item->setData(NocturneUi::NoteExcerptRole, cached.excerpt);
            item->setData(NocturneUi::NoteGroupRole,
                          folderIndex >= 0 ? m_noteFolderCombo->itemText(folderIndex)
                                           : QStringLiteral("未分组"));
            item->setData(NocturneUi::NoteTimeRole,
                          updatedAt.toLocalTime().toString(QStringLiteral("MM-dd  HH:mm")));
            item->setData(NocturneUi::NoteKindRole, cached.kind);
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
    QAction* openStickyAction = nullptr;
    if (item->data(Qt::UserRole + 4).toString() == QStringLiteral("sticky")) {
        openStickyAction = menu.addAction(QStringLiteral("在桌面打开"));
        menu.addSeparator();
    }
    QAction* renameAction = menu.addAction(QStringLiteral("重命名"));
    QAction* deleteAction = menu.addAction(QStringLiteral("移到回收站"));
    QAction* selected = menu.exec(m_noteList->viewport()->mapToGlobal(position));
    if (openStickyAction && selected == openStickyAction)
        openSticky(item->data(Qt::UserRole).toLongLong());
    else if (selected == renameAction)
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
        suggested = QStringLiteral("夜航-笔记");

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

void MainWindow::suspendHeavyContent()
{
    if (m_contentSuspended || m_quitting || !m_editor)
        return;
    if (!saveCurrentNote(true))
        return;

    m_saveTimer->stop();
    m_noteCache.clear();
    m_loadingNote = true;
    m_editor->clear();
    m_editor->document()->clearUndoRedoStacks();
    m_editor->document()->setModified(false);
    m_loadingNote = false;
    m_contentSuspended = true;
}

void MainWindow::resumeHeavyContent()
{
    if (!m_contentSuspended)
        return;
    m_contentSuspended = false;
    if (m_currentNoteId > 0)
        loadNote(m_currentNoteId);
}

StickyNoteWindow* MainWindow::openSticky(qint64 noteId, bool activate)
{
    saveCurrentNote(true);

    if (noteId > 0) {
        for (StickyNoteWindow* window : m_stickyWindows) {
            if (window && window->noteId() == noteId) {
                if (activate)
                    window->summon();
                return window;
            }
        }
    }

    auto* window = new StickyNoteWindow(m_database, noteId);
    m_stickyWindows.append(window);
    connect(window, &StickyNoteWindow::noteSaved, this,
            [this](qint64 savedNoteId) {
                m_noteCache.remove(savedNoteId);
                refreshNotes(m_currentNoteId);
            });
    connect(window, &QObject::destroyed, this,
            [this, window] { m_stickyWindows.removeOne(window); });
    if (activate)
        window->summon();
    return window;
}

void MainWindow::restorePinnedStickies()
{
    QString error;
    const QList<NoteSummary> notes = m_database->listNoteSummaries(QString(), &error);
    if (!error.isEmpty()) {
        setStatusMessage(databaseErrorText(QStringLiteral("恢复桌面便签失败"), error), true);
        return;
    }

    QSettings settings;
    for (const NoteSummary& note : notes) {
        if (note.kind != QStringLiteral("sticky"))
            continue;
        const QString base = QStringLiteral("stickyNotes/%1/").arg(note.id);
        if (settings.value(base + QStringLiteral("open"), false).toBool()
            && settings.value(base + QStringLiteral("pinned"), true).toBool()) {
            openSticky(note.id, false)->summon();
        }
    }
}

void MainWindow::summonSticky()
{
    openSticky();
}

void MainWindow::showMainWindow()
{
    if (isMinimized())
        showNormal();
    else
        show();
    raise();
    activateWindow();
    if (m_contentSuspended && !m_resumeQueued) {
        m_resumeQueued = true;
        QTimer::singleShot(0, this, [this] {
            m_resumeQueued = false;
            resumeHeavyContent();
        });
    }
}

void MainWindow::scheduleAutomaticBackup()
{
    // Give startup, note loading and pinned-sticky restoration priority. The actual
    // snapshot and attachment copy run on their own thread.
    QTimer::singleShot(5000, this, [this] {
        if (m_quitting
            || property("suppressTrayNotifications").toBool()
            || qApp->organizationName() == QStringLiteral("FeatherNoteTests")) {
            return;
        }
        startBackup(true);
    });
}

void MainWindow::startBackup(bool automatic)
{
    if (m_quitting)
        return;
    if (automatic
        && BackupManager(m_database->dataDirectory())
               .hasAutomaticBackupForDate(QDate::currentDate())) {
        return;
    }
    if (m_backupThread) {
        if (!automatic)
            setStatusMessage(QStringLiteral("已有备份正在进行，请稍候"), true);
        return;
    }
    if (!saveCurrentNote(true))
        return;
    for (StickyNoteWindow* window : m_stickyWindows) {
        if (window)
            window->flushSave();
    }

    setStatusMessage(automatic ? QStringLiteral("正在建立今日自动备份…")
                               : QStringLiteral("正在备份本地资料…"));
    const QString dataDirectory = m_database->dataDirectory();
    auto result = std::make_shared<BackupResult>();
    QThread* thread = QThread::create([dataDirectory, automatic, result] {
        BackupManager manager(dataDirectory);
        *result = manager.create(automatic ? BackupKind::Automatic
                                           : BackupKind::Manual);
    });
    m_backupThread = thread;
    connect(thread, &QThread::finished, this,
            [this, thread, result, automatic] {
                if (m_backupThread == thread)
                    m_backupThread = nullptr;
                thread->deleteLater();

                if (!result->success) {
                    setStatusMessage(
                        QStringLiteral("备份失败：%1").arg(result->error), true);
                } else if (result->created) {
                    const QString cleaned = QDir::toNativeSeparators(result->directory);
                    setStatusMessage(
                        result->removedOldBackups > 0
                            ? QStringLiteral("备份完成，并清理 %1 份过期备份")
                                  .arg(result->removedOldBackups)
                            : QStringLiteral("备份完成"));
                    if (!automatic && !m_quitAfterBackup && isVisible()) {
                        QMessageBox::information(
                            this,
                            QStringLiteral("备份完成"),
                            QStringLiteral("夜航已建立可独立恢复的本地备份：\n%1")
                                .arg(cleaned));
                    }
                } else {
                    setStatusMessage(QStringLiteral("今日自动备份已经就绪"));
                }

                if (m_quitAfterBackup) {
                    if (m_trayIcon)
                        m_trayIcon->hide();
                    qApp->quit();
                }
            });
    thread->start(QThread::LowPriority);
}

void MainWindow::openBackupDirectory()
{
    const QString directory = BackupManager(m_database->dataDirectory()).backupRoot();
    if (!QDir().mkpath(directory)
        || !QDesktopServices::openUrl(QUrl::fromLocalFile(directory))) {
        setStatusMessage(QStringLiteral("无法打开备份目录"), true);
    }
}

void MainWindow::requestQuit()
{
    m_quitting = true;
    saveCurrentNote(true);
    for (StickyNoteWindow* window : m_stickyWindows) {
        if (window)
            window->flushSave();
    }
    if (m_trayIcon)
        m_trayIcon->hide();
    QSettings settings;
    settings.setValue(QStringLiteral("main/geometry"), saveGeometry());
    if (m_backupThread) {
        m_quitAfterBackup = true;
        hide();
        return;
    }
    qApp->quit();
}

void MainWindow::toggleMaximized()
{
    isMaximized() ? showNormal() : showMaximized();
}

void MainWindow::updateWindowChrome()
{
    if (!m_maximizeButton)
        return;
    const bool maximized = isMaximized();
    m_maximizeButton->setText(maximized ? QStringLiteral("❐") : QStringLiteral("□"));
    m_maximizeButton->setToolTip(maximized ? QStringLiteral("还原") : QStringLiteral("最大化"));
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

void MainWindow::changeEvent(QEvent* event)
{
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange) {
        updateWindowChrome();
        if (isMinimized()) {
            QTimer::singleShot(250, this, [this] {
                if (isMinimized())
                    suspendHeavyContent();
            });
        } else if (m_contentSuspended && isVisible()) {
            QTimer::singleShot(0, this, &MainWindow::resumeHeavyContent);
        }
    }
}

bool MainWindow::nativeEvent(const QByteArray& eventType, void* message, qintptr* result)
{
    if (WindowChrome::handleNativeHitTest(this, message, result))
        return true;
    return QMainWindow::nativeEvent(eventType, message, result);
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
    suspendHeavyContent();
    event->ignore();
    if (!m_trayHintShown
        && !property("suppressTrayNotifications").toBool()) {
        m_trayIcon->showMessage(QStringLiteral("夜航仍在后台"),
                                QStringLiteral("按 Ctrl+Alt+N 可随时新建桌面便签。"),
                                QSystemTrayIcon::Information,
                                2500);
        m_trayHintShown = true;
    }
}
