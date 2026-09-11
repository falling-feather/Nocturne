#include "VoiceAudio.h"
#include <QDataStream>
#include <QFileInfo>
#include <QSaveFile>

bool VoiceAudio::writePcmWave(
    const QString& path, const QByteArray& pcm, int rate, int channels, QString* error)
{
    if (QFileInfo::exists(path))
    {
        if (error)
            *error = QStringLiteral("目标录音文件已存在，未覆盖。");
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
    {
        if (error)
            *error = file.errorString();
        return false;
    }
    QByteArray header;
    QDataStream stream(&header, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.writeRawData("RIFF", 4);
    stream << quint32(pcm.size() + 36);
    stream.writeRawData("WAVEfmt ", 8);
    stream << quint32(16) << quint16(1) << quint16(channels) << quint32(rate) << quint32(rate * channels * 2)
           << quint16(channels * 2) << quint16(16);
    stream.writeRawData("data", 4);
    stream << quint32(pcm.size());
    if (file.write(header) != header.size() || file.write(pcm) != pcm.size() || !file.commit())
    {
        if (error)
            *error = file.errorString();
        return false;
    }
    return true;
}
