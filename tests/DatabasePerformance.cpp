#include "Database.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QStandardPaths>
#include <QString>
#include <QUuid>
#include <QVector>

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <memory>

namespace {

bool check(bool condition, const char* message)
{
    if (!condition)
        std::cerr << "FAIL: " << message << '\n';
    return condition;
}

double percentileMs(QVector<qint64> samples, double percentile)
{
    if (samples.isEmpty())
        return 0.0;
    std::sort(samples.begin(), samples.end());
    const qsizetype index = std::clamp<qsizetype>(
        static_cast<qsizetype>((samples.size() - 1) * percentile),
        0,
        samples.size() - 1);
    return samples.at(index) / 1'000'000.0;
}

void removeTestData(const QString& dataDirectory)
{
    const QString normalizedTestPath = QDir::cleanPath(dataDirectory);
    const QString normalizedTestRoot = QDir::cleanPath(
        QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation));
    if (!normalizedTestPath.isEmpty()
        && normalizedTestPath.contains(QStringLiteral("qttest"), Qt::CaseInsensitive)
        && normalizedTestPath.startsWith(normalizedTestRoot)) {
        QDir(dataDirectory).removeRecursively();
    }
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName(QStringLiteral("FeatherNoteTests"));
    QCoreApplication::setApplicationName(
        QStringLiteral("Performance-%1")
            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));

    constexpr int kNoteCount = 600;
    constexpr int kSummaryRuns = 30;
    constexpr int kBodyRuns = 120;
    const QString paragraph = QStringLiteral(
        "海面风向、关卡节奏、镜头调度与角色动机在此汇合。"
        "这是一段用于模拟真实创作笔记体量的正文，不参与列表渲染。\n");
    QString longBody;
    longBody.reserve(6200);
    for (int i = 0; i < 72; ++i)
        longBody += paragraph;

    bool ok = true;
    QString error;
    auto database = std::make_unique<Database>();
    ok &= check(database->open(&error), "performance database opens");
    if (!ok) {
        std::cerr << error.toStdString() << '\n';
        return 1;
    }

    QVector<qint64> ids;
    ids.reserve(kNoteCount);
    for (int index = 0; index < kNoteCount; ++index) {
        const QString title = QStringLiteral("灵感航线 %1").arg(index, 4, 10, QLatin1Char('0'));
        const QString plain = QStringLiteral("编号 %1\n").arg(index) + longBody;
        const QString html = QStringLiteral(
            "<h1>%1</h1><p>%2</p><img src=\"attachments/concept-%3.png\">"
            "<p>%2</p>")
                                 .arg(title, longBody.toHtmlEscaped())
                                 .arg(index % 24);
        const qint64 id = database->createNote(title, html, plain, &error);
        if (id <= 0) {
            std::cerr << "FAIL: seeding note " << index << ": "
                      << error.toStdString() << '\n';
            ok = false;
            break;
        }
        ids.append(id);
    }

    QList<NoteSummary> summaries = database->listNoteSummaries(QString(), &error);
    ok &= check(error.isEmpty() && summaries.size() == kNoteCount,
                "summary query returns every seeded note");
    if (!ids.isEmpty())
        database->note(ids.at(ids.size() / 2), &error);

    QVector<qint64> summarySamples;
    summarySamples.reserve(kSummaryRuns);
    for (int run = 0; run < kSummaryRuns; ++run) {
        QElapsedTimer timer;
        timer.start();
        summaries = database->listNoteSummaries(QString(), &error);
        summarySamples.append(timer.nsecsElapsed());
        ok &= check(error.isEmpty() && summaries.size() == kNoteCount,
                    "repeated summary query stays complete");
    }

    QVector<qint64> bodySamples;
    bodySamples.reserve(kBodyRuns);
    for (int run = 0; run < kBodyRuns && !ids.isEmpty(); ++run) {
        const qint64 id = ids.at((run * 37) % ids.size());
        QElapsedTimer timer;
        timer.start();
        const std::optional<NoteRecord> note = database->note(id, &error);
        bodySamples.append(timer.nsecsElapsed());
        ok &= check(error.isEmpty() && note.has_value()
                        && note->plainText.size() > 2000,
                    "selected body query returns the requested large note");
    }

    const double summaryMedian = percentileMs(summarySamples, 0.50);
    const double summaryP95 = percentileMs(summarySamples, 0.95);
    const double bodyMedian = percentileMs(bodySamples, 0.50);
    const double bodyP95 = percentileMs(bodySamples, 0.95);
    const QFileInfo databaseFile(QDir(database->dataDirectory()).filePath(
        QStringLiteral("notebook.sqlite3")));

    std::cout << std::fixed << std::setprecision(3)
              << "PERF notes=" << kNoteCount
              << " database_mib=" << (databaseFile.size() / 1024.0 / 1024.0)
              << " summary_median_ms=" << summaryMedian
              << " summary_p95_ms=" << summaryP95
              << " body_median_ms=" << bodyMedian
              << " body_p95_ms=" << bodyP95 << '\n';

    ok &= check(summaryMedian < 80.0, "summary median remains below 80 ms");
    ok &= check(summaryP95 < 160.0, "summary p95 remains below 160 ms");
    ok &= check(bodyMedian < 15.0, "selected body median remains below 15 ms");
    ok &= check(bodyP95 < 40.0, "selected body p95 remains below 40 ms");

    const QString dataDirectory = database->dataDirectory();
    database.reset();
    removeTestData(dataDirectory);
    return ok ? 0 : 1;
}
