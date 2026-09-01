#pragma once

#include <QFrame>
#include <QPoint>

class QMouseEvent;

class WindowDragArea final : public QFrame
{
public:
    explicit WindowDragArea(QWidget* parent = nullptr, bool doubleClickMaximizes = true);

protected:
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    bool doubleClickMaximizes_ = true;
    bool fallbackDragging_ = false;
    QPoint fallbackOffset_;
};

namespace WindowChrome {

// 处理 Windows 无边框窗口的八向缩放与原生标题栏拖动命中；其他平台返回 false。
bool handleNativeHitTest(QWidget* window,
                         void* message,
                         qintptr* result,
                         int logicalResizeBorder = 7);

}
