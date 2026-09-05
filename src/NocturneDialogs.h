#pragma once

#include <QDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QColor>
#include <QStringList>

class QVBoxLayout;

// Shared app-owned titlebar and surface for every modal workflow.
class NocturneDialog final : public QDialog
{
public:
    explicit NocturneDialog(QWidget* parent = nullptr);
    QWidget* body() const { return m_body; }
protected:
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;
private:
    QWidget* m_body = nullptr;
};

namespace NocturneDialogs {
void decorate(QDialog* dialog);
QMessageBox::StandardButton information(QWidget* parent, const QString& title, const QString& text);
QMessageBox::StandardButton critical(QWidget* parent, const QString& title, const QString& text);
QMessageBox::StandardButton question(QWidget* parent, const QString& title, const QString& text,
    QMessageBox::StandardButtons buttons = QMessageBox::Yes | QMessageBox::Cancel,
    QMessageBox::StandardButton defaultButton = QMessageBox::Cancel);
QString getText(QWidget* parent, const QString& title, const QString& label,
    QLineEdit::EchoMode mode = QLineEdit::Normal, const QString& text = QString(), bool* ok = nullptr);
QString getOpenFileName(QWidget* parent, const QString& title, const QString& directory,
    const QString& filter);
QStringList getOpenFileNames(QWidget* parent, const QString& title, const QString& directory,
    const QString& filter);
QString getSaveFileName(QWidget* parent, const QString& title, const QString& directory,
    const QString& filter, QString* selectedFilter = nullptr);
QColor getColor(const QColor& initial, QWidget* parent, const QString& title);
}

