#pragma once

#include <QString>
#include <QWidget>

class Database;
class QLabel;
class QCloseEvent;
class QMenu;
class QMoveEvent;
class QResizeEvent;
class QSlider;
class QTextEdit;
class QTimer;
class QToolButton;

class StickyNoteWindow final : public QWidget
{
    Q_OBJECT

public:
    explicit StickyNoteWindow(Database* db,
                              qint64 noteId = 0,
                              QWidget* parent = nullptr);

    qint64 noteId() const;
    bool isPinned() const;

public slots:
    void summon();
    void flushSave();
    void reloadFromDatabase();
    void setPinned(bool pinned);

signals:
    void noteSaved(qint64 noteId);

protected:
    void closeEvent(QCloseEvent* event) override;
    void moveEvent(QMoveEvent* event) override;
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    QString settingKey(const QString& name) const;
    void restoreWindowSettings();
    void persistWindowSettings(bool open);
    void prepareFirstSummon();
    void persistGeometry();
    void markDirty();
    void applyOpacity(int percent);
    void updatePinButton();

    Database* db_ = nullptr;
    QTextEdit* editor_ = nullptr;
    QLabel* saveHint_ = nullptr;
    QLabel* opacityValueLabel_ = nullptr;
    QSlider* opacitySlider_ = nullptr;
    QToolButton* pinButton_ = nullptr;
    QMenu* settingsMenu_ = nullptr;
    QTimer* saveTimer_ = nullptr;
    qint64 noteId_ = 0;
    QString lastSavedText_;
    bool dirty_ = false;
    bool firstSummon_ = true;
    bool restoredGeometry_ = false;
    bool geometryPersistenceReady_ = false;
    bool pinned_ = true;
    int spawnIndex_ = 0;
};
