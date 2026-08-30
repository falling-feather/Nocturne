#include "StickyNoteWindow.h"

#include "Branding.h"
#include "Database.h"

#include <QCloseEvent>
#include <QCursor>
#include <QFrame>
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
    setWindowTitle(QStringLiteral("快速便签"));
    setWindowIcon(NocturneBrand::appIcon());
    setAttribute(Qt::WA_DeleteOnClose, false);
    setMinimumSize(280, 220);
    resize(400, 340);

    auto* header = new QFrame(this);
    header->setObjectName(QStringLiteral("stickyHeader"));
    auto* brandIcon = new QLabel(header);
    brandIcon->setObjectName(QStringLiteral("stickyBrandIcon"));
    brandIcon->setPixmap(NocturneBrand::appIcon().pixmap(26, 26));
    brandIcon->setFixedSize(28, 28);
    brandIcon->setAlignment(Qt::AlignCenter);
    auto* title = new QLabel(QStringLiteral("夜航便签"), header);
    title->setObjectName(QStringLiteral("stickyTitle"));
    saveHint_->setObjectName(QStringLiteral("saveHint"));
    saveHint_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    saveHint_->setText(QStringLiteral("已自动保存"));

    auto* heading = new QHBoxLayout(header);
    heading->setContentsMargins(12, 9, 12, 9);
    heading->setSpacing(9);
    heading->addWidget(brandIcon);
    heading->addWidget(title);
    heading->addStretch(1);
    heading->addWidget(saveHint_);

    editor_->setAcceptRichText(false);
    editor_->setUndoRedoEnabled(true);
    editor_->setPlaceholderText(QStringLiteral("一念入舟，随手记下…"));
    editor_->setTabChangesFocus(false);

    auto* body = new QFrame(this);
    body->setObjectName(QStringLiteral("stickyBody"));
    auto* bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(12, 12, 12, 12);
    bodyLayout->addWidget(editor_, 1);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(header);
    layout->addWidget(body, 1);

    setStyleSheet(QStringLiteral(R"(
        QWidget#stickyNoteWindow {
            background-color: #172238;
        }
        QFrame#stickyHeader {
            background-color: #172238;
            border-bottom: 1px solid #344560;
        }
        QFrame#stickyBody {
            background-color: #EEE5D5;
        }
        QLabel#stickyBrandIcon {
            background: transparent;
        }
        QLabel#stickyTitle {
            color: #F1D7A2;
            font-size: 14px;
            font-weight: 700;
        }
        QLabel#saveHint {
            color: #9EABC0;
            font-size: 11px;
        }
        QTextEdit {
            color: #1C2738;
            background-color: #FBF7EE;
            border: 1px solid #D0C3AC;
            border-radius: 9px;
            padding: 10px;
            selection-background-color: #A55346;
            selection-color: #FFF9ED;
            font-size: 11pt;
        }
        QTextEdit:focus {
            border-color: #A55346;
        }
        QScrollBar:vertical {
            background: transparent;
            width: 9px;
            margin: 2px;
        }
        QScrollBar::handle:vertical {
            background: #BBB09F;
            min-height: 24px;
            border-radius: 4px;
        }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {
            height: 0;
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
