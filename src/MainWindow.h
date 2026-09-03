#pragma once

#include "Database.h"

#include <QCache>
#include <QList>
#include <QMainWindow>

class GlobalHotkey;
class NoteEditor;
class StickyNoteWindow;

class QAction;
class QCloseEvent;
class QComboBox;
class QEvent;
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

class MainWindow final : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(Database* database, QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void changeEvent(QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;

private:
    void buildUi();
    void buildMenus();
    void buildTray();
    void applyTheme();
    void connectSignals();
    void restoreWindowState();

    void ensureFirstNote();
    void refreshNotes(qint64 preferredId = -1);
    void loadNote(qint64 noteId);
    bool saveCurrentNote(bool force = false);
    void scheduleSave();
    void createNote();
    void collectStickies();
    void renameCurrentNote();
    void deleteCurrentNote();
    void showNoteContextMenu(const QPoint& position);
    void importDocument();
    void exportDocument();

    void refreshFolders(qint64 preferredFilter = Database::AllFolders);
    void createFolder();
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
    void requestQuit();
    void toggleMaximized();
    void updateWindowChrome();
    void updateFormatControls();
    void setStatusMessage(const QString& message, bool warning = false);

    Database* m_database = nullptr;
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
    QToolButton* m_maximizeButton = nullptr;
    QListWidget* m_noteList = nullptr;
    QLineEdit* m_searchEdit = nullptr;
    QComboBox* m_folderFilter = nullptr;
    QPushButton* m_folderManageButton = nullptr;
    QPushButton* m_newNoteButton = nullptr;
    QPushButton* m_stickyButton = nullptr;
    QLabel* m_noteCountLabel = nullptr;

    QLineEdit* m_titleEdit = nullptr;
    QComboBox* m_noteFolderCombo = nullptr;
    NoteEditor* m_editor = nullptr;
    QToolButton* m_boldButton = nullptr;
    QToolButton* m_italicButton = nullptr;
    QToolButton* m_underlineButton = nullptr;
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
    GlobalHotkey* m_globalHotkey = nullptr;
    QThread* m_backupThread = nullptr;
};
