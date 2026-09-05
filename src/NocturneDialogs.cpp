#include "NocturneDialogs.h"
#include "Branding.h"
#include "NocturneStyle.h"
#include "WindowChrome.h"

#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QRegularExpression>
#include <QToolButton>
#include <QStyle>
#include <QVBoxLayout>

namespace {
void markPrimary(QPushButton* button)
{
    button->setProperty("primaryAction", true);
    button->style()->unpolish(button);
    button->style()->polish(button);
}

QWidget* addChrome(QDialog* dialog, QWidget* body)
{
    dialog->setWindowFlag(Qt::FramelessWindowHint);
    dialog->setProperty("nocturneDialog", true);
    dialog->setWindowIcon(NocturneBrand::appIcon());
    auto* outer = new QVBoxLayout(dialog);
    outer->setContentsMargins(1, 1, 1, 1);
    outer->setSpacing(0);
    auto* header = new WindowDragArea(dialog, false);
    header->setObjectName(QStringLiteral("dialogHeader"));
    header->setFixedHeight(48);
    auto* row = new QHBoxLayout(header);
    row->setContentsMargins(16, 0, 8, 0);
    row->setSpacing(10);
    auto* logo = new QLabel(header);
    logo->setPixmap(NocturneBrand::appIcon().pixmap(26, 26));
    row->addWidget(logo);
    auto* title = new QLabel(dialog->windowTitle(), header);
    title->setObjectName(QStringLiteral("dialogCaption"));
    title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    QObject::connect(dialog, &QWidget::windowTitleChanged, title, &QLabel::setText);
    row->addWidget(title, 1);
    auto* close = new QToolButton(header);
    close->setObjectName(QStringLiteral("dialogCloseButton"));
    close->setAccessibleName(QStringLiteral("关闭对话框"));
    close->setToolTip(QStringLiteral("关闭 · Esc"));
    close->setProperty("windowChromeInteractive", true);
    close->setFixedSize(32, 32);
    close->setFocusPolicy(Qt::NoFocus);
    NocturneUi::setGlyph(close, NocturneUi::Glyph::Close);
    row->addWidget(close);
    QObject::connect(close, &QToolButton::clicked, dialog, &QDialog::reject);
    outer->addWidget(header);
    body->setObjectName(QStringLiteral("dialogBody"));
    outer->addWidget(body, 1);
    return body;
}

QLabel* heading(const QString& title, QWidget* parent, QVBoxLayout* layout)
{
    auto* label = new QLabel(title, parent);
    label->setObjectName(QStringLiteral("dialogHeading"));
    label->setWordWrap(true);
    layout->addWidget(label);
    return label;
}

QMessageBox::StandardButton message(QWidget* parent, const QString& title, const QString& text,
    QMessageBox::StandardButtons buttons, QMessageBox::StandardButton defaultButton)
{
    NocturneDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("nocturneMessageDialog"));
    dialog.setWindowTitle(QStringLiteral("夜航 · %1").arg(title));
    dialog.setMinimumWidth(430);
    dialog.resize(480, 300);
    auto* layout = new QVBoxLayout(dialog.body());
    layout->setContentsMargins(28, 25, 28, 24);
    layout->setSpacing(18);
    heading(title, dialog.body(), layout);
    auto* content = new QLabel(text, dialog.body());
    content->setObjectName(QStringLiteral("dialogMessage"));
    content->setTextFormat(Qt::PlainText);
    content->setTextInteractionFlags(Qt::TextSelectableByMouse);
    content->setWordWrap(true);
    layout->addWidget(content);
    layout->addStretch();
    auto* row = new QHBoxLayout;
    row->addStretch();
    QMessageBox::StandardButton result = buttons.testFlag(QMessageBox::Cancel)
        ? QMessageBox::Cancel : (buttons.testFlag(QMessageBox::No) ? QMessageBox::No : QMessageBox::Ok);
    for (const auto choice : {QMessageBox::Cancel, QMessageBox::No, QMessageBox::Yes, QMessageBox::Ok}) {
        if (!buttons.testFlag(choice)) continue;
        const QString label = choice == QMessageBox::Cancel ? QStringLiteral("取消")
            : choice == QMessageBox::No ? QStringLiteral("暂不")
            : choice == QMessageBox::Yes ? QStringLiteral("确认") : QStringLiteral("知道了");
        auto* button = new QPushButton(label, dialog.body());
        button->setObjectName(QStringLiteral("messageButton_%1").arg(static_cast<int>(choice)));
        if (choice == QMessageBox::Yes || choice == QMessageBox::Ok)
            markPrimary(button);
        button->setMinimumSize(86, 36);
        button->setDefault(choice == defaultButton);
        row->addWidget(button);
        QObject::connect(button, &QPushButton::clicked, &dialog, [&, choice] { result = choice; dialog.accept(); });
    }
    layout->addLayout(row);
    dialog.exec();
    return result;
}

QStringList chooseFiles(QWidget* parent, const QString& title, const QString& directory,
    const QString& filter, QFileDialog::FileMode mode, bool save, QString* selectedFilter)
{
    QFileDialog dialog(parent, title, directory, filter);
    dialog.setObjectName(QStringLiteral("nocturneFileDialog"));
    dialog.setOption(QFileDialog::DontUseNativeDialog, true);
    dialog.setOption(QFileDialog::DontConfirmOverwrite, true);
    dialog.setFileMode(mode);
    dialog.setAcceptMode(save ? QFileDialog::AcceptSave : QFileDialog::AcceptOpen);
    dialog.setLabelText(QFileDialog::Accept, save ? QStringLiteral("导出") : QStringLiteral("选择"));
    dialog.setLabelText(QFileDialog::Reject, QStringLiteral("取消"));
    dialog.setLabelText(QFileDialog::LookIn, QStringLiteral("位置"));
    dialog.setLabelText(QFileDialog::FileName, QStringLiteral("文件名"));
    dialog.setLabelText(QFileDialog::FileType, QStringLiteral("文件类型"));
    if (selectedFilter && !selectedFilter->isEmpty()) dialog.selectNameFilter(*selectedFilter);
    auto suffix = [&dialog](const QString& nameFilter) {
        const auto match = QRegularExpression(QStringLiteral("\\*\\.([a-zA-Z0-9]+)")).match(nameFilter);
        if (match.hasMatch()) dialog.setDefaultSuffix(match.captured(1));
    };
    suffix(dialog.selectedNameFilter());
    QObject::connect(&dialog, &QFileDialog::filterSelected, &dialog, suffix);
    NocturneDialogs::decorate(&dialog);
    dialog.resize(820, 580);
    while (dialog.exec() == QDialog::Accepted) {
        const QStringList paths = dialog.selectedFiles();
        if (paths.isEmpty()) return {};
        if (save && QFileInfo::exists(paths.first())) {
            const auto answer = NocturneDialogs::question(&dialog, QStringLiteral("替换已有文件"),
                QStringLiteral("“%1”已经存在。确认用当前笔记替换它？").arg(QFileInfo(paths.first()).fileName()));
            if (answer != QMessageBox::Yes) continue;
        }
        if (selectedFilter) *selectedFilter = dialog.selectedNameFilter();
        return paths;
    }
    return {};
}
}

NocturneDialog::NocturneDialog(QWidget* parent) : QDialog(parent), m_body(new QWidget(this))
{
    addChrome(this, m_body);
    setModal(true);
}
bool NocturneDialog::nativeEvent(const QByteArray& type, void* message, qintptr* result)
{
    if (WindowChrome::handleNativeHitTest(this, message, result, 5)) return true;
    return QDialog::nativeEvent(type, message, result);
}

namespace NocturneDialogs {
void decorate(QDialog* dialog)
{
    auto* body = new QWidget(dialog);
    if (dialog->layout()) body->setLayout(dialog->layout());
    addChrome(dialog, body);
}
QMessageBox::StandardButton information(QWidget* parent, const QString& title, const QString& text)
{
    return message(parent, title, text, QMessageBox::Ok, QMessageBox::Ok);
}
QMessageBox::StandardButton critical(QWidget* parent, const QString& title, const QString& text)
{
    return message(parent, title, text, QMessageBox::Ok, QMessageBox::Ok);
}
QMessageBox::StandardButton question(QWidget* parent, const QString& title, const QString& text,
    QMessageBox::StandardButtons buttons, QMessageBox::StandardButton defaultButton)
{
    return message(parent, title, text, buttons, defaultButton);
}
QString getText(QWidget* parent, const QString& title, const QString& label, QLineEdit::EchoMode mode,
                const QString& text, bool* ok)
{
    NocturneDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("nocturneInputDialog"));
    dialog.setWindowTitle(QStringLiteral("夜航 · %1").arg(title));
    dialog.resize(440, 280);
    auto* layout = new QVBoxLayout(dialog.body());
    layout->setContentsMargins(28, 25, 28, 24);
    layout->setSpacing(16);
    heading(title, dialog.body(), layout);
    auto* fieldLabel = new QLabel(label, dialog.body());
    fieldLabel->setObjectName(QStringLiteral("dialogMessage"));
    layout->addWidget(fieldLabel);
    auto* edit = new QLineEdit(text, dialog.body());
    edit->setObjectName(QStringLiteral("dialogInput"));
    edit->setEchoMode(mode);
    edit->setMinimumHeight(38);
    edit->setAccessibleName(label);
    layout->addWidget(edit);
    layout->addStretch();
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog.body());
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("保存"));
    markPrimary(buttons->button(QDialogButtonBox::Ok));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(edit, &QLineEdit::textChanged, &dialog, [buttons](const QString& value) {
        buttons->button(QDialogButtonBox::Ok)->setEnabled(!value.trimmed().isEmpty());
    });
    buttons->button(QDialogButtonBox::Ok)->setEnabled(!text.trimmed().isEmpty());
    edit->selectAll();
    edit->setFocus();
    const bool accepted = dialog.exec() == QDialog::Accepted;
    if (ok) *ok = accepted;
    return accepted ? edit->text() : QString();
}
QString getOpenFileName(QWidget* parent, const QString& title, const QString& directory, const QString& filter)
{
    return chooseFiles(parent, title, directory, filter, QFileDialog::ExistingFile, false, nullptr).value(0);
}
QStringList getOpenFileNames(QWidget* parent, const QString& title, const QString& directory, const QString& filter)
{
    return chooseFiles(parent, title, directory, filter, QFileDialog::ExistingFiles, false, nullptr);
}
QString getSaveFileName(QWidget* parent, const QString& title, const QString& directory,
    const QString& filter, QString* selectedFilter)
{
    return chooseFiles(parent, title, directory, filter, QFileDialog::AnyFile, true, selectedFilter).value(0);
}
QColor getColor(const QColor& initial, QWidget* parent, const QString& title)
{
    NocturneDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("nocturneColorDialog"));
    dialog.setWindowTitle(QStringLiteral("夜航 · %1").arg(title));
    dialog.resize(430, 380);
    auto* layout = new QVBoxLayout(dialog.body());
    layout->setContentsMargins(28, 24, 28, 24);
    layout->setSpacing(18);
    heading(title, dialog.body(), layout);
    auto* hint = new QLabel(QStringLiteral("选择一抹颜色，或输入 HEX 色值。"), dialog.body());
    hint->setObjectName(QStringLiteral("dialogMessage"));
    layout->addWidget(hint);
    auto* edit = new QLineEdit((initial.isValid() ? initial : NocturneUi::theme().text).name(), dialog.body());
    edit->setObjectName(QStringLiteral("colorHexInput"));
    edit->setAccessibleName(QStringLiteral("HEX 颜色"));
    auto* grid = new QGridLayout;
    grid->setSpacing(12);
    const QStringList swatches = { "#DEE5EB", "#94A4B5", "#D8BD88", "#A5C9BC", "#96B8D2", "#B6A4D1",
        "#253846", "#637586", "#8C693C", "#48775D", "#526F83", "#846C97",
        "#FFFFFF", "#F0C6A4", "#D5847D", "#D5CF9D", "#87B4B8", "#C894AA" };
    for (int i = 0; i < swatches.size(); ++i) {
        auto* button = new QPushButton(dialog.body());
        button->setFixedSize(42, 30);
        button->setToolTip(swatches.at(i));
        button->setAccessibleName(swatches.at(i));
        button->setStyleSheet(QStringLiteral("QPushButton { background: %1; border: 2px solid transparent; border-radius: 5px; }"
                                            "QPushButton:focus { border-color: %2; }")
                                 .arg(swatches.at(i), NocturneUi::theme().accent.name()));
        grid->addWidget(button, i / 6, i % 6);
        QObject::connect(button, &QPushButton::clicked, &dialog, [edit, value = swatches.at(i)] { edit->setText(value); });
    }
    layout->addLayout(grid);
    layout->addWidget(edit);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog.body());
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("应用颜色"));
    markPrimary(buttons->button(QDialogButtonBox::Ok));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    layout->addWidget(buttons);
    QObject::connect(edit, &QLineEdit::textChanged, &dialog, [buttons](const QString& value) {
        buttons->button(QDialogButtonBox::Ok)->setEnabled(QColor::isValidColorName(value));
    });
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    return dialog.exec() == QDialog::Accepted ? QColor(edit->text()) : QColor();
}
}
