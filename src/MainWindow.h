#pragma once

#include "Database.h"
#include "SourceFile.h"

#include <QCache>
#include <QList>
#include <QMainWindow>

class GlobalHotkey;
class NoteEditor;
class StickyNoteWindow;
class NotebookTree;
class FolderComboBox;
class DocumentOutline;

class QAction;
class QCloseEvent;
class QComboBox;
class QEvent;
class QResizeEvent;
class QFrame;
class QProgressBar;
class QVBoxLayout;
class QStackedWidget;
class QImage;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QMenu;
class QMenuBar;
class QPoint;
class QPushButton;
class QSystemTrayIcon;
class QThread;
class QTimer;
class QToolButton;
class QPlainTextEdit;
class FindBar;

class MainWindow final : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(Database* database, QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void changeEvent(QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;

private:
    void buildUi();
    void buildMenus();
    void buildTray();
    void applyTheme();
    void chooseTheme(const QString& id);
    void toggleFocusMode();
    void toggleTodoPanel();
    void updateAdaptiveLayout();
    void updateDocumentInfo();
    void connectSignals();
    void restoreWindowState();
    void buildWorkspaceUi();
    void loadSourceState(qint64 noteId, const NoteRecord& note);
    void toggleSourceView();
    void renderSourcePreview();
    void convertCurrentNoteMath();
    void compareSource();
    void reloadSource();
    void relinkSource();
    void relinkSelectedDirectory();
    void configureSelectedRefreshRoot();
    void refreshLinkedFolders();
    void showHistory();
    void showRecovery();
    void showTemplates();
    void captureSelection();
    void showAiHandoff();
    void showVoiceInput();
    void rememberReadingPosition();
    void navigateHistory(int delta);
    void openNoteById(qint64 id);
    bool keepRecoveryDraft(const QString& html, const QString& plainText);

    void ensureFirstNote();
    void refreshNotes(qint64 preferredId = -1);
    void loadNote(qint64 noteId);
    bool saveCurrentNote(bool force = false);
    void scheduleSave();
    void createNote();
    void collectStickies();
    void configureGlobalHotkey();
    void renameCurrentNote();
    void deleteCurrentNote();
    void showNoteContextMenu(const QPoint& position);
    void importDocument();
    void importFolder();
    void runImport(const QStringList& paths, qint64 parentFolder = -1);
    void setSelectionTodo();
    void locateTodo(QListWidgetItem* item);
    void navigateToTodo(qint64 noteId, const QString& anchor);
    void showTodoDetails(qint64 todoId);
    void showTodoContextMenu(const QPoint& position);
    bool deleteSingleTodo(qint64 todoId);
    void insertMarkdown();
    void moveSelectedFolder();
    void exportDocument();

    void refreshFolders(qint64 preferredFilter = Database::AllFolders);
    void createFolder(qint64 parentId = 0);
    void renameSelectedFolder();
    void deleteSelectedFolder();
    void moveCurrentNoteToSelectedFolder();
    qint64 selectedFolderFilter() const;

    void refreshTodos();
    void addTodo();
    void clearCompletedTodos();

    void toggleBold();
    void toggleItalic();
    void toggleUnderline();
    void applyHeading();
    void applyBodyStyle();
    void toggleList(bool numbered);
    void applyFontSize(const QString& text);
    void pickTextColor();

    void chooseImages();
    void insertImagesFromFiles(const QStringList& files);
    void insertImage(const QImage& image, const QString& sourceName = QString());
    QString saveImageAttachment(const QImage& image, const QString& sourceName, QString* error);

    void cacheNote(const NoteRecord& note);
    int summaryRevision(qint64 noteId) const;
    void selectCurrentFolderInEditor();
    void suspendHeavyContent();
    void resumeHeavyContent();

    StickyNoteWindow* openSticky(qint64 noteId = 0, bool activate = true);
    void restorePinnedStickies();
    void summonSticky();
    void showMainWindow();
    void scheduleAutomaticBackup();
    void startBackup(bool automatic);
    void openBackupDirectory();
    void updateGlobalHotkeyPresentation();
    QString globalHotkeyText() const;
    void requestQuit();
    void toggleMaximized();
    void updateWindowChrome();
    void updateFormatControls();
    void setStatusMessage(const QString& message, bool warning = false);

    Database* m_database = nullptr;
    QAction* m_convertMathAction = nullptr;
    qint64 m_currentNoteId = -1;
    qint64 m_currentFolderId = Database::UnfiledFolder;
    QString m_currentNoteKind = QStringLiteral("note");
    int m_currentBodyRevision = 0;
    QByteArray m_currentContentHash;
    QDateTime m_currentCreatedAt;
    bool m_loadingNote = false;
    bool m_loadingFolders = false;
    bool m_loadingTodos = false;
    bool m_dirty = false;
    bool m_quitting = false;
    bool m_trayHintShown = false;
    bool m_contentSuspended = false;
    bool m_resumeQueued = false;
    bool m_quitAfterBackup = false;

    QCache<qint64, NoteRecord> m_noteCache;

    QMenuBar* m_appMenuBar = nullptr;
    QFrame* m_navigation = nullptr;
    QFrame* m_todoPane = nullptr;
    QFrame* m_formatBar = nullptr;
    QWidget* m_writingColumn = nullptr;
    QVBoxLayout* m_writingLayout = nullptr;
    QStackedWidget* m_documentStack = nullptr;
    QLabel* m_emptyHeading = nullptr;
    QLabel* m_emptyHint = nullptr;
    QLabel* m_noteMeta = nullptr;
    QLabel* m_wordCountLabel = nullptr;
    QProgressBar* m_todoProgress = nullptr;
    QToolButton* m_focusButton = nullptr;
    QToolButton* m_todoToggleButton = nullptr;
    QToolButton* m_railTodoButton = nullptr;
    QToolButton* m_themeButton = nullptr;
    QToolButton* m_settingsButton = nullptr;
    QToolButton* m_openStickyButton = nullptr;
    QAction* m_focusAction = nullptr;
    QList<QAction*> m_themeActions;
    bool m_focusMode = false;
    bool m_todoRequested = true;
    bool m_narrowTodoOverride = false;
    QToolButton* m_maximizeButton = nullptr;
    NotebookTree* m_noteList = nullptr;
    qint64 m_treeFolderId = Database::AllFolders;
    QToolButton* m_headingButton = nullptr;
    QLineEdit* m_searchEdit = nullptr;
    FolderComboBox* m_folderFilter = nullptr;
    QPushButton* m_folderManageButton = nullptr;
    QPushButton* m_newNoteButton = nullptr;
    QPushButton* m_stickyButton = nullptr;
    QLabel* m_noteCountLabel = nullptr;

    QLineEdit* m_titleEdit = nullptr;
    FolderComboBox* m_noteFolderCombo = nullptr;
    DocumentOutline* m_outline = nullptr;
    QToolButton* m_outlineButton = nullptr;
    bool m_outlineRequested = false;
    NoteEditor* m_editor = nullptr;
    QStackedWidget* m_bodyStack = nullptr;
    QPlainTextEdit* m_sourceEditor = nullptr;
    QFrame* m_sourceBar = nullptr;
    QLabel* m_sourceState = nullptr;
    QToolButton* m_sourceToggle = nullptr;
    FindBar* m_findBar = nullptr;
    std::optional<LinkedSourceRecord> m_linkedSource;
    std::optional<SourceSnapshot> m_sourceSnapshot;
    QString m_sourceLoadError;
    QThread* m_sourceRefresh = nullptr;
    QString m_viewMode = QStringLiteral("all");
    QComboBox* m_todoFolderFilter = nullptr;
    QList<qint64> m_navigationHistory;
    int m_navigationIndex = -1;
    bool m_historyJump = false;
    QToolButton* m_backButton = nullptr;
    QToolButton* m_forwardButton = nullptr;
    QToolButton* m_boldButton = nullptr;
    QToolButton* m_italicButton = nullptr;
    QToolButton* m_underlineButton = nullptr;
    QToolButton* m_alignLeftButton = nullptr;
    QToolButton* m_alignCenterButton = nullptr;
    QToolButton* m_alignRightButton = nullptr;
    QToolButton* m_imageToolsButton = nullptr;
    QComboBox* m_fontSizeCombo = nullptr;
    QLabel* m_saveStateLabel = nullptr;

    QLineEdit* m_todoInput = nullptr;
    QListWidget* m_todoList = nullptr;
    QLabel* m_todoSummaryLabel = nullptr;

    QTimer* m_saveTimer = nullptr;
    QTimer* m_searchTimer = nullptr;
    QList<StickyNoteWindow*> m_stickyWindows;
    QSystemTrayIcon* m_trayIcon = nullptr;
    QMenu* m_trayMenu = nullptr;
    QAction* m_trayStickyAction = nullptr;
    GlobalHotkey* m_globalHotkey = nullptr;
    QThread* m_backupThread = nullptr;
};
