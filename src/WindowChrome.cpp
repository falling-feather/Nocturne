#include "WindowChrome.h"

#include <QMouseEvent>
#include <QWindow>

#ifdef Q_OS_WIN
#  include <windows.h>
#  include <windowsx.h>
#endif

WindowDragArea::WindowDragArea(QWidget* parent, bool doubleClickMaximizes)
    : QFrame(parent)
    , doubleClickMaximizes_(doubleClickMaximizes)
{
    setProperty("windowDragArea", true);
}

void WindowDragArea::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        QFrame::mousePressEvent(event);
        return;
    }

    QWidget* topLevel = window();
    fallbackOffset_ = event->globalPosition().toPoint() - topLevel->frameGeometry().topLeft();
    fallbackDragging_ = true;

    if (QWindow* handle = topLevel->windowHandle(); handle && handle->startSystemMove())
        fallbackDragging_ = false;

    event->accept();
}

void WindowDragArea::mouseMoveEvent(QMouseEvent* event)
{
    if (fallbackDragging_ && (event->buttons() & Qt::LeftButton)) {
        QWidget* topLevel = window();
        if (!topLevel->isMaximized())
            topLevel->move(event->globalPosition().toPoint() - fallbackOffset_);
        event->accept();
        return;
    }
    QFrame::mouseMoveEvent(event);
}

void WindowDragArea::mouseReleaseEvent(QMouseEvent* event)
{
    fallbackDragging_ = false;
    QFrame::mouseReleaseEvent(event);
}

void WindowDragArea::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (doubleClickMaximizes_ && event->button() == Qt::LeftButton) {
        QWidget* topLevel = window();
        topLevel->isMaximized() ? topLevel->showNormal() : topLevel->showMaximized();
        event->accept();
        return;
    }
    QFrame::mouseDoubleClickEvent(event);
}

bool WindowChrome::handleNativeHitTest(QWidget* window,
                                       void* message,
                                       qintptr* result,
                                       int logicalResizeBorder)
{
#ifdef Q_OS_WIN
    if (!window || !message || !result)
        return false;

    const MSG* nativeMessage = static_cast<const MSG*>(message);
    if (nativeMessage->message != WM_NCHITTEST)
        return false;

    const QPoint globalPoint(GET_X_LPARAM(nativeMessage->lParam),
                             GET_Y_LPARAM(nativeMessage->lParam));

    if (!window->isMaximized() && !window->isFullScreen()) {
        RECT nativeRect{};
        if (GetWindowRect(nativeMessage->hwnd, &nativeRect)) {
            const int border = qMax(4, qRound(logicalResizeBorder * window->devicePixelRatioF()));
            const bool left = globalPoint.x() >= nativeRect.left
                && globalPoint.x() < nativeRect.left + border;
            const bool right = globalPoint.x() < nativeRect.right
                && globalPoint.x() >= nativeRect.right - border;
            const bool top = globalPoint.y() >= nativeRect.top
                && globalPoint.y() < nativeRect.top + border;
            const bool bottom = globalPoint.y() < nativeRect.bottom
                && globalPoint.y() >= nativeRect.bottom - border;

            if (top && left)
                *result = HTTOPLEFT;
            else if (top && right)
                *result = HTTOPRIGHT;
            else if (bottom && left)
                *result = HTBOTTOMLEFT;
            else if (bottom && right)
                *result = HTBOTTOMRIGHT;
            else if (left)
                *result = HTLEFT;
            else if (right)
                *result = HTRIGHT;
            else if (top)
                *result = HTTOP;
            else if (bottom)
                *result = HTBOTTOM;
            else
                *result = HTCLIENT;

            if (*result != HTCLIENT)
                return true;
        }
    }

    QWidget* hit = window->childAt(window->mapFromGlobal(globalPoint));
    for (QWidget* current = hit; current && current != window;
         current = current->parentWidget()) {
        if (current->property("windowChromeInteractive").toBool()) {
            *result = HTCLIENT;
            return true;
        }
        if (current->property("windowDragArea").toBool()) {
            *result = HTCAPTION;
            return true;
        }
    }
#else
    Q_UNUSED(window)
    Q_UNUSED(message)
    Q_UNUSED(result)
    Q_UNUSED(logicalResizeBorder)
#endif
    return false;
}
