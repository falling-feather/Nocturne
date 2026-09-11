#pragma once
#include <QString>
namespace TextDiff
{
QString unified(const QString& before, const QString& after,
    const QString& beforeName = QStringLiteral("之前"), const QString& afterName = QStringLiteral("当前"));
}
