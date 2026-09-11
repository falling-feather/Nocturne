#include "Branding.h"
#include "AiExchange.h"
#include "ProfileMigration.h"
#include "BackupManager.h"
#include "Database.h"
#include "GlobalHotkey.h"
#include "MainWindow.h"
#include "DocumentImporter.h"
#include "NocturneDialogs.h"
#include "NocturneStyle.h"

#include <QApplication>
#include <QDir>
#include <QFont>
#include <QLockFile>
#include <QMessageBox>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QDirIterator>
#include <QFile>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QUrl>
#include <QCryptographicHash>
#include <QUuid>
#include <QTextStream>

int main(int argc, char* argv[])
{
    for(int i=1;i<argc;++i)if(QString::fromLocal8Bit(argv[i])==QStringLiteral("--mcp"))return AiExchange::runMcp(argc,argv);
    QApplication app(argc, argv);
    const bool startupBenchmark = app.arguments().contains(
        QStringLiteral("--benchmark-startup"));
    const bool backgroundBenchmark = app.arguments().contains(
        QStringLiteral("--benchmark-background"));
    const bool benchmarkProfile = startupBenchmark || backgroundBenchmark;
    const bool testProfile = app.arguments().contains(QStringLiteral("--test-profile"));
    const bool linkProjectDocs = app.arguments().contains(QStringLiteral("--link-project-docs"));
    if ((testProfile || benchmarkProfile)
        && qEnvironmentVariableIntValue("NOCTURNE_ALLOW_TEST_PROFILE") != 1) {
        if (benchmarkProfile) {
            qWarning("Benchmark profiles require NOCTURNE_ALLOW_TEST_PROFILE=1.");
            return 2;
        }
        QApplication::setApplicationDisplayName(NocturneBrand::englishName());
        QApplication::setStyle(QStringLiteral("Fusion"));
        NocturneUi::applyPalette();
        app.setStyleSheet(NocturneUi::styleSheet());
        NocturneDialogs::information(nullptr, QStringLiteral("请使用桌面夜航"),
            QStringLiteral("独立预览入口已停用。请从桌面“夜航 Nocturne”打开统一的笔记库。\n"
                           "隔离配置仅供显式启用的自动化测试使用。"));
        return 2;
    }
    app.setProperty("automationProfile", testProfile || benchmarkProfile);
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
    QApplication::setStyle(QStringLiteral("Fusion"));
    NocturneUi::setTheme(QSettings().value(QStringLiteral("appearance/theme"), QStringLiteral("night")).toString());
    NocturneUi::applyPalette();
    app.setStyleSheet(NocturneUi::styleSheet());

    const QString dailyDirectory = QDir::homePath() + QStringLiteral("/NocturneData");
    const QString lockDir = benchmarkProfile || testProfile
        ? QStandardPaths::writableLocation(QStandardPaths::TempLocation) : QDir::homePath();
    QDir().mkpath(lockDir);
    QLockFile instanceLock(QDir(lockDir).filePath(benchmarkProfile
        ? (backgroundBenchmark ? QStringLiteral("FeatherNoteBackgroundBenchmark.lock")
                               : QStringLiteral("FeatherNoteStartupBenchmark.lock"))
        : (testProfile ? QStringLiteral("FeatherNoteUiSmoke.lock")
                       : QStringLiteral("FeatherNotePrototype.lock"))));
    instanceLock.setStaleLockTime(0);
    if (!instanceLock.tryLock(100)) {
        if (linkProjectDocs) {
            qWarning("Night is already running; document linking was not started.");
            return 3;
        }
        QSettings settings;
        QKeySequence configured(
            settings.value(GlobalHotkey::settingsKey(),
                           GlobalHotkey::portableText(GlobalHotkey::defaultSequence()))
                .toString(),
            QKeySequence::PortableText);
        QString validationError;
        if (!GlobalHotkey::validate(configured, &validationError))
            configured = GlobalHotkey::defaultSequence();
        NocturneDialogs::information(nullptr,
                                 QStringLiteral("夜航已在运行"),
                                 QStringLiteral("夜航已经在后台运行。\n"
                                                "请按 %1 新建桌面便签，或从系统托盘打开主窗口。")
                                     .arg(GlobalHotkey::displayText(configured)));
        return 0;
    }

    QString error;
    if (!testProfile && !benchmarkProfile) {
        if (!prepareDailyData(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation), dailyDirectory, &error)) {
            NocturneDialogs::critical(nullptr, QStringLiteral("资料迁移未完成"), error); return 1;
        }
        app.setProperty("nocturneDataDirectory", dailyDirectory);
    }
    Database database;
    if (!database.open(&error)) {
        if (linkProjectDocs) {
            qWarning().noquote() << QStringLiteral("无法初始化资料库：") + error;
            return 1;
        }
        NocturneDialogs::critical(nullptr,
                              QStringLiteral("无法打开本地数据"),
                              QStringLiteral("夜航无法初始化本地数据库：\n%1").arg(error));
        return 1;
    }

    if (linkProjectDocs) {
        QString report;
        const bool ok = DocumentImporter::linkRecentProjectDocs(
            QStringLiteral("D:/代码玩具测试"), 60, &report);
        QTextStream(stdout) << report << Qt::endl;
        return ok ? 0 : 1;
    }

    MainWindow window(&database);
    window.setProperty("suppressTrayNotifications", benchmarkProfile || testProfile);
    window.show();
    if (startupBenchmark)
        QTimer::singleShot(2500, &app, &QCoreApplication::quit);
    if (backgroundBenchmark) {
        QTimer::singleShot(450, &window, &QWidget::close);
        QTimer::singleShot(4500, &app, &QCoreApplication::quit);
    }
    return app.exec();
}
