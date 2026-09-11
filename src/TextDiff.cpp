#include "TextDiff.h"
#include <QStringList>
#include <QVector>
#include <algorithm>

QString TextDiff::unified(
    const QString& before, const QString& after, const QString& beforeName, const QString& afterName)
{
    if (before == after)
        return QStringLiteral("没有内容变化。\n");
    const auto a = before.split('\n'), b = after.split('\n');
    QString output = "--- " + beforeName + "\n+++ " + afterName + "\n";
    int prefix = 0;
    while (prefix < a.size() && prefix < b.size() && a[prefix] == b[prefix])
        ++prefix;
    int tail = 0;
    while (tail < a.size() - prefix && tail < b.size() - prefix
        && a[a.size() - 1 - tail] == b[b.size() - 1 - tail])
        ++tail;
    const int n = int(a.size()) - prefix - tail, m = int(b.size()) - prefix - tail;
    output += QStringLiteral("@@ -%1,%2 +%1,%3 @@\n").arg(prefix + 1).arg(n).arg(m);
    if (qint64(n + 1) * (m + 1) <= 2000000)
    {
        QVector<int> lengths((n + 1) * (m + 1));
        auto cell = [&](int i, int j) -> int& { return lengths[i * (m + 1) + j]; };
        for (int i = n - 1; i >= 0; --i)
            for (int j = m - 1; j >= 0; --j)
                cell(i, j) = a[prefix + i] == b[prefix + j] ? cell(i + 1, j + 1) + 1
                                                            : std::max(cell(i + 1, j), cell(i, j + 1));
        int i = 0, j = 0;
        while (i < n || j < m)
        {
            if (i < n && j < m && a[prefix + i] == b[prefix + j])
            {
                output += " " + a[prefix + i] + "\n";
                ++i;
                ++j;
            }
            else if (i < n && (j == m || cell(i + 1, j) >= cell(i, j + 1)))
            {
                output += "-" + a[prefix + i++] + "\n";
            }
            else
                output += "+" + b[prefix + j++] + "\n";
        }
    }
    else
    {
        // Bound working memory; the larger changed block is still exact and complete.
        for (int i = 0; i < n; ++i)
            output += "-" + a[prefix + i] + "\n";
        for (int j = 0; j < m; ++j)
            output += "+" + b[prefix + j] + "\n";
    }
    return output;
}
