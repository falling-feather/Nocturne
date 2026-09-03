#include "GlobalHotkey.h"

#include <QApplication>

#ifdef Q_OS_WIN
#  include <windows.h>
#endif

#include <utility>

namespace {
constexpr int kHotkeyId = 0x4E48;
constexpr int kProbeHotkeyId = 0x4E49;

QKeySequence normalizedSequence(const QKeySequence& sequence)
{
    if (sequence.count() != 1)
        return sequence;
    return QKeySequence(sequence[0]);
}

bool supportedKey(Qt::Key key)
{
    if ((key >= Qt::Key_A && key <= Qt::Key_Z)
        || (key >= Qt::Key_0 && key <= Qt::Key_9)
        || (key >= Qt::Key_F1 && key <= Qt::Key_F24)) {
        return true;
    }
    switch (key) {
    case Qt::Key_Space:
    case Qt::Key_Tab:
    case Qt::Key_Backspace:
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Escape:
    case Qt::Key_Insert:
    case Qt::Key_Delete:
    case Qt::Key_Home:
    case Qt::Key_End:
    case Qt::Key_Left:
    case Qt::Key_Up:
    case Qt::Key_Right:
    case Qt::Key_Down:
    case Qt::Key_PageUp:
    case Qt::Key_PageDown:
        return true;
    default:
        return false;
    }
}

#ifdef Q_OS_WIN
UINT nativeVirtualKey(Qt::Key key)
{
    if (key >= Qt::Key_A && key <= Qt::Key_Z)
        return static_cast<UINT>('A' + (key - Qt::Key_A));
    if (key >= Qt::Key_0 && key <= Qt::Key_9)
        return static_cast<UINT>('0' + (key - Qt::Key_0));
    if (key >= Qt::Key_F1 && key <= Qt::Key_F24)
        return static_cast<UINT>(VK_F1 + (key - Qt::Key_F1));
    switch (key) {
    case Qt::Key_Space: return VK_SPACE;
    case Qt::Key_Tab: return VK_TAB;
    case Qt::Key_Backspace: return VK_BACK;
    case Qt::Key_Return: return VK_RETURN;
    case Qt::Key_Enter: return VK_RETURN;
    case Qt::Key_Escape: return VK_ESCAPE;
    case Qt::Key_Insert: return VK_INSERT;
    case Qt::Key_Delete: return VK_DELETE;
    case Qt::Key_Home: return VK_HOME;
    case Qt::Key_End: return VK_END;
    case Qt::Key_Left: return VK_LEFT;
    case Qt::Key_Up: return VK_UP;
    case Qt::Key_Right: return VK_RIGHT;
    case Qt::Key_Down: return VK_DOWN;
    case Qt::Key_PageUp: return VK_PRIOR;
    case Qt::Key_PageDown: return VK_NEXT;
    default: return 0;
    }
}

UINT nativeModifiers(Qt::KeyboardModifiers modifiers)
{
    UINT native = MOD_NOREPEAT;
    if (modifiers.testFlag(Qt::ControlModifier))
        native |= MOD_CONTROL;
    if (modifiers.testFlag(Qt::AltModifier))
        native |= MOD_ALT;
    if (modifiers.testFlag(Qt::ShiftModifier))
        native |= MOD_SHIFT;
    if (modifiers.testFlag(Qt::MetaModifier))
        native |= MOD_WIN;
    return native;
}
#endif
} // namespace

GlobalHotkey::GlobalHotkey(WId windowId,
                           const QKeySequence& sequence,
                           std::function<void()> callback,
                           QObject* parent)
    : QObject(parent)
    , m_windowId(windowId)
    , m_callback(std::move(callback))
{
    qApp->installNativeEventFilter(this);
    QString ignoredError;
    if (!setSequence(sequence, &ignoredError))
        m_sequence = normalizedSequence(sequence);
}

GlobalHotkey::~GlobalHotkey()
{
    if (m_registered)
        unregisterSequence(kHotkeyId);
    if (qApp)
        qApp->removeNativeEventFilter(this);
}

bool GlobalHotkey::isRegistered() const
{
    return m_registered;
}

QKeySequence GlobalHotkey::sequence() const
{
    return m_sequence;
}

QKeySequence GlobalHotkey::defaultSequence()
{
    return QKeySequence(QStringLiteral("Ctrl+Alt+N"), QKeySequence::PortableText);
}

QString GlobalHotkey::settingsKey()
{
    return QStringLiteral("shortcuts/newSticky");
}

QString GlobalHotkey::portableText(const QKeySequence& sequence)
{
    return normalizedSequence(sequence).toString(QKeySequence::PortableText);
}

QString GlobalHotkey::displayText(const QKeySequence& sequence)
{
    QString text = normalizedSequence(sequence).toString(QKeySequence::NativeText);
    if (text.isEmpty())
        text = portableText(sequence);
    return text;
}

bool GlobalHotkey::validate(const QKeySequence& sequence, QString* error)
{
    if (error)
        error->clear();
    if (sequence.count() != 1 || sequence[0].key() == Qt::Key_unknown) {
        if (error)
            *error = QStringLiteral("请输入一个单段快捷键组合。");
        return false;
    }

    const QKeyCombination combination = sequence[0];
    const Qt::KeyboardModifiers modifiers = combination.keyboardModifiers();
    constexpr Qt::KeyboardModifiers allowedModifiers = Qt::ControlModifier
        | Qt::AltModifier | Qt::ShiftModifier | Qt::MetaModifier;
    if ((modifiers & allowedModifiers) == Qt::NoModifier) {
        if (error)
            *error = QStringLiteral("全局快捷键至少需要 Ctrl、Alt、Shift 或 Win 中的一项。");
        return false;
    }
    if ((modifiers & ~allowedModifiers) != Qt::NoModifier) {
        if (error)
            *error = QStringLiteral("当前不支持数字小键盘或布局切换修饰键。");
        return false;
    }

    const Qt::Key key = combination.key();
    if (!supportedKey(key)) {
        if (error)
            *error = QStringLiteral("请使用字母、数字、F1—F24 或常用导航键作为主键。");
        return false;
    }
    return true;
}

bool GlobalHotkey::setSequence(const QKeySequence& requested, QString* error)
{
    if (error)
        error->clear();
    QString validationError;
    if (!validate(requested, &validationError)) {
        if (error)
            *error = validationError;
        return false;
    }

    const QKeySequence candidate = normalizedSequence(requested);
    if (m_registered && candidate == m_sequence)
        return true;

#ifndef Q_OS_WIN
    if (error)
        *error = QStringLiteral("当前平台尚未实现全局快捷键注册。");
    return false;
#else
    if (!registerSequence(candidate, kProbeHotkeyId)) {
        if (error) {
            *error = QStringLiteral("%1 已被其他程序或窗口占用；当前绑定没有改变。")
                         .arg(displayText(candidate));
        }
        return false;
    }
    unregisterSequence(kProbeHotkeyId);

    const QKeySequence previous = m_sequence;
    const bool previousRegistered = m_registered;
    if (previousRegistered)
        unregisterSequence(kHotkeyId);

    if (registerSequence(candidate, kHotkeyId)) {
        m_sequence = candidate;
        m_registered = true;
        return true;
    }

    m_sequence = previous;
    m_registered = previousRegistered && registerSequence(previous, kHotkeyId);
    if (error) {
        *error = m_registered
            ? QStringLiteral("%1 在切换时被占用；已恢复原快捷键 %2。")
                  .arg(displayText(candidate), displayText(previous))
            : QStringLiteral("%1 注册失败，且原快捷键未能恢复；请重新选择。")
                  .arg(displayText(candidate));
    }
    return false;
#endif
}

bool GlobalHotkey::registerSequence(const QKeySequence& sequence, int id) const
{
#ifndef Q_OS_WIN
    Q_UNUSED(sequence)
    Q_UNUSED(id)
    return false;
#else
    const QKeyCombination combination = sequence[0];
    const UINT virtualKey = nativeVirtualKey(combination.key());
    return virtualKey != 0
        && RegisterHotKey(reinterpret_cast<HWND>(m_windowId),
                          id,
                          nativeModifiers(combination.keyboardModifiers()),
                          virtualKey);
#endif
}

void GlobalHotkey::unregisterSequence(int id) const
{
#ifdef Q_OS_WIN
    UnregisterHotKey(reinterpret_cast<HWND>(m_windowId), id);
#else
    Q_UNUSED(id)
#endif
}

bool GlobalHotkey::nativeEventFilter(const QByteArray& eventType,
                                     void* message,
                                     qintptr* result)
{
    Q_UNUSED(eventType)
    Q_UNUSED(result)
#ifdef Q_OS_WIN
    const MSG* nativeMessage = static_cast<const MSG*>(message);
    if (nativeMessage && nativeMessage->message == WM_HOTKEY
        && nativeMessage->hwnd == reinterpret_cast<HWND>(m_windowId)
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
