#pragma once

#include <QAbstractNativeEventFilter>
#include <QKeySequence>
#include <QObject>
#include <QString>
#include <QtGui/qwindowdefs.h>

#include <functional>

class GlobalHotkey final : public QObject, public QAbstractNativeEventFilter
{
public:
    GlobalHotkey(WId windowId,
                 const QKeySequence& sequence,
                 std::function<void()> callback,
                 QObject* parent = nullptr);
    ~GlobalHotkey() override;

    bool isRegistered() const;
    QKeySequence sequence() const;
    bool setSequence(const QKeySequence& sequence, QString* error = nullptr);

    static QKeySequence defaultSequence();
    static QString settingsKey();
    static QString portableText(const QKeySequence& sequence);
    static QString displayText(const QKeySequence& sequence);
    static bool validate(const QKeySequence& sequence, QString* error = nullptr);

    bool nativeEventFilter(const QByteArray& eventType,
                           void* message,
                           qintptr* result) override;

private:
    bool registerSequence(const QKeySequence& sequence, int id) const;
    void unregisterSequence(int id) const;

    WId m_windowId = 0;
    QKeySequence m_sequence;
    std::function<void()> m_callback;
    bool m_registered = false;
};
