#pragma once

#include <QMainWindow>

class Database;
class GlobalHotkey;
class NoteEditor;
class StickyNoteWindow;

class QAction;
class QCloseEvent;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QMenu;
class QPushButton;
class QSystemTrayIcon;
class QTimer;
class QToolButton;

class MainWindow final : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(Database* database, QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* event) override;

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
    void deleteCurrentNote();
    void importDocument();
    void exportDocument();

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

    void summonSticky();
    void showMainWindow();
    void requestQuit();
    void updateFormatControls();
    void setStatusMessage(const QString& message, bool warning = false);

    Database* m_database = nullptr;
    qint64 m_currentNoteId = -1;
    bool m_loadingNote = false;
    bool m_loadingTodos = false;
    bool m_dirty = false;
    bool m_quitting = false;
    bool m_trayHintShown = false;

    QListWidget* m_noteList = nullptr;
    QLineEdit* m_searchEdit = nullptr;
    QPushButton* m_newNoteButton = nullptr;
    QPushButton* m_stickyButton = nullptr;
    QLabel* m_noteCountLabel = nullptr;

    QLineEdit* m_titleEdit = nullptr;
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
    StickyNoteWindow* m_stickyWindow = nullptr;
    QSystemTrayIcon* m_trayIcon = nullptr;
    QMenu* m_trayMenu = nullptr;
    GlobalHotkey* m_globalHotkey = nullptr;
};

