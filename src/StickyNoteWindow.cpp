#include "StickyNoteWindow.h"

#include "Branding.h"
#include "Database.h"
#include "WindowChrome.h"
#include "NocturneStyle.h"

#include <QCloseEvent>
#include <QCursor>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMoveEvent>
#include <QResizeEvent>
#include <QScreen>
#include <QSettings>
#include <QSignalBlocker>
#include <QSizeGrip>
#include <QSlider>
#include <QTextEdit>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidgetAction>

#include <algorithm>

namespace {
constexpr auto kLegacyGeometryKey = "quickNote/geometry";
constexpr auto kLegacyOpacityKey = "quickNote/opacity";
constexpr int kSaveDelayMs = 600;
constexpr int kCursorOffset = 18;
constexpr int kCascadeStep = 26;
constexpr int kCascadeCount = 7;
constexpr int kMinimumOpacity = 55;
constexpr int kMaximumOpacity = 100;
constexpr int kDefaultOpacity = 96;

int g_spawnSequence = 0;
bool g_legacyGeometryClaimed = false;

int boundedCoordinate(int value, int low, int high)
{
    return std::clamp(value, low, std::max(low, high));
}
}

StickyNoteWindow::StickyNoteWindow(Database* db, qint64 noteId, QWidget* parent)
    : QWidget(parent, Qt::Tool | Qt::WindowStaysOnTopHint | Qt::FramelessWindowHint)
    , db_(db)
    , editor_(new QTextEdit(this))
    , saveHint_(new QLabel(this))
    , saveTimer_(new QTimer(this))
    , noteId_(noteId)
    , spawnIndex_(g_spawnSequence++)
{
    setObjectName(QStringLiteral("stickyNoteWindow"));
    setWindowTitle(QStringLiteral("夜航便签"));
    setWindowIcon(NocturneBrand::appIcon());
    setAttribute(Qt::WA_DeleteOnClose, true);
    setMinimumSize(280, 220);
    resize(400, 340);

    auto* header = new WindowDragArea(this, false);
    header->setObjectName(QStringLiteral("stickyHeader"));
    header->setFixedHeight(48);
    auto* brandIcon = new QLabel(header);
    brandIcon->setObjectName(QStringLiteral("stickyBrandIcon"));
    brandIcon->setPixmap(NocturneBrand::appIcon().pixmap(24, 24));
    brandIcon->setFixedSize(26, 26);
    brandIcon->setAlignment(Qt::AlignCenter);
    auto* title = new QLabel(QStringLiteral("随手记"), header);
    title->setObjectName(QStringLiteral("stickyTitle"));
    saveHint_->setObjectName(QStringLiteral("saveHint"));
    saveHint_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    saveHint_->setText(QStringLiteral("已自动保存"));

    pinButton_ = new QToolButton(header);
    pinButton_->setObjectName(QStringLiteral("stickyPinButton"));
    NocturneUi::setGlyph(pinButton_, NocturneUi::Glyph::Pin);
    pinButton_->setCheckable(true);
    pinButton_->setAutoRaise(true);
    pinButton_->setFocusPolicy(Qt::NoFocus);
    pinButton_->setFixedSize(40, 48);
    pinButton_->setProperty("windowChromeInteractive", true);
    pinButton_->setAccessibleName(QStringLiteral("钉在桌面"));

    auto* settingsButton = new QToolButton(header);
    settingsButton->setObjectName(QStringLiteral("stickySettingsButton"));
    NocturneUi::setGlyph(settingsButton, NocturneUi::Glyph::More);
    settingsButton->setToolTip(QStringLiteral("便签设置"));
    settingsButton->setAutoRaise(true);
    settingsButton->setFocusPolicy(Qt::NoFocus);
    settingsButton->setFixedSize(38, 48);
    settingsButton->setProperty("windowChromeInteractive", true);
    auto* closeButton = new QToolButton(header);
    closeButton->setObjectName(QStringLiteral("stickyCloseButton"));
    NocturneUi::setGlyph(closeButton, NocturneUi::Glyph::Close);
    closeButton->setToolTip(QStringLiteral("关闭便签"));
    closeButton->setAutoRaise(true);
    closeButton->setFocusPolicy(Qt::NoFocus);
    closeButton->setFixedSize(42, 48);
    closeButton->setProperty("windowChromeInteractive", true);

    auto* heading = new QHBoxLayout(header);
    heading->setContentsMargins(13, 0, 0, 0);
    heading->setSpacing(8);
    heading->addWidget(brandIcon);
    heading->addWidget(title);
    heading->addStretch(1);

    heading->addWidget(pinButton_);
    heading->addWidget(settingsButton);
    heading->addWidget(closeButton);

    settingsMenu_ = new QMenu(this);
    settingsMenu_->setObjectName(QStringLiteral("stickySettingsMenu"));
    auto* opacityPanel = new QWidget(settingsMenu_);
    opacityPanel->setObjectName(QStringLiteral("opacityPanel"));
    auto* opacityLayout = new QVBoxLayout(opacityPanel);
    opacityLayout->setContentsMargins(14, 12, 14, 13);
    opacityLayout->setSpacing(8);
    auto* opacityHeading = new QHBoxLayout;
    opacityHeading->setContentsMargins(0, 0, 0, 0);
    auto* opacityTitle = new QLabel(QStringLiteral("便签不透明度"), opacityPanel);
    opacityTitle->setObjectName(QStringLiteral("opacityTitle"));
    opacityValueLabel_ = new QLabel(opacityPanel);
    opacityValueLabel_->setObjectName(QStringLiteral("opacityValue"));
    opacityValueLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    opacityHeading->addWidget(opacityTitle);
    opacityHeading->addStretch(1);
    opacityHeading->addWidget(opacityValueLabel_);
    opacitySlider_ = new QSlider(Qt::Horizontal, opacityPanel);
    opacitySlider_->setObjectName(QStringLiteral("opacitySlider"));
    opacitySlider_->setRange(kMinimumOpacity, kMaximumOpacity);
    opacitySlider_->setSingleStep(1);
    opacitySlider_->setPageStep(5);
    opacitySlider_->setToolTip(QStringLiteral("范围 55%—100%"));
    opacityLayout->addLayout(opacityHeading);
    opacityLayout->addWidget(opacitySlider_);
    auto* opacityHint = new QLabel(
        QStringLiteral("每枚便签独立记忆；新便签沿用最近一次设置。"), opacityPanel);
    opacityHint->setObjectName(QStringLiteral("opacityHint"));
    opacityHint->setWordWrap(true);
    opacityLayout->addWidget(opacityHint);

    auto* opacityAction = new QWidgetAction(settingsMenu_);
    opacityAction->setDefaultWidget(opacityPanel);
    settingsMenu_->addAction(opacityAction);
    settingsButton->setMenu(settingsMenu_);
    settingsButton->setPopupMode(QToolButton::InstantPopup);

    editor_->setObjectName(QStringLiteral("stickyEditor"));
    editor_->setAcceptRichText(false);
    editor_->setFont(QFont(NocturneUi::sansFamily(), 12));
    editor_->setUndoRedoEnabled(true);
    editor_->setPlaceholderText(QStringLiteral("一念入舟，随手记下…"));
    editor_->setTabChangesFocus(false);

    auto* body = new QFrame(this);
    body->setObjectName(QStringLiteral("stickyBody"));
    auto* bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(24, 24, 18, 12);
    bodyLayout->setSpacing(12);
    bodyLayout->addWidget(editor_, 1);
    auto* gripRow = new QHBoxLayout;
    gripRow->setContentsMargins(0, 0, 0, 0);
    gripRow->addWidget(saveHint_);
    gripRow->addStretch(1);
    auto* sizeGrip = new QSizeGrip(body);
    sizeGrip->setObjectName(QStringLiteral("stickySizeGrip"));
    sizeGrip->setFixedSize(16, 16);
    gripRow->addWidget(sizeGrip);
    bodyLayout->addLayout(gripRow);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(header);
    layout->addWidget(body, 1);

    saveTimer_->setSingleShot(true);
    saveTimer_->setInterval(kSaveDelayMs);
    connect(saveTimer_, &QTimer::timeout, this, &StickyNoteWindow::flushSave);
    connect(closeButton, &QToolButton::clicked, this, &StickyNoteWindow::close);
    connect(pinButton_, &QToolButton::toggled, this, &StickyNoteWindow::setPinned);

    reloadFromDatabase();
    restoreWindowSettings();
    connect(opacitySlider_, &QSlider::valueChanged, this, &StickyNoteWindow::applyOpacity);
    connect(editor_, &QTextEdit::textChanged, this, &StickyNoteWindow::markDirty);
    geometryPersistenceReady_ = true;
}

qint64 StickyNoteWindow::noteId() const
{
    return noteId_;
}

bool StickyNoteWindow::isPinned() const
{
    return pinned_;
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
    if (noteId_ > 0)
        persistWindowSettings(true);
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
    const qint64 savedId = db_->saveStickyNote(noteId_, text, &error);
    if (savedId >= 0) {
        noteId_ = savedId;
        lastSavedText_ = text;
        dirty_ = false;
        saveHint_->setText(QStringLiteral("已自动保存"));
        saveHint_->setToolTip(QString());
        if (savedId > 0) {
            persistWindowSettings(true);
            emit noteSaved(savedId);
        }
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

    std::optional<NoteRecord> record;
    QString error;
    if (noteId_ > 0 && db_)
        record = db_->note(noteId_, &error);
    if (!error.isEmpty()) {
        saveHint_->setText(QStringLiteral("读取失败"));
        saveHint_->setToolTip(error);
        return;
    }
    if (noteId_ > 0 && (!record.has_value() || record->kind != QStringLiteral("sticky"))) {
        saveHint_->setText(QStringLiteral("便签不可用"));
        saveHint_->setToolTip(QStringLiteral("对应便签不存在或已移入回收站"));
        return;
    }

    const QSignalBlocker blocker(editor_);
    lastSavedText_ = record.has_value() ? record->plainText : QString();
    editor_->setPlainText(lastSavedText_);
    dirty_ = false;
    saveHint_->setText(QStringLiteral("已自动保存"));
    saveHint_->setToolTip(QString());
}

void StickyNoteWindow::setPinned(bool pinned)
{
    if (pinned_ == pinned
        && windowFlags().testFlag(Qt::WindowStaysOnTopHint) == pinned) {
        updatePinButton();
        return;
    }

    const QRect previousGeometry = geometry();
    const bool wasVisible = isVisible();
    pinned_ = pinned;
    setWindowFlag(Qt::WindowStaysOnTopHint, pinned_);
    setGeometry(previousGeometry);
    if (wasVisible) {
        show();
        raise();
        activateWindow();
    }
    updatePinButton();
    if (noteId_ > 0)
        QSettings().setValue(settingKey(QStringLiteral("pinned")), pinned_);
}

void StickyNoteWindow::closeEvent(QCloseEvent* event)
{
    flushSave();
    if (dirty_) {
        event->ignore();
        return;
    }
    if (noteId_ > 0)
        persistWindowSettings(false);
    event->accept();
}

void StickyNoteWindow::moveEvent(QMoveEvent* event)
{
    QWidget::moveEvent(event);
    persistGeometry();
}

bool StickyNoteWindow::nativeEvent(const QByteArray& eventType, void* message, qintptr* result)
{
    if (WindowChrome::handleNativeHitTest(this, message, result, 8))
        return true;
    return QWidget::nativeEvent(eventType, message, result);
}

void StickyNoteWindow::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    persistGeometry();
}

QString StickyNoteWindow::settingKey(const QString& name) const
{
    if (noteId_ <= 0)
        return {};
    return QStringLiteral("stickyNotes/%1/%2").arg(noteId_).arg(name);
}

void StickyNoteWindow::restoreWindowSettings()
{
    QSettings settings;
    int savedOpacity = settings.value(QLatin1String(kLegacyOpacityKey),
                                      kDefaultOpacity).toInt();
    if (noteId_ > 0 && settings.contains(settingKey(QStringLiteral("opacity"))))
        savedOpacity = settings.value(settingKey(QStringLiteral("opacity"))).toInt();
    savedOpacity = std::clamp(savedOpacity, kMinimumOpacity, kMaximumOpacity);

    pinned_ = noteId_ <= 0
        || settings.value(settingKey(QStringLiteral("pinned")), true).toBool();
    setWindowFlag(Qt::WindowStaysOnTopHint, pinned_);
    {
        const QSignalBlocker blocker(pinButton_);
        pinButton_->setChecked(pinned_);
    }
    updatePinButton();

    opacitySlider_->setValue(savedOpacity);
    opacityValueLabel_->setText(QStringLiteral("%1%").arg(savedOpacity));
    setWindowOpacity(savedOpacity / 100.0);

    QByteArray savedGeometry;
    if (noteId_ > 0)
        savedGeometry = settings.value(settingKey(QStringLiteral("geometry"))).toByteArray();
    if (savedGeometry.isEmpty() && !g_legacyGeometryClaimed) {
        savedGeometry = settings.value(QLatin1String(kLegacyGeometryKey)).toByteArray();
        g_legacyGeometryClaimed = !savedGeometry.isEmpty();
    }
    if (!savedGeometry.isEmpty())
        restoredGeometry_ = restoreGeometry(savedGeometry);
}

void StickyNoteWindow::persistWindowSettings(bool open)
{
    if (noteId_ <= 0)
        return;
    QSettings settings;
    settings.setValue(settingKey(QStringLiteral("geometry")), saveGeometry());
    settings.setValue(settingKey(QStringLiteral("opacity")), opacitySlider_->value());
    settings.setValue(settingKey(QStringLiteral("pinned")), pinned_);
    settings.setValue(settingKey(QStringLiteral("open")), open);
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
        const int cascade = (spawnIndex_ % kCascadeCount) * kCascadeStep;
        target = cursorPosition + QPoint(kCursorOffset + cascade,
                                         kCursorOffset + cascade);
        if (target.x() + width() > available.right() + 1)
            target.setX(cursorPosition.x() - width() - kCursorOffset - cascade);
        if (target.y() + height() > available.bottom() + 1)
            target.setY(cursorPosition.y() - height() - kCursorOffset - cascade);
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
    if (!geometryPersistenceReady_ || noteId_ <= 0)
        return;
    QSettings().setValue(settingKey(QStringLiteral("geometry")), saveGeometry());
}

void StickyNoteWindow::markDirty()
{
    dirty_ = true;
    saveHint_->setText(QStringLiteral("等待自动保存…"));
    saveHint_->setToolTip(QString());
    saveTimer_->start();
}

void StickyNoteWindow::applyOpacity(int percent)
{
    const int bounded = std::clamp(percent, kMinimumOpacity, kMaximumOpacity);
    setWindowOpacity(bounded / 100.0);
    opacityValueLabel_->setText(QStringLiteral("%1%").arg(bounded));
    QSettings settings;
    settings.setValue(QLatin1String(kLegacyOpacityKey), bounded);
    if (noteId_ > 0)
        settings.setValue(settingKey(QStringLiteral("opacity")), bounded);
}

void StickyNoteWindow::updatePinButton()
{
    const QSignalBlocker blocker(pinButton_);
    pinButton_->setChecked(pinned_);
    pinButton_->setToolTip(pinned_
        ? QStringLiteral("已钉在桌面：保持在其他窗口之上；单击取消")
        : QStringLiteral("钉在桌面：保持在其他窗口之上"));
}
