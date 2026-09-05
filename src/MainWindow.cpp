#include "MainWindow.h"

#include "BackupManager.h"
#include "Branding.h"
#include "Database.h"
#include "GlobalHotkey.h"
#include "NoteEditor.h"
#include "NocturneStyle.h"
#include "NocturneDialogs.h"
#include "StickyNoteWindow.h"
#include "WindowChrome.h"

#include <QAbstractItemView>
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QCloseEvent>
#include <QColorDialog>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QImageReader>
#include <QImageWriter>
#include <QInputDialog>
#include <QKeySequenceEdit>
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
#include <QShortcut>
#include <QStackedWidget>
#include <QProgressBar>
#include <QResizeEvent>
#include <QScreen>
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
#include <memory>

namespace {
constexpr int kAutoSaveDelayMs = 650;
constexpr int kSearchDelayMs = 180;
constexpr int kMaxStoredImageSide = 1800;
constexpr int kNoteCacheMaxKiB = 24 * 1024;

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

MainWindow::MainWindow(Database* database, QWidget* parent)
    : QMainWindow(parent, Qt::Window | Qt::FramelessWindowHint)
    , m_database(database)
{
    m_noteCache.setMaxCost(kNoteCacheMaxKiB);
    setObjectName(QStringLiteral("mainWindow"));
    setWindowTitle(NocturneBrand::chineseName());
    setWindowIcon(NocturneBrand::appIcon());
    qApp->setStyle(QStringLiteral("Fusion"));
    NocturneUi::setTheme(QSettings().value(QStringLiteral("appearance/theme"),
                                          QStringLiteral("night")).toString());
    m_todoRequested = QSettings().value(QStringLiteral("appearance/showTodos"), true).toBool();
    resize(1440, 900);
    setMinimumSize(920, 600);

    buildUi();
    buildMenus();
    applyTheme();
    connectSignals();

    buildTray();
    restoreWindowState();
    updateAdaptiveLayout();

    QSettings settings;
    QKeySequence configuredSequence(
        settings.value(GlobalHotkey::settingsKey(),
                       GlobalHotkey::portableText(GlobalHotkey::defaultSequence()))
            .toString(),
        QKeySequence::PortableText);
    QString hotkeyValidationError;
    if (!GlobalHotkey::validate(configuredSequence, &hotkeyValidationError)) {
        configuredSequence = GlobalHotkey::defaultSequence();
        settings.setValue(GlobalHotkey::settingsKey(),
                          GlobalHotkey::portableText(configuredSequence));
    }
    m_globalHotkey = new GlobalHotkey(winId(),
                                      configuredSequence,
                                      [this] { summonSticky(); },
                                      this);
    updateGlobalHotkeyPresentation();
    if (!m_globalHotkey->isRegistered()) {
        setStatusMessage(
            QStringLiteral("%1 已被其他程序占用；可在“管理 → 唤笺快捷键”中更换")
                .arg(globalHotkeyText()),
            true);
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
    using namespace NocturneUi;
    auto* central = new NocturneBackdrop(this);
    central->setObjectName(QStringLiteral("windowShell"));
    auto* shell = new QVBoxLayout(central);
    shell->setContentsMargins(0, 0, 0, 0);
    shell->setSpacing(0);

    auto* titleBar = new WindowDragArea(central, true);
    titleBar->setObjectName(QStringLiteral("appTitleBar"));
    titleBar->setFixedHeight(52);
    auto* titleRow = new QHBoxLayout(titleBar);
    titleRow->setContentsMargins(0, 0, 0, 0);
    titleRow->setSpacing(0);
    auto* logoCell = new QWidget(titleBar);
    logoCell->setObjectName(QStringLiteral("logoCell"));
    logoCell->setFixedSize(64, 52);
    auto* logo = new QLabel(logoCell);
    logo->setPixmap(NocturneBrand::appIcon().pixmap(36, 36));
    logo->setGeometry(14, 8, 36, 36);
    titleRow->addWidget(logoCell);
    titleRow->addSpacing(25);
    auto* brand = new QLabel(NocturneBrand::chineseName(), titleBar);
    brand->setObjectName(QStringLiteral("titleBrandName"));
    titleRow->addWidget(brand);
    titleRow->addSpacing(16);
    auto* english = new QLabel(QStringLiteral("NOCTURNE"), titleBar);
    english->setObjectName(QStringLiteral("titleBrandEnglish"));
    titleRow->addWidget(english);
    titleRow->addSpacing(28);
    m_appMenuBar = new QMenuBar(titleBar);
    m_appMenuBar->setObjectName(QStringLiteral("appMenuBar"));
    m_appMenuBar->setNativeMenuBar(false);
    m_appMenuBar->setProperty("windowChromeInteractive", true);
    m_appMenuBar->setFixedHeight(34);
    titleRow->addWidget(m_appMenuBar, 0, Qt::AlignVCenter);
    titleRow->addStretch();

    auto windowButton = [titleBar, titleRow](Glyph glyph, const QString& name, const QString& tip) {
        auto* button = new QToolButton(titleBar);
        button->setObjectName(name);
        button->setToolTip(tip);
        button->setAccessibleName(tip);
        button->setProperty("windowChromeInteractive", true);
        button->setProperty("windowControl", true);
        button->setFixedSize(46, 51);
        button->setFocusPolicy(Qt::NoFocus);
        setGlyph(button, glyph);
        titleRow->addWidget(button);
        return button;
    };
    auto* minimize = windowButton(Glyph::Minimize, "windowMinimizeButton", QStringLiteral("最小化"));
    m_maximizeButton = windowButton(Glyph::Maximize, "windowMaximizeButton", QStringLiteral("最大化"));
    auto* close = windowButton(Glyph::Close, "windowCloseButton", QStringLiteral("关闭到系统托盘"));
    shell->addWidget(titleBar);

    auto* content = new QFrame(central);
    auto* row = new QHBoxLayout(content);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);
    auto* rail = new QFrame(content);
    rail->setObjectName(QStringLiteral("toolRail"));
    rail->setFixedWidth(64);
    auto* railLayout = new QVBoxLayout(rail);
    railLayout->setContentsMargins(9, 20, 9, 18);
    railLayout->setSpacing(15);
    auto railButton = [rail, railLayout](Glyph glyph, const QString& name, const QString& tip) {
        auto* button = new QToolButton(rail);
        button->setObjectName(name);
        button->setToolTip(tip);
        button->setAccessibleName(tip);
        button->setProperty("railButton", true);
        button->setProperty("iconTone", QStringLiteral("rail"));
        button->setFixedSize(46, 46);
        setGlyph(button, glyph);
        button->setIconSize(QSize(23, 23));
        railLayout->addWidget(button);
        return button;
    };
    auto* libraryButton = railButton(Glyph::Notebook, "libraryButton", QStringLiteral("笔记目录"));
    libraryButton->setCheckable(true);
    libraryButton->setChecked(true);
    m_stickyButton = new QPushButton(rail);
    m_stickyButton->setObjectName(QStringLiteral("secondaryButton"));
    m_stickyButton->setAccessibleName(QStringLiteral("新建桌面便签"));
    m_stickyButton->setProperty("iconTone", QStringLiteral("rail"));
    m_stickyButton->setFixedSize(46, 46);
    setGlyph(m_stickyButton, Glyph::Sticky);
    railLayout->addWidget(m_stickyButton);
    auto* collect = railButton(Glyph::Book, "collectButton", QStringLiteral("收舟入册 · Ctrl+Shift+B"));
    m_railTodoButton = railButton(Glyph::Todo, "railTodoButton", QStringLiteral("显示 / 收起待办"));
    m_railTodoButton->setCheckable(true);
    railLayout->addStretch();
    m_themeButton = railButton(Glyph::Palette, "themeButton", QStringLiteral("切换外观 · 夜航 / 雾港 / 月白"));
    m_themeButton->setPopupMode(QToolButton::InstantPopup);
    m_settingsButton = railButton(Glyph::Settings, "settingsButton", QStringLiteral("快捷设置与本地备份"));
    m_settingsButton->setPopupMode(QToolButton::InstantPopup);
    auto* local = new QLabel(QStringLiteral("●"), rail);
    local->setObjectName(QStringLiteral("saveState"));
    local->setToolTip(QStringLiteral("所有笔记保存在本机"));
    local->setAlignment(Qt::AlignCenter);
    railLayout->addWidget(local);
    row->addWidget(rail);

    m_navigation = new NocturneGlassPanel(false, content);
    m_navigation->setObjectName(QStringLiteral("navigation"));
    m_navigation->setFixedWidth(280);
    auto* nav = new QVBoxLayout(m_navigation);
    nav->setContentsMargins(16, 24, 16, 20);
    nav->setSpacing(14);
    auto* captionRow = new QHBoxLayout;
    captionRow->setContentsMargins(6, 0, 6, 0);
    auto* caption = new QLabel(QStringLiteral("笔记"), m_navigation);
    caption->setObjectName(QStringLiteral("sectionCaption"));
    auto* count = new QLabel(m_navigation);
    count->setObjectName(QStringLiteral("libraryCount"));
    captionRow->addWidget(caption);
    captionRow->addSpacing(8);
    captionRow->addWidget(count);
    captionRow->addStretch();
    nav->addLayout(captionRow);
    m_newNoteButton = new QPushButton(QStringLiteral("  新建笔记"), m_navigation);
    m_newNoteButton->setObjectName(QStringLiteral("primaryButton"));
    m_newNoteButton->setProperty("iconTone", QStringLiteral("primary"));
    m_newNoteButton->setMinimumHeight(40);
    m_newNoteButton->setToolTip(QStringLiteral("新建笔记 · Ctrl+N"));
    setGlyph(m_newNoteButton, Glyph::Plus);
    auto* hintLayout = new QHBoxLayout(m_newNoteButton);
    hintLayout->setContentsMargins(0, 0, 12, 0);
    hintLayout->addStretch();
    auto* shortcutHint = new QLabel(QStringLiteral("Ctrl N"), m_newNoteButton);
    shortcutHint->setObjectName(QStringLiteral("primaryHint"));
    shortcutHint->setAttribute(Qt::WA_TransparentForMouseEvents);
    hintLayout->addWidget(shortcutHint);
    nav->addWidget(m_newNoteButton);

    m_searchEdit = new QLineEdit(m_navigation);
    m_searchEdit->setObjectName(QStringLiteral("searchEdit"));
    m_searchEdit->setAccessibleName(QStringLiteral("搜索笔记"));
    m_searchEdit->setPlaceholderText(QStringLiteral("搜索笔记…"));
    m_searchEdit->setToolTip(QStringLiteral("搜索标题与正文 · Ctrl+K"));
    m_searchEdit->setClearButtonEnabled(true);
    m_searchEdit->setMinimumHeight(36);
    auto* searchIcon = m_searchEdit->addAction(icon(Glyph::Search), QLineEdit::LeadingPosition);
    setGlyph(searchIcon, Glyph::Search);
    nav->addWidget(m_searchEdit);
    auto* filterRow = new QHBoxLayout;
    filterRow->setSpacing(4);
    m_folderFilter = new NocturneComboBox(m_navigation);
    m_folderFilter->setObjectName(QStringLiteral("folderFilter"));
    m_folderFilter->setAccessibleName(QStringLiteral("按分组筛选笔记"));
    m_folderFilter->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_folderFilter->setMinimumContentsLength(7);
    m_folderManageButton = new QPushButton(m_navigation);
    m_folderManageButton->setObjectName(QStringLiteral("quietButton"));
    m_folderManageButton->setFixedSize(30, 32);
    m_folderManageButton->setToolTip(QStringLiteral("管理分组"));
    m_folderManageButton->setAccessibleName(QStringLiteral("管理分组"));
    setGlyph(m_folderManageButton, Glyph::More);
    filterRow->addWidget(m_folderFilter, 1);
    filterRow->addWidget(m_folderManageButton);
    nav->addLayout(filterRow);
    m_noteList = new QListWidget(m_navigation);
    m_noteList->setObjectName(QStringLiteral("noteList"));
    m_noteList->setAccessibleName(QStringLiteral("笔记列表"));
    m_noteList->setFrameShape(QFrame::NoFrame);
    m_noteList->setSpacing(2);
    m_noteList->setUniformItemSizes(true);
    m_noteList->setMouseTracking(true);
    m_noteList->setItemDelegate(new NocturneNoteDelegate(m_noteList));
    m_noteList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_noteList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_noteList->setContextMenuPolicy(Qt::CustomContextMenu);
    nav->addWidget(m_noteList, 1);
    auto* navDivider = new QFrame(m_navigation);
    navDivider->setObjectName(QStringLiteral("separator"));
    navDivider->setFixedHeight(1);
    nav->addWidget(navDivider);
    auto* motto = new QLabel(NocturneBrand::motto(), m_navigation);
    motto->setObjectName(QStringLiteral("brandMotto"));
    nav->addWidget(motto);
    m_noteCountLabel = new QLabel(m_navigation);
    m_noteCountLabel->setObjectName(QStringLiteral("noteCount"));
    nav->addWidget(m_noteCountLabel);
    row->addWidget(m_navigation);

    auto* editorPane = new NocturnePaperPanel(content);
    editorPane->setObjectName(QStringLiteral("editorPane"));
    auto* editorLayout = new QVBoxLayout(editorPane);
    editorLayout->setContentsMargins(0, 0, 0, 0);
    editorLayout->setSpacing(0);
    auto* documentHeader = new QFrame(editorPane);
    documentHeader->setObjectName(QStringLiteral("documentHeader"));
    documentHeader->setFixedHeight(58);
    auto* headerRow = new QHBoxLayout(documentHeader);
    headerRow->setContentsMargins(22, 0, 22, 0);
    headerRow->setSpacing(8);
    m_noteFolderCombo = new NocturneComboBox(documentHeader);
    m_noteFolderCombo->setObjectName(QStringLiteral("noteFolderCombo"));
    m_noteFolderCombo->setToolTip(QStringLiteral("把当前笔记移动到分组"));
    m_noteFolderCombo->setFixedWidth(145);
    headerRow->addWidget(m_noteFolderCombo);
    auto* breadcrumb = new QLabel(QStringLiteral("/  笔记"), documentHeader);
    breadcrumb->setObjectName(QStringLiteral("mutedLabel"));
    headerRow->addWidget(breadcrumb);
    headerRow->addStretch();
    m_openStickyButton = new QToolButton(documentHeader);
    m_openStickyButton->setObjectName(QStringLiteral("openStickyButton"));
    m_openStickyButton->setText(QStringLiteral("编辑便签"));
    m_openStickyButton->setToolTip(QStringLiteral("在独立便签窗口中编辑正文"));
    setGlyph(m_openStickyButton, Glyph::Sticky);
    m_openStickyButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    headerRow->addWidget(m_openStickyButton);
    m_openStickyButton->hide();
    m_focusButton = new QToolButton(documentHeader);
    m_focusButton->setObjectName(QStringLiteral("focusButton"));
    m_focusButton->setText(QStringLiteral("专注"));
    m_focusButton->setCheckable(true);
    m_focusButton->setToolTip(QStringLiteral("专注写作 · F11；Esc 返回"));
    setGlyph(m_focusButton, Glyph::Focus);
    m_focusButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_focusButton->setFixedHeight(32);
    headerRow->addWidget(m_focusButton);
    m_todoToggleButton = new QToolButton(documentHeader);
    m_todoToggleButton->setObjectName(QStringLiteral("todoToggleButton"));
    m_todoToggleButton->setText(QStringLiteral("待办"));
    m_todoToggleButton->setCheckable(true);
    m_todoToggleButton->setToolTip(QStringLiteral("显示 / 收起待办"));
    setGlyph(m_todoToggleButton, Glyph::Todo);
    m_todoToggleButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_todoToggleButton->setFixedHeight(32);
    headerRow->addWidget(m_todoToggleButton);
    editorLayout->addWidget(documentHeader);

    m_formatBar = new QFrame(editorPane);
    m_formatBar->setObjectName(QStringLiteral("formatBar"));
    m_formatBar->setFixedHeight(46);
    auto* formatRow = new QHBoxLayout(m_formatBar);
    formatRow->setContentsMargins(24, 5, 24, 5);
    formatRow->setSpacing(3);
    auto tool = [this, formatRow](const QString& text, const QString& name,
                                  const QString& tip, bool checkable = false) {
        auto* button = new QToolButton(m_formatBar);
        button->setObjectName(name);
        button->setText(text);
        button->setToolTip(tip);
        button->setAccessibleName(tip);
        button->setCheckable(checkable);
        button->setFixedSize(31, 31);
        formatRow->addWidget(button);
        return button;
    };
    auto separator = [this, formatRow] {
        auto* line = new QFrame(m_formatBar);
        line->setObjectName(QStringLiteral("toolSeparator"));
        line->setFixedSize(1, 20);
        formatRow->addSpacing(5); formatRow->addWidget(line); formatRow->addSpacing(5);
    };
    auto* bodyButton = tool(QStringLiteral("正文"), "bodyButton", QStringLiteral("恢复正文样式"));
    bodyButton->setFixedWidth(48);
    separator();
    m_boldButton = tool("B", "boldButton", QStringLiteral("粗体 · Ctrl+B"), true);
    QFont bold("Segoe UI", 11); bold.setBold(true); m_boldButton->setFont(bold);
    m_italicButton = tool("I", "italicButton", QStringLiteral("斜体 · Ctrl+I"), true);
    QFont italic("Georgia", 12); italic.setItalic(true); m_italicButton->setFont(italic);
    m_underlineButton = tool("U", "underlineButton", QStringLiteral("下划线 · Ctrl+U"), true);
    QFont underline("Segoe UI", 11); underline.setUnderline(true); m_underlineButton->setFont(underline);
    separator();
    auto* headingButton = tool("H1", "headingButton", QStringLiteral("一级标题"));
    auto* bulletButton = tool("", "bulletButton", QStringLiteral("项目符号列表"));
    setGlyph(bulletButton, Glyph::Bullet);
    auto* numberedButton = tool("", "numberedButton", QStringLiteral("编号列表"));
    setGlyph(numberedButton, Glyph::Numbered);
    separator();
    auto* colorButton = tool("A", "colorButton", QStringLiteral("文字颜色"));
    auto* imageButton = tool("", "imageButton", QStringLiteral("插入图片 · 可直接拖入或粘贴"));
    setGlyph(imageButton, Glyph::Image);
    formatRow->addStretch();
    m_fontSizeCombo = new NocturneComboBox(m_formatBar);
    m_fontSizeCombo->setObjectName(QStringLiteral("fontSizeCombo"));
    m_fontSizeCombo->setAccessibleName(QStringLiteral("字号"));
    m_fontSizeCombo->setToolTip(QStringLiteral("字号（磅）"));
    m_fontSizeCombo->setEditable(true);
    m_fontSizeCombo->setInsertPolicy(QComboBox::NoInsert);
    m_fontSizeCombo->addItems({"10", "12", "14", "16", "20", "24", "32"});
    m_fontSizeCombo->setCurrentText("12");
    m_fontSizeCombo->setFixedWidth(70);
    formatRow->addWidget(m_fontSizeCombo);
    editorLayout->addWidget(m_formatBar);

    m_documentStack = new QStackedWidget(editorPane);
    m_documentStack->setObjectName(QStringLiteral("documentStack"));
    auto* writingHost = new QWidget(m_documentStack);
    auto* columnRow = new QHBoxLayout(writingHost);
    columnRow->setContentsMargins(0, 0, 0, 0);
    m_writingColumn = new QWidget(writingHost);
    m_writingColumn->setObjectName(QStringLiteral("writingColumn"));
    m_writingColumn->setMaximumWidth(940);
    m_writingLayout = new QVBoxLayout(m_writingColumn);
    m_writingLayout->setContentsMargins(48, 42, 48, 16);
    m_writingLayout->setSpacing(12);
    m_titleEdit = new QLineEdit(m_writingColumn);
    m_titleEdit->setObjectName(QStringLiteral("titleEdit"));
    m_titleEdit->setAccessibleName(QStringLiteral("笔记标题"));
    m_titleEdit->setPlaceholderText(QStringLiteral("为这一页，起个名字"));
    m_titleEdit->setMinimumHeight(54);
    m_writingLayout->addWidget(m_titleEdit);
    m_noteMeta = new QLabel(m_writingColumn);
    m_noteMeta->setObjectName(QStringLiteral("noteMeta"));
    m_writingLayout->addWidget(m_noteMeta);
    m_writingLayout->addSpacing(22);
    m_editor = new NoteEditor(m_writingColumn);
    m_editor->setObjectName(QStringLiteral("noteEditor"));
    m_editor->setAccessibleName(QStringLiteral("笔记正文"));
    m_editor->setFrameShape(QFrame::NoFrame);
    m_editor->setFont(QFont(sansFamily(), 12));
    m_writingLayout->addWidget(m_editor, 1);
    columnRow->addStretch();
    columnRow->addWidget(m_writingColumn, 1);
    columnRow->addStretch();
    m_documentStack->addWidget(writingHost);

    auto* empty = new QWidget(m_documentStack);
    empty->setObjectName(QStringLiteral("emptyState"));
    auto* emptyLayout = new QVBoxLayout(empty);
    emptyLayout->setContentsMargins(40, 40, 40, 60);
    emptyLayout->setSpacing(18);
    emptyLayout->addStretch();
    auto* emptyLogo = new QLabel(empty);
    emptyLogo->setPixmap(NocturneBrand::appIcon().pixmap(64, 64));
    emptyLogo->setAlignment(Qt::AlignCenter);
    emptyLayout->addWidget(emptyLogo);
    m_emptyHeading = new QLabel(QStringLiteral("新的灵感，还在路上"), empty);
    m_emptyHeading->setObjectName(QStringLiteral("emptyHeading"));
    m_emptyHeading->setAlignment(Qt::AlignCenter);
    emptyLayout->addWidget(m_emptyHeading);
    m_emptyHint = new QLabel(empty);
    m_emptyHint->setObjectName(QStringLiteral("emptyHint"));
    m_emptyHint->setAlignment(Qt::AlignCenter);
    m_emptyHint->setWordWrap(true);
    emptyLayout->addWidget(m_emptyHint);
    auto* emptyCreate = new QPushButton(QStringLiteral("写下第一句"), empty);
    emptyCreate->setObjectName(QStringLiteral("emptyCreateButton"));
    emptyCreate->setFixedWidth(140);
    emptyLayout->addWidget(emptyCreate, 0, Qt::AlignHCenter);
    emptyLayout->addStretch();
    m_documentStack->addWidget(empty);
    editorLayout->addWidget(m_documentStack, 1);

    auto* footer = new QFrame(editorPane);
    footer->setObjectName(QStringLiteral("editorFooter"));
    footer->setFixedHeight(42);
    auto* footerRow = new QHBoxLayout(footer);
    footerRow->setContentsMargins(25, 0, 25, 0);
    m_saveStateLabel = new QLabel(QStringLiteral("已保存"), footer);
    m_saveStateLabel->setObjectName(QStringLiteral("saveState"));
    m_saveStateLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    footerRow->addWidget(m_saveStateLabel, 1);
    m_wordCountLabel = new QLabel(footer);
    m_wordCountLabel->setObjectName(QStringLiteral("wordCount"));
    footerRow->addWidget(m_wordCountLabel);
    footerRow->addSpacing(15);
    auto* focusHint = new QLabel(QStringLiteral("F11 专注"), footer);
    focusHint->setObjectName(QStringLiteral("mutedLabel"));
    footerRow->addWidget(focusHint);
    editorLayout->addWidget(footer);
    row->addWidget(editorPane, 1);

    m_todoPane = new NocturneGlassPanel(true, content);
    m_todoPane->setObjectName(QStringLiteral("todoPane"));
    m_todoPane->setFixedWidth(260);
    auto* todos = new QVBoxLayout(m_todoPane);
    todos->setContentsMargins(22, 27, 22, 22);
    todos->setSpacing(16);
    auto* todoHeading = new QHBoxLayout;
    auto* todoTitle = new QLabel(QStringLiteral("待办"), m_todoPane);
    todoTitle->setObjectName(QStringLiteral("panelTitle"));
    todoHeading->addWidget(todoTitle, 1);
    auto* closeTodo = new QToolButton(m_todoPane);
    closeTodo->setObjectName(QStringLiteral("closeTodoButton"));
    closeTodo->setToolTip(QStringLiteral("收起待办"));
    setGlyph(closeTodo, Glyph::Close);
    todoHeading->addWidget(closeTodo);
    todos->addLayout(todoHeading);
    auto* todoHint = new QLabel(QStringLiteral("一件一件，慢慢完成。"), m_todoPane);
    todoHint->setObjectName(QStringLiteral("mutedLabel"));
    todos->addWidget(todoHint);
    todos->addSpacing(12);
    m_todoSummaryLabel = new QLabel(m_todoPane);
    m_todoSummaryLabel->setObjectName(QStringLiteral("todoSummary"));
    todos->addWidget(m_todoSummaryLabel);
    m_todoProgress = new QProgressBar(m_todoPane);
    m_todoProgress->setObjectName(QStringLiteral("todoProgress"));
    m_todoProgress->setTextVisible(false);
    m_todoProgress->setFixedHeight(3);
    todos->addWidget(m_todoProgress);
    m_todoList = new QListWidget(m_todoPane);
    m_todoList->setObjectName(QStringLiteral("todoList"));
    m_todoList->setAccessibleName(QStringLiteral("待办列表"));
    m_todoList->setFrameShape(QFrame::NoFrame);
    m_todoList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_todoList->setMouseTracking(true);
    m_todoList->setItemDelegate(new NocturneTodoDelegate(m_todoList));
    todos->addWidget(m_todoList, 1);
    auto* todoInputRow = new QHBoxLayout;
    todoInputRow->setSpacing(8);
    m_todoInput = new QLineEdit(m_todoPane);
    m_todoInput->setObjectName(QStringLiteral("todoInput"));
    m_todoInput->setAccessibleName(QStringLiteral("添加待办"));
    m_todoInput->setPlaceholderText(QStringLiteral("添加待办…"));
    m_todoInput->setMinimumHeight(36);
    auto* addTodo = new QPushButton(m_todoPane);
    addTodo->setObjectName(QStringLiteral("roundButton"));
    addTodo->setProperty("iconTone", QStringLiteral("primary"));
    addTodo->setToolTip(QStringLiteral("添加待办 · Enter"));
    addTodo->setAccessibleName(QStringLiteral("添加待办"));
    addTodo->setFixedSize(36, 36);
    setGlyph(addTodo, Glyph::Plus);
    todoInputRow->addWidget(m_todoInput, 1);
    todoInputRow->addWidget(addTodo);
    todos->addLayout(todoInputRow);
    todos->addStretch(1);
    auto* clearTodo = new QPushButton(QStringLiteral("  清理已完成"), m_todoPane);
    clearTodo->setObjectName(QStringLiteral("quietButton"));
    setGlyph(clearTodo, Glyph::Trash);
    todos->addWidget(clearTodo);
    row->addWidget(m_todoPane);
    shell->addWidget(content, 1);
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
    connect(addTodo, &QPushButton::clicked, this, &MainWindow::addTodo);
    connect(clearTodo, &QPushButton::clicked, this, &MainWindow::clearCompletedTodos);
    connect(minimize, &QToolButton::clicked, this, &MainWindow::showMinimized);
    connect(m_maximizeButton, &QToolButton::clicked, this, &MainWindow::toggleMaximized);
    connect(close, &QToolButton::clicked, this, &MainWindow::close);
    connect(collect, &QToolButton::clicked, this, &MainWindow::collectStickies);
    connect(m_focusButton, &QToolButton::clicked, this, &MainWindow::toggleFocusMode);
    connect(m_todoToggleButton, &QToolButton::clicked, this, &MainWindow::toggleTodoPanel);
    connect(m_railTodoButton, &QToolButton::clicked, this, &MainWindow::toggleTodoPanel);
    connect(closeTodo, &QToolButton::clicked, this, &MainWindow::toggleTodoPanel);
    connect(emptyCreate, &QPushButton::clicked, this, &MainWindow::createNote);
    connect(m_openStickyButton, &QToolButton::clicked, this, [this] { openSticky(m_currentNoteId); });
    connect(libraryButton, &QToolButton::clicked, this, [this, libraryButton] {
        libraryButton->setChecked(true);
        m_focusMode = false;
        m_narrowTodoOverride = false;
        updateAdaptiveLayout();
        m_noteList->setFocus();
    });
    auto* searchShortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+K")), this);
    connect(searchShortcut, &QShortcut::activated, this, [this] {
        m_focusMode = false; m_narrowTodoOverride = false; updateAdaptiveLayout();
        m_searchEdit->setFocus(); m_searchEdit->selectAll();
    });
    auto* escape = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    connect(escape, &QShortcut::activated, this, [this] {
        if (m_focusMode) toggleFocusMode();
        else if (m_searchEdit->hasFocus()) { m_searchEdit->clear(); m_editor->setFocus(); }
    });
    updateWindowChrome();
}


void MainWindow::buildMenus()
{
    auto* appearanceMenu = new QMenu(QStringLiteral("外观"), this);
    appearanceMenu->setObjectName(QStringLiteral("appearanceMenu"));
    auto* themeGroup = new QActionGroup(this);
    const QStringList themeIds = {"night", "harbor", "moonlight"};
    const QStringList themeNames = {QStringLiteral("夜航 · 墨蓝暖金"),
        QStringLiteral("雾港 · 青灰银月"), QStringLiteral("月白 · 清朗纸页")};
    for (int index = 0; index < themeIds.size(); ++index) {
        QAction* action = appearanceMenu->addAction(themeNames.at(index));
        action->setObjectName(QStringLiteral("theme_%1").arg(themeIds.at(index)));
        action->setData(themeIds.at(index));
        action->setCheckable(true);
        action->setChecked(themeIds.at(index) == NocturneUi::theme().id);
        themeGroup->addAction(action);
        m_themeActions.append(action);
        connect(action, &QAction::triggered, this, [this, action] { chooseTheme(action->data().toString()); });
    }
    m_themeButton->setMenu(appearanceMenu);
    auto* settingsMenu = new QMenu(this);
    settingsMenu->addMenu(appearanceMenu);
    m_focusAction = settingsMenu->addAction(QStringLiteral("专注写作"));
    m_focusAction->setObjectName(QStringLiteral("focusAction"));
    m_focusAction->setCheckable(true);
    m_focusAction->setShortcut(QKeySequence(Qt::Key_F11));
    addAction(m_focusAction);
    connect(m_focusAction, &QAction::triggered, this, &MainWindow::toggleFocusMode);
    settingsMenu->addSeparator();
    settingsMenu->addAction(QStringLiteral("唤笺快捷键…"), this, &MainWindow::configureGlobalHotkey);
    settingsMenu->addAction(QStringLiteral("立即备份本地资料"), this, [this] { startBackup(false); });
    settingsMenu->addAction(QStringLiteral("打开备份目录"), this, &MainWindow::openBackupDirectory);
    m_settingsButton->setMenu(settingsMenu);

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
    QAction* collectAction = manageMenu->addAction(QStringLiteral("收舟入册…"));
    collectAction->setObjectName(QStringLiteral("collectStickiesAction"));
    collectAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+B")));
    manageMenu->addSeparator();
    QAction* hotkeyAction = manageMenu->addAction(QStringLiteral("唤笺快捷键…"));
    hotkeyAction->setObjectName(QStringLiteral("hotkeySettingsAction"));
    manageMenu->addSeparator();
    QAction* newFolderAction = manageMenu->addAction(QStringLiteral("新建分组…"));
    newFolderAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+G")));
    QAction* renameFolderAction = manageMenu->addAction(QStringLiteral("重命名当前分组…"));
    QAction* deleteFolderAction = manageMenu->addAction(QStringLiteral("删除当前分组…"));

    auto* helpMenu = m_appMenuBar->addMenu(QStringLiteral("帮助(&H)"));
    QAction* shortcutHelp = helpMenu->addAction(QStringLiteral("快捷键说明"));
    connect(shortcutHelp, &QAction::triggered, this, [this] {
        NocturneDialogs::information(this,
                                 QStringLiteral("夜航快捷键"),
                                 QStringLiteral("%1　全局新建桌面便签\n"
                                                "Ctrl+Shift+N　窗口内新建桌面便签\n"
                                                "Ctrl+N　　　新建笔记\n"
                                                "Ctrl+K　　　搜索笔记\n"
                                                "F11 / Esc　　进入 / 退出专注\n"
                                                "Ctrl+O　　　导入文档\n"
                                                "Ctrl+Shift+I　插入图片\n"
                                                "Ctrl+Shift+B　收舟入册\n"
                                                "Ctrl+Shift+G　新建分组\n"
                                                "F2　　　　　重命名当前笔记\n"
                                                "Ctrl+B / I / U　文字格式\n"
                                                "Ctrl+Shift+S　导出当前笔记")
                                     .arg(globalHotkeyText()));
    });
    QAction* backupHelp = helpMenu->addAction(QStringLiteral("备份与恢复说明"));
    connect(backupHelp, &QAction::triggered, this, [this] {
        NocturneDialogs::information(
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
    connect(collectAction, &QAction::triggered, this, &MainWindow::collectStickies);
    connect(hotkeyAction, &QAction::triggered, this, &MainWindow::configureGlobalHotkey);
    connect(newFolderAction, &QAction::triggered, this, &MainWindow::createFolder);
    connect(renameFolderAction, &QAction::triggered, this, &MainWindow::renameSelectedFolder);
    connect(deleteFolderAction, &QAction::triggered, this, &MainWindow::deleteSelectedFolder);
}

void MainWindow::buildTray()
{
    if (!QSystemTrayIcon::isSystemTrayAvailable())
        return;

    m_trayIcon = new QSystemTrayIcon(NocturneBrand::appIcon(), this);
    m_trayIcon->setToolTip(QStringLiteral("夜航 · Nocturne · %1 新建桌面便签")
                               .arg(globalHotkeyText()));
    m_trayMenu = new QMenu(this);
    m_trayMenu->setObjectName(QStringLiteral("trayMenu"));
    QAction* openAction = m_trayMenu->addAction(QStringLiteral("打开夜航"));
    m_trayStickyAction = m_trayMenu->addAction(
        QStringLiteral("新建桌面便签　%1").arg(globalHotkeyText()));
    m_trayStickyAction->setObjectName(QStringLiteral("trayStickyAction"));
    m_trayMenu->addSeparator();
    QAction* quitAction = m_trayMenu->addAction(QStringLiteral("退出"));
    m_trayIcon->setContextMenu(m_trayMenu);

    connect(openAction, &QAction::triggered, this, &MainWindow::showMainWindow);
    connect(m_trayStickyAction, &QAction::triggered, this, &MainWindow::summonSticky);
    connect(quitAction, &QAction::triggered, this, &MainWindow::requestQuit);
    connect(m_trayIcon, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick)
                    showMainWindow();
            });
    m_trayIcon->show();
}

QString MainWindow::globalHotkeyText() const
{
    const QKeySequence sequence = m_globalHotkey
        ? m_globalHotkey->sequence()
        : GlobalHotkey::defaultSequence();
    return GlobalHotkey::displayText(sequence);
}

void MainWindow::updateGlobalHotkeyPresentation()
{
    const QString hotkey = globalHotkeyText();
    if (m_stickyButton) {
        m_stickyButton->setToolTip(
            QStringLiteral("全局快捷键：%1").arg(hotkey));
    }
    if (m_trayStickyAction) {
        m_trayStickyAction->setText(
            QStringLiteral("新建桌面便签　%1").arg(hotkey));
    }
    if (m_trayIcon) {
        m_trayIcon->setToolTip(
            QStringLiteral("夜航 · Nocturne · %1 新建桌面便签").arg(hotkey));
    }
}

void MainWindow::applyTheme()
{
    NocturneUi::applyPalette();
    qApp->setStyleSheet(NocturneUi::styleSheet());
    qApp->setFont(QFont(NocturneUi::sansFamily(), 10));
    for (QWidget* window : QApplication::topLevelWidgets()) {
        NocturneUi::refreshIcons(window);
        window->update();
    }
    if (m_editor) {
        // Theme changes only repaint; never mark an existing note dirty.
        const QSignalBlocker blocker(m_editor);
        m_editor->refreshTheme();
    }
    updateWindowChrome();
}

void MainWindow::chooseTheme(const QString& id)
{
    if (NocturneUi::theme().id == id) return;
    NocturneUi::setTheme(id);
    QSettings().setValue(QStringLiteral("appearance/theme"), NocturneUi::theme().id);
    applyTheme();
    for (QAction* action : m_themeActions) {
        const QSignalBlocker blocker(action);
        action->setChecked(action->data().toString() == NocturneUi::theme().id);
    }
}

void MainWindow::toggleFocusMode()
{
    m_focusMode = !m_focusMode;
    updateAdaptiveLayout();
    m_editor->setFocus();
}

void MainWindow::toggleTodoPanel()
{
    if (m_focusMode) m_focusMode = false;
    const bool opening = !m_todoPane->isVisible();
    m_todoRequested = opening;
    m_narrowTodoOverride = opening && width() < 1200;
    QSettings().setValue(QStringLiteral("appearance/showTodos"), m_todoRequested);
    updateAdaptiveLayout();
    if (opening) m_todoInput->setFocus();
}

void MainWindow::updateAdaptiveLayout()
{
    if (!m_navigation || !m_todoPane || !m_formatBar) return;
    const bool narrow = width() < 1200;
    const bool showTodo = !m_focusMode && m_todoRequested && (!narrow || m_narrowTodoOverride);
    m_navigation->setFixedWidth(width() < 1080 ? 242 : 280);
    m_navigation->setVisible(!m_focusMode && width() >= 900 && !(narrow && showTodo));
    m_todoPane->setVisible(showTodo);
    m_formatBar->setVisible(!m_focusMode);
    const QSignalBlocker focusBlocker(m_focusButton);
    const QSignalBlocker todoBlocker(m_todoToggleButton);
    const QSignalBlocker railBlocker(m_railTodoButton);
    m_focusButton->setChecked(m_focusMode);
    m_focusButton->setText(m_focusMode ? QStringLiteral("退出专注") : QStringLiteral("专注"));
    m_todoToggleButton->setChecked(showTodo);
    m_railTodoButton->setChecked(showTodo);
    if (m_focusAction) {
        const QSignalBlocker blocker(m_focusAction);
        m_focusAction->setChecked(m_focusMode);
    }
    if (m_writingLayout) {
        const int margin = narrow && !m_focusMode ? 32 : 48;
        m_writingLayout->setContentsMargins(margin, m_focusMode ? 60 : 42, margin, 16);
    }
}

void MainWindow::resizeEvent(QResizeEvent* event)
{
    QMainWindow::resizeEvent(event);
    updateAdaptiveLayout();
}

void MainWindow::updateDocumentInfo()
{
    if (!m_editor || !m_noteMeta) return;
    const bool exists = m_currentNoteId > 0;
    m_wordCountLabel->setText(exists
        ? QStringLiteral("%1 字符").arg(qMax(0, m_editor->document()->characterCount() - 1))
        : QString());
    m_noteMeta->setText(exists
        ? QStringLiteral("%1  ·  %2").arg(
            m_currentCreatedAt.toLocalTime().toString(QStringLiteral("yyyy年M月d日")),
            m_noteFolderCombo->currentText())
        : QString());
    m_openStickyButton->setVisible(exists && m_currentNoteKind == QStringLiteral("sticky"));
    m_formatBar->setEnabled(exists && m_currentNoteKind != QStringLiteral("sticky"));
    m_noteFolderCombo->setEnabled(exists);
    m_documentStack->setCurrentIndex(exists ? 0 : 1);
    if (!exists) {
        const bool searching = !m_searchEdit->text().trimmed().isEmpty();
        m_emptyHeading->setText(searching ? QStringLiteral("暂未找到这段文字") : QStringLiteral("新的灵感，还在路上"));
        m_emptyHint->setText(searching ? QStringLiteral("试试更短的关键词，或清空搜索看看其他笔记。")
                                       : QStringLiteral("这里还是空白。写下第一句，让念头在此靠岸。"));
        m_saveStateLabel->setText(searching ? QStringLiteral("没有匹配的笔记") : QStringLiteral("准备好，开始记录"));
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
    connect(m_editor, &QTextEdit::textChanged, this, [this] { scheduleSave(); updateDocumentInfo(); });
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
    if (QScreen* target = screen()) {
        const QRect available = target->availableGeometry();
        setMinimumSize(qMin(920, available.width()), qMin(600, available.height()));
        resize(qMin(width(), available.width()), qMin(height(), available.height()));
        move(std::clamp(x(), available.left(), available.right() - width() + 1),
             std::clamp(y(), available.top(), available.bottom() - height() + 1));
    }
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

    const QString hotkey = globalHotkeyText().toHtmlEscaped();
    const QString html = QStringLiteral(
        "<p>白天来不及停留的念头，就在此刻慢慢展开。</p>"
        "<p>一段读到的文字，一个尚未成形的计划，<br>"
        "或是窗外的灯，和忽然安静下来的自己。</p>"
        "<h2>从这一页，开始夜航</h2>"
        "<ul><li>按 <b>Ctrl+N</b> 新建笔记，用 <b>Ctrl+K</b> 寻找文字；</li>"
        "<li>按 <b>%1</b>，随时记下一枚桌面便签；</li>"
        "<li>按 <b>F11</b> 进入专注，留一整页给此刻的想法；</li>"
        "<li>点左下角的调色盘，在夜航、雾港、月白之间切换。</li></ul>"
        "<p>文字会自动保存在本机。图片也可以直接粘贴或拖入。</p>"
        "<p><i>不必急着抵达。写下来，就是一次出发。</i></p>")
                             .arg(hotkey);
    QTextDocument welcome;
    welcome.setHtml(html);
    m_database->createNote(QStringLiteral("今夜，让灵感靠岸"), html, welcome.toPlainText(), &error);
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
        item->setSizeHint(QSize(0, 104));
        if (note.id == preferredId || (preferredId < 0 && note.id == m_currentNoteId))
            selectedRow = index;
    }
    m_noteCountLabel->setText(QStringLiteral("本地存储 · %1 篇笔记").arg(notes.size()));
    findChild<QLabel*>(QStringLiteral("libraryCount"))->setText(QString::number(notes.size()));

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
        updateDocumentInfo();
        return;
    }

    if (selectedRow < 0)
        selectedRow = 0;
    m_noteList->setCurrentRow(selectedRow);
    const qint64 selectedId = m_noteList->currentItem()->data(Qt::UserRole).toLongLong();
    const int selectedRevision = m_noteList->currentItem()->data(Qt::UserRole + 1).toInt();
    if (selectedId != m_currentNoteId || selectedRevision != m_currentBodyRevision)
        loadNote(selectedId);
    updateDocumentInfo();
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
    Q_UNUSED(cacheHit)
    setStatusMessage(QStringLiteral("已保存 · %1")
                         .arg(loaded.updatedAt.toLocalTime().toString(QStringLiteral("MM-dd HH:mm"))));
    updateDocumentInfo();
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

void MainWindow::collectStickies()
{
    if (!saveCurrentNote(true))
        return;
    for (StickyNoteWindow* window : m_stickyWindows) {
        if (window)
            window->flushSave();
    }

    QString error;
    const QList<NoteSummary> summaries = m_database->listNoteSummaries(QString(), &error);
    if (!error.isEmpty()) {
        setStatusMessage(databaseErrorText(QStringLiteral("读取便签失败"), error), true);
        return;
    }
    QList<NoteSummary> stickies;
    for (const NoteSummary& summary : summaries) {
        if (summary.kind == QStringLiteral("sticky"))
            stickies.append(summary);
    }
    if (stickies.isEmpty()) {
        NocturneDialogs::information(this,
                                 QStringLiteral("收舟入册"),
                                 QStringLiteral("尚无可入册的桌面便签。先记下一枚灵感，再回来整理。"));
        return;
    }

    NocturneDialog dialog(this);
    dialog.setObjectName(QStringLiteral("collectStickiesDialog"));
    dialog.setWindowTitle(QStringLiteral("收舟入册 · 夜航"));
    dialog.setWindowIcon(NocturneBrand::appIcon());
    dialog.resize(610, 570);
    auto* layout = new QVBoxLayout(dialog.body());
    layout->setContentsMargins(28, 24, 28, 22);
    layout->setSpacing(11);

    auto* eyebrow = new QLabel(QStringLiteral("NOCTURNE · DESK NOTES"), &dialog);
    eyebrow->setObjectName(QStringLiteral("collectEyebrow"));
    layout->addWidget(eyebrow);
    auto* heading = new QLabel(QStringLiteral("收舟入册"), &dialog);
    heading->setObjectName(QStringLiteral("collectHeading"));
    QFont headingFont(NocturneUi::serifFamily(), 22);
    headingFont.setWeight(QFont::DemiBold);
    heading->setFont(headingFont);
    layout->addWidget(heading);
    auto* description = new QLabel(
        QStringLiteral("勾选要汇集的便签；拖动条目可调整它们在正式笔记中的先后。"),
        &dialog);
    description->setObjectName(QStringLiteral("collectHint"));
    description->setWordWrap(true);
    layout->addWidget(description);

    auto* stickyList = new QListWidget(&dialog);
    stickyList->setObjectName(QStringLiteral("collectStickyList"));
    stickyList->setDragDropMode(QAbstractItemView::InternalMove);
    stickyList->setDefaultDropAction(Qt::MoveAction);
    stickyList->setSelectionMode(QAbstractItemView::SingleSelection);
    stickyList->setMinimumHeight(235);
    for (const NoteSummary& sticky : stickies) {
        QString excerpt = sticky.excerpt.simplified();
        if (excerpt.isEmpty())
            excerpt = QStringLiteral("空白便签");
        const QString updated = sticky.updatedAt.isValid()
            ? sticky.updatedAt.toLocalTime().toString(QStringLiteral("MM-dd  HH:mm"))
            : QStringLiteral("刚刚");
        auto* item = new QListWidgetItem(
            QStringLiteral("%1\n%2  ·  %3")
                .arg(sticky.title.trimmed().isEmpty() ? QStringLiteral("快速便签")
                                                      : sticky.title.trimmed(),
                     excerpt.left(60),
                     updated),
            stickyList);
        item->setData(Qt::UserRole, sticky.id);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable
                       | Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled);
        item->setCheckState(Qt::Checked);
        item->setSizeHint(QSize(0, 62));
    }
    layout->addWidget(stickyList, 1);

    auto* selectionRow = new QHBoxLayout;
    selectionRow->setSpacing(12);
    auto* selectionSummary = new QLabel(&dialog);
    selectionSummary->setObjectName(QStringLiteral("collectHint"));
    selectionRow->addWidget(selectionSummary, 1);
    auto* selectAllButton = new QPushButton(QStringLiteral("全选"), &dialog);
    selectAllButton->setObjectName(QStringLiteral("quietButton"));
    auto* clearButton = new QPushButton(QStringLiteral("清空"), &dialog);
    clearButton->setObjectName(QStringLiteral("quietButton"));
    selectionRow->addWidget(selectAllButton);
    selectionRow->addWidget(clearButton);
    layout->addLayout(selectionRow);

    auto* form = new QFormLayout;
    form->setHorizontalSpacing(16);
    form->setVerticalSpacing(10);
    auto* titleEdit = new QLineEdit(
        QStringLiteral("夜航拾遗 · %1")
            .arg(QDate::currentDate().toString(QStringLiteral("yyyy-MM-dd"))),
        &dialog);
    titleEdit->setObjectName(QStringLiteral("collectNoteTitle"));
    titleEdit->setMaxLength(160);
    auto* titleLabel = new QLabel(QStringLiteral("笔记名称"), &dialog);
    titleLabel->setObjectName(QStringLiteral("collectFormLabel"));
    form->addRow(titleLabel, titleEdit);
    auto* folderCombo = new NocturneComboBox(&dialog);
    folderCombo->setObjectName(QStringLiteral("collectFolderCombo"));
    folderCombo->addItem(QStringLiteral("未分组"), Database::UnfiledFolder);
    const QList<FolderRecord> folders = m_database->listFolders(&error);
    if (!error.isEmpty()) {
        setStatusMessage(databaseErrorText(QStringLiteral("读取分组失败"), error), true);
        return;
    }
    for (const FolderRecord& folder : folders)
        folderCombo->addItem(folder.name, folder.id);
    const int currentFolderIndex = folderCombo->findData(
        m_currentFolderId > 0 ? m_currentFolderId : Database::UnfiledFolder);
    folderCombo->setCurrentIndex(std::max(0, currentFolderIndex));
    auto* folderLabel = new QLabel(QStringLiteral("归入分组"), &dialog);
    folderLabel->setObjectName(QStringLiteral("collectFormLabel"));
    form->addRow(folderLabel, folderCombo);
    layout->addLayout(form);

    auto* preservationHint = new QLabel(
        QStringLiteral("这是非破坏性操作：入册完成后，原便签仍会保留。"), &dialog);
    preservationHint->setObjectName(QStringLiteral("collectHint"));
    layout->addWidget(preservationHint);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok
                                             | QDialogButtonBox::Cancel,
                                         &dialog);
    QPushButton* acceptButton = buttons->button(QDialogButtonBox::Ok);
    acceptButton->setText(QStringLiteral("收舟入册"));
    acceptButton->setObjectName(QStringLiteral("collectStickiesAccept"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    layout->addWidget(buttons);

    const auto updateSelection = [stickyList, selectionSummary, titleEdit, acceptButton] {
        int checked = 0;
        for (int row = 0; row < stickyList->count(); ++row) {
            if (stickyList->item(row)->checkState() == Qt::Checked)
                ++checked;
        }
        selectionSummary->setText(QStringLiteral("已选 %1 / %2 枚")
                                      .arg(checked)
                                      .arg(stickyList->count()));
        acceptButton->setEnabled(checked > 0 && !titleEdit->text().trimmed().isEmpty());
    };
    connect(stickyList, &QListWidget::itemChanged, &dialog,
            [updateSelection](QListWidgetItem*) { updateSelection(); });
    connect(titleEdit, &QLineEdit::textChanged, &dialog,
            [updateSelection](const QString&) { updateSelection(); });
    connect(selectAllButton, &QPushButton::clicked, &dialog, [stickyList] {
        for (int row = 0; row < stickyList->count(); ++row)
            stickyList->item(row)->setCheckState(Qt::Checked);
    });
    connect(clearButton, &QPushButton::clicked, &dialog, [stickyList] {
        for (int row = 0; row < stickyList->count(); ++row)
            stickyList->item(row)->setCheckState(Qt::Unchecked);
    });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    updateSelection();

    if (dialog.exec() != QDialog::Accepted)
        return;

    QList<qint64> selectedIds;
    for (int row = 0; row < stickyList->count(); ++row) {
        QListWidgetItem* item = stickyList->item(row);
        if (item->checkState() == Qt::Checked)
            selectedIds.append(item->data(Qt::UserRole).toLongLong());
    }
    const qint64 folderId = folderCombo->currentData().toLongLong();
    const qint64 noteId = m_database->collectStickyNotes(selectedIds,
                                                         titleEdit->text(),
                                                         folderId,
                                                         &error);
    if (noteId <= 0) {
        setStatusMessage(databaseErrorText(QStringLiteral("收舟入册失败"), error), true);
        return;
    }

    m_noteCache.clear();
    m_searchTimer->stop();
    m_searchEdit->clear();
    refreshFolders(folderId);
    refreshNotes(noteId);
    setStatusMessage(QStringLiteral("已将 %1 枚便签收入“%2”")
                         .arg(selectedIds.size())
                         .arg(titleEdit->text().simplified()));
}

void MainWindow::configureGlobalHotkey()
{
    if (!m_globalHotkey)
        return;

    NocturneDialog dialog(this);
    dialog.setObjectName(QStringLiteral("hotkeyDialog"));
    dialog.setWindowTitle(QStringLiteral("唤笺快捷键 · 夜航"));
    dialog.setWindowIcon(NocturneBrand::appIcon());
    dialog.resize(540, 360);
    auto* layout = new QVBoxLayout(dialog.body());
    layout->setContentsMargins(28, 24, 28, 22);
    layout->setSpacing(12);

    auto* eyebrow = new QLabel(QStringLiteral("NOCTURNE · SUMMON"), &dialog);
    eyebrow->setObjectName(QStringLiteral("hotkeyEyebrow"));
    layout->addWidget(eyebrow);
    auto* heading = new QLabel(QStringLiteral("唤笺快捷键"), &dialog);
    heading->setObjectName(QStringLiteral("hotkeyHeading"));
    QFont headingFont(NocturneUi::serifFamily(), 22);
    headingFont.setWeight(QFont::DemiBold);
    heading->setFont(headingFont);
    layout->addWidget(heading);
    auto* hint = new QLabel(
        QStringLiteral("按下一个组合键，用于在其他应用或游戏前台时新建桌面便签。"),
        &dialog);
    hint->setObjectName(QStringLiteral("hotkeyHint"));
    hint->setWordWrap(true);
    layout->addWidget(hint);

    auto* form = new QFormLayout;
    form->setHorizontalSpacing(16);
    auto* sequenceLabel = new QLabel(QStringLiteral("全局组合"), &dialog);
    sequenceLabel->setObjectName(QStringLiteral("hotkeyFormLabel"));
    auto* sequenceEdit = new QKeySequenceEdit(m_globalHotkey->sequence(), &dialog);
    sequenceEdit->setObjectName(QStringLiteral("hotkeySequenceEdit"));
    sequenceEdit->setMaximumSequenceLength(1);
    sequenceEdit->setClearButtonEnabled(true);
    form->addRow(sequenceLabel, sequenceEdit);
    layout->addLayout(form);

    auto* validationLabel = new QLabel(&dialog);
    validationLabel->setObjectName(QStringLiteral("hotkeyValidationLabel"));
    validationLabel->setWordWrap(true);
    validationLabel->setMinimumHeight(42);
    layout->addWidget(validationLabel);

    auto* utilityRow = new QHBoxLayout;
    auto* currentLabel = new QLabel(
        QStringLiteral("当前绑定：%1").arg(globalHotkeyText()), &dialog);
    currentLabel->setObjectName(QStringLiteral("hotkeyHint"));
    utilityRow->addWidget(currentLabel, 1);
    auto* resetButton = new QPushButton(QStringLiteral("恢复默认"), &dialog);
    resetButton->setObjectName(QStringLiteral("quietButton"));
    utilityRow->addWidget(resetButton);
    layout->addLayout(utilityRow);
    layout->addStretch(1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save
                                             | QDialogButtonBox::Cancel,
                                         &dialog);
    QPushButton* saveButton = buttons->button(QDialogButtonBox::Save);
    saveButton->setText(QStringLiteral("应用快捷键"));
    saveButton->setObjectName(QStringLiteral("hotkeyApplyButton"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    layout->addWidget(buttons);

    const auto showValidation = [validationLabel, saveButton](
                                    const QKeySequence& sequence) {
        QString validationError;
        const bool valid = GlobalHotkey::validate(sequence, &validationError);
        validationLabel->setText(
            valid ? QStringLiteral("组合有效；应用时会检查是否已被其他程序占用。")
                  : validationError);
        validationLabel->setStyleSheet(
            valid ? QString() : QStringLiteral("color: #D5847D;"));
        saveButton->setEnabled(valid);
    };
    connect(sequenceEdit, &QKeySequenceEdit::keySequenceChanged, &dialog,
            [showValidation](const QKeySequence& sequence) {
                showValidation(sequence);
            });
    connect(resetButton, &QPushButton::clicked, &dialog,
            [sequenceEdit] { sequenceEdit->setKeySequence(GlobalHotkey::defaultSequence()); });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(saveButton, &QPushButton::clicked, &dialog,
            [this, &dialog, sequenceEdit, validationLabel] {
                const QKeySequence previous = m_globalHotkey->sequence();
                const QKeySequence candidate = sequenceEdit->keySequence();
                QString error;
                if (!m_globalHotkey->setSequence(candidate, &error)) {
                    validationLabel->setText(error);
                    validationLabel->setStyleSheet(QStringLiteral("color: #D5847D;"));
                    return;
                }

                QSettings settings;
                settings.setValue(GlobalHotkey::settingsKey(),
                                  GlobalHotkey::portableText(candidate));
                settings.sync();
                if (settings.status() != QSettings::NoError) {
                    QString restoreError;
                    m_globalHotkey->setSequence(previous, &restoreError);
                    validationLabel->setText(
                        restoreError.isEmpty()
                            ? QStringLiteral("无法写入快捷键设置；已恢复原绑定。")
                            : QStringLiteral("无法写入设置；%1").arg(restoreError));
                    validationLabel->setStyleSheet(QStringLiteral("color: #D5847D;"));
                    return;
                }

                updateGlobalHotkeyPresentation();
                setStatusMessage(QStringLiteral("全局快捷键已改为 %1")
                                     .arg(globalHotkeyText()));
                dialog.accept();
            });
    showValidation(sequenceEdit->keySequence());
    dialog.exec();
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
    QAction* collectStickyAction = nullptr;
    if (item->data(Qt::UserRole + 4).toString() == QStringLiteral("sticky")) {
        openStickyAction = menu.addAction(QStringLiteral("在桌面打开"));
        collectStickyAction = menu.addAction(QStringLiteral("收舟入册…"));
        menu.addSeparator();
    }
    QAction* renameAction = menu.addAction(QStringLiteral("重命名"));
    QAction* deleteAction = menu.addAction(QStringLiteral("移到回收站"));
    QAction* selected = menu.exec(m_noteList->viewport()->mapToGlobal(position));
    if (openStickyAction && selected == openStickyAction)
        openSticky(item->data(Qt::UserRole).toLongLong());
    else if (collectStickyAction && selected == collectStickyAction)
        collectStickies();
    else if (selected == renameAction)
        renameCurrentNote();
    else if (selected == deleteAction)
        deleteCurrentNote();
}

void MainWindow::deleteCurrentNote()
{
    if (m_currentNoteId < 0)
        return;
    if (NocturneDialogs::question(this,
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
    const QString path = NocturneDialogs::getOpenFileName(
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
    QString path = NocturneDialogs::getSaveFileName(
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
    const QString name = NocturneDialogs::getText(this,
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
    const QString name = NocturneDialogs::getText(this,
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
    if (NocturneDialogs::question(
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
    m_todoSummaryLabel->setText(todos.isEmpty() ? QStringLiteral("暂无待办，留一点空白。")
        : QStringLiteral("%1 / %2  已完成").arg(completed).arg(todos.size()));
    m_todoProgress->setRange(0, qMax(1, static_cast<int>(todos.size())));
    m_todoProgress->setValue(completed);
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
    const QColor selected = NocturneDialogs::getColor(m_editor->textColor(), this, QStringLiteral("选择文字颜色"));
    if (!selected.isValid())
        return;
    QTextCharFormat format;
    format.setForeground(selected);
    m_editor->mergeCurrentCharFormat(format);
}

void MainWindow::chooseImages()
{
    const QStringList files = NocturneDialogs::getOpenFileNames(
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
                        NocturneDialogs::information(
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
    NocturneUi::setGlyph(m_maximizeButton, maximized ? NocturneUi::Glyph::Restore : NocturneUi::Glyph::Maximize);
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
    m_saveStateLabel->setToolTip(message);
    m_saveStateLabel->setStyleSheet(warning ? QStringLiteral("color: #D5847D;") : QString());
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
                                QStringLiteral("按 %1 可随时新建桌面便签。")
                                    .arg(globalHotkeyText()),
                                QSystemTrayIcon::Information,
                                2500);
        m_trayHintShown = true;
    }
}
