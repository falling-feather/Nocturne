#include "Database.h"
#include "MainWindow.h"

#include <QApplication>
#include <QDir>
#include <QFont>
#include <QLockFile>
#include <QMessageBox>
#include <QStandardPaths>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    const bool testProfile = app.arguments().contains(QStringLiteral("--test-profile"));
    if (testProfile)
        QStandardPaths::setTestModeEnabled(true);
    QApplication::setOrganizationName(QStringLiteral("FeatherNote"));
    QApplication::setOrganizationDomain(QStringLiteral("local.feathernote"));
    QApplication::setApplicationName(testProfile
        ? QStringLiteral("FeatherNoteUiSmoke")
        : QStringLiteral("FeatherNote"));
    QApplication::setApplicationVersion(QStringLiteral(FEATHERNOTE_VERSION));
    QApplication::setQuitOnLastWindowClosed(false);

    QFont appFont(QStringLiteral("Microsoft YaHei UI"));
    appFont.setPointSize(10);
    QApplication::setFont(appFont);

    const QString lockDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    QDir().mkpath(lockDir);
    QLockFile instanceLock(QDir(lockDir).filePath(testProfile
        ? QStringLiteral("FeatherNoteUiSmoke.lock")
        : QStringLiteral("FeatherNotePrototype.lock")));
    instanceLock.setStaleLockTime(0);
    if (!instanceLock.tryLock(100)) {
        QMessageBox::information(nullptr,
                                 QStringLiteral("FeatherNote 已在运行"),
                                 QStringLiteral("FeatherNote 已经在后台运行。\n"
                                                "请按 Ctrl+Alt+N 呼出快速便签，或从系统托盘打开主窗口。"));
        return 0;
    }

    Database database;
    QString error;
    if (!database.open(&error)) {
        QMessageBox::critical(nullptr,
                              QStringLiteral("无法打开本地数据"),
                              QStringLiteral("FeatherNote 无法初始化本地数据库：\n%1").arg(error));
        return 1;
    }

    MainWindow window(&database);
    window.show();
    return app.exec();
}
