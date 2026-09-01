#include "Branding.h"
#include "Database.h"
#include "MainWindow.h"

#include <QApplication>
#include <QDir>
#include <QFont>
#include <QLockFile>
#include <QMessageBox>
#include <QStandardPaths>
#include <QTimer>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    const bool startupBenchmark = app.arguments().contains(
        QStringLiteral("--benchmark-startup"));
    const bool backgroundBenchmark = app.arguments().contains(
        QStringLiteral("--benchmark-background"));
    const bool benchmarkProfile = startupBenchmark || backgroundBenchmark;
    const bool testProfile = app.arguments().contains(QStringLiteral("--test-profile"));
    if (testProfile || benchmarkProfile)
        QStandardPaths::setTestModeEnabled(true);
    // 保留旧内部身份，确保 V0.1.1 用户的数据库与 QSettings 原位延续。
    QApplication::setOrganizationName(QStringLiteral("FeatherNote"));
    QApplication::setOrganizationDomain(QStringLiteral("local.feathernote"));
    QApplication::setApplicationName(benchmarkProfile
        ? (backgroundBenchmark ? QStringLiteral("FeatherNoteBackgroundBenchmark")
                               : QStringLiteral("FeatherNoteStartupBenchmark"))
        : (testProfile ? QStringLiteral("FeatherNoteUiSmoke")
                       : QStringLiteral("FeatherNote")));
    QApplication::setApplicationDisplayName(benchmarkProfile
        ? (backgroundBenchmark ? QStringLiteral("Nocturne Background Benchmark")
                               : QStringLiteral("Nocturne Startup Benchmark"))
        : (testProfile ? QStringLiteral("Nocturne UI Smoke")
                       : NocturneBrand::englishName()));
    QApplication::setApplicationVersion(QStringLiteral(NOCTURNE_VERSION));
    QApplication::setWindowIcon(NocturneBrand::appIcon());
    QApplication::setQuitOnLastWindowClosed(false);

    QFont appFont(QStringLiteral("Microsoft YaHei UI"));
    appFont.setPointSize(10);
    QApplication::setFont(appFont);

    const QString lockDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    QDir().mkpath(lockDir);
    QLockFile instanceLock(QDir(lockDir).filePath(benchmarkProfile
        ? (backgroundBenchmark ? QStringLiteral("FeatherNoteBackgroundBenchmark.lock")
                               : QStringLiteral("FeatherNoteStartupBenchmark.lock"))
        : (testProfile ? QStringLiteral("FeatherNoteUiSmoke.lock")
                       : QStringLiteral("FeatherNotePrototype.lock"))));
    instanceLock.setStaleLockTime(0);
    if (!instanceLock.tryLock(100)) {
        QMessageBox::information(nullptr,
                                 QStringLiteral("夜航已在运行"),
                                 QStringLiteral("夜航已经在后台运行。\n"
                                                "请按 Ctrl+Alt+N 新建桌面便签，或从系统托盘打开主窗口。"));
        return 0;
    }

    Database database;
    QString error;
    if (!database.open(&error)) {
        QMessageBox::critical(nullptr,
                              QStringLiteral("无法打开本地数据"),
                              QStringLiteral("夜航无法初始化本地数据库：\n%1").arg(error));
        return 1;
    }

    MainWindow window(&database);
    window.setProperty("suppressTrayNotifications", benchmarkProfile);
    window.show();
    if (startupBenchmark)
        QTimer::singleShot(2500, &app, &QCoreApplication::quit);
    if (backgroundBenchmark) {
        QTimer::singleShot(450, &window, &QWidget::close);
        QTimer::singleShot(4500, &app, &QCoreApplication::quit);
    }
    return app.exec();
}
