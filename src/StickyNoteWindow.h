#pragma once

#include <QString>
#include <QWidget>

class Database;
class QLabel;
class QCloseEvent;
class QMoveEvent;
class QResizeEvent;
class QTextEdit;
class QTimer;

class StickyNoteWindow final : public QWidget
{
    Q_OBJECT

public:
    explicit StickyNoteWindow(Database* db, QWidget* parent = nullptr);

public slots:
    void summon();
    void flushSave();

protected:
    void closeEvent(QCloseEvent* event) override;
    void moveEvent(QMoveEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void prepareFirstSummon();
    void persistGeometry();
    void markDirty();

    Database* db_ = nullptr;
    QTextEdit* editor_ = nullptr;
    QLabel* saveHint_ = nullptr;
    QTimer* saveTimer_ = nullptr;
    QString lastSavedText_;
    bool dirty_ = false;
    bool firstSummon_ = true;
    bool restoredGeometry_ = false;
    bool geometryPersistenceReady_ = false;
};

