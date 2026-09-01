#include "Branding.h"

#include <QSize>

namespace NocturneBrand {

QString chineseName()
{
    return QStringLiteral("夜航");
}

QString englishName()
{
    return QStringLiteral("Nocturne");
}

QString displayName()
{
    return QStringLiteral("夜航 · Nocturne");
}

QString motto()
{
    return QStringLiteral("所见所思，杂而成章。");
}

QIcon appIcon()
{
    static const QIcon icon = [] {
        QIcon cached;
        const int sizes[] = {16, 24, 32, 48, 64, 128, 256};
        for (const int size : sizes) {
            cached.addFile(QStringLiteral(":/branding/nocturne-%1.png").arg(size),
                           QSize(size, size));
        }
        return cached;
    }();
    return icon;
}

}
