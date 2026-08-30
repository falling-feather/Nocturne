#include "StickyNoteWindow.h"

#include "Database.h"

#include <QCloseEvent>
#include <QCursor>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMoveEvent>
#include <QResizeEvent>
#include <QScreen>
#include <QSettings>
#include <QSignalBlocker>
#include <QTextEdit>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace {
constexpr auto kGeometryKey = "quickNote/geometry";
constexpr int kSaveDelayMs = 600;
constexpr int kCursorOffset = 18;

int boundedCoordinate(int value, int low, int high)
{
    return std::clamp(value, low, std::max(low, high));
}
}

StickyNoteWindow::StickyNoteWindow(Database* db, QWidget* parent)
    : QWidget(parent, Qt::Tool | Qt::WindowStaysOnTopHint)
    , db_(db)
    , editor_(new QTextEdit(this))
    , saveHint_(new QLabel(this))
    , saveTimer_(new QTimer(this))
{
    setObjectName(QStringLiteral("stickyNoteWindow"));
    setWindowTitle(QStringLiteral("FeatherNote - 快速便签"));
    setAttribute(Qt::WA_DeleteOnClose, false);
    setMinimumSize(260, 200);
    resize(380, 320);

    auto* title = new QLabel(QStringLiteral("快速便签"), this);
    title->setObjectName(QStringLiteral("stickyTitle"));
    saveHint_->setObjectName(QStringLiteral("saveHint"));
    saveHint_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    saveHint_->setText(QStringLiteral("已自动保存"));

    auto* heading = new QHBoxLayout;
    heading->setContentsMargins(0, 0, 0, 0);
    heading->setSpacing(8);
    heading->addWidget(title);
    heading->addStretch(1);
    heading->addWidget(saveHint_);

    editor_->setAcceptRichText(false);
    editor_->setUndoRedoEnabled(true);
    editor_->setPlaceholderText(QStringLiteral("随手记下灵感、待办或临时信息…"));
    editor_->setTabChangesFocus(false);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 10, 12, 12);
    layout->setSpacing(7);
    layout->addLayout(heading);
    layout->addWidget(editor_, 1);

    setStyleSheet(QStringLiteral(R"(
        QWidget#stickyNoteWindow {
            background-color: #fff3ad;
        }
        QLabel#stickyTitle {
            color: #4b421f;
            font-size: 13px;
            font-weight: 600;
        }
        QLabel#saveHint {
            color: #81764d;
            font-size: 11px;
        }
        QTextEdit {
            color: #302b19;
            background-color: #fff9cf;
            border: 1px solid #ddcf82;
            border-radius: 6px;
            padding: 7px;
            selection-background-color: #e6c95c;
        }
        QTextEdit:focus {
            border-color: #bba544;
        }
    )"));

    saveTimer_->setSingleShot(true);
    saveTimer_->setInterval(kSaveDelayMs);
    connect(saveTimer_, &QTimer::timeout, this, &StickyNoteWindow::flushSave);

    reloadFromDatabase();

    connect(editor_, &QTextEdit::textChanged, this, &StickyNoteWindow::markDirty);

    const QByteArray savedGeometry = QSettings().value(QLatin1String(kGeometryKey)).toByteArray();
    if (!savedGeometry.isEmpty())
        restoredGeometry_ = restoreGeometry(savedGeometry);
    geometryPersistenceReady_ = true;
}

void StickyNoteWindow::summon()
{
    if (firstSummon_) {
        prepareFirstSummon();
        firstSummon_ = false;
    }

    show();
    raise();
    activateWindow();
    editor_->setFocus(Qt::ShortcutFocusReason);
}

void StickyNoteWindow::flushSave()
{
    saveTimer_->stop();

    const QString text = editor_->toPlainText();
    if (!dirty_ || text == lastSavedText_) {
        dirty_ = false;
        saveHint_->setText(QStringLiteral("已自动保存"));
        saveHint_->setToolTip(QString());
        return;
    }

    if (!db_) {
        saveHint_->setText(QStringLiteral("无法保存"));
        saveHint_->setToolTip(QStringLiteral("数据库尚未初始化"));
        return;
    }

    QString error;
    const qint64 savedId = db_->saveStickyNote(text, &error);
    if (savedId >= 0) {
        noteId_ = savedId;
        lastSavedText_ = text;
        dirty_ = false;
        saveHint_->setText(QStringLiteral("已自动保存"));
        saveHint_->setToolTip(QString());
        if (savedId > 0)
            emit noteSaved(savedId);
        return;
    }

    saveHint_->setText(QStringLiteral("保存失败"));
    saveHint_->setToolTip(error);
}

void StickyNoteWindow::reloadFromDatabase()
{
    if (dirty_) {
        flushSave();
        if (dirty_)
            return;
    }
    saveTimer_->stop();

    QString error;
    const std::optional<NoteRecord> record = db_ ? db_->stickyNote(&error) : std::nullopt;
    if (!error.isEmpty()) {
        saveHint_->setText(QStringLiteral("读取失败"));
        saveHint_->setToolTip(error);
        return;
    }

    const QSignalBlocker blocker(editor_);
    noteId_ = record.has_value() ? record->id : 0;
    lastSavedText_ = record.has_value() ? record->plainText : QString();
    editor_->setPlainText(lastSavedText_);
    dirty_ = false;
    saveHint_->setText(QStringLiteral("已自动保存"));
    saveHint_->setToolTip(QString());
}

void StickyNoteWindow::closeEvent(QCloseEvent* event)
{
    flushSave();
    hide();
    event->ignore();
}

void StickyNoteWindow::moveEvent(QMoveEvent* event)
{
    QWidget::moveEvent(event);
    persistGeometry();
}

void StickyNoteWindow::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    persistGeometry();
}

void StickyNoteWindow::prepareFirstSummon()
{
    const QPoint cursorPosition = QCursor::pos();
    QScreen* screen = QGuiApplication::screenAt(cursorPosition);
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    if (!screen)
        return;

    const QRect available = screen->availableGeometry();
    if (width() > available.width() || height() > available.height()) {
        resize(std::min(width(), available.width()),
               std::min(height(), available.height()));
    }

    const QRect currentGeometry(pos(), size());
    QPoint target = currentGeometry.topLeft();
    if (!restoredGeometry_ || !available.intersects(currentGeometry)) {
        target = cursorPosition + QPoint(kCursorOffset, kCursorOffset);
        if (target.x() + width() > available.right() + 1)
            target.setX(cursorPosition.x() - width() - kCursorOffset);
        if (target.y() + height() > available.bottom() + 1)
            target.setY(cursorPosition.y() - height() - kCursorOffset);
    }

    target.setX(boundedCoordinate(target.x(),
                                  available.left(),
                                  available.right() - width() + 1));
    target.setY(boundedCoordinate(target.y(),
                                  available.top(),
                                  available.bottom() - height() + 1));
    move(target);
    persistGeometry();
}

void StickyNoteWindow::persistGeometry()
{
    if (!geometryPersistenceReady_)
        return;
    QSettings().setValue(QLatin1String(kGeometryKey), saveGeometry());
}

void StickyNoteWindow::markDirty()
{
    dirty_ = true;
    saveHint_->setText(QStringLiteral("等待自动保存…"));
    saveHint_->setToolTip(QString());
    saveTimer_->start();
}
