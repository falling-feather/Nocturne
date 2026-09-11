#include "VoiceAudio.h"
#include <QCoreApplication>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QSaveFile>
#include <QtEndian>
#include <algorithm>
#include <array>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#endif

namespace
{
constexpr qint64 maxPcmBytes = 128 * 1024 * 1024;
}

struct VoiceRecorder::Impl
{
    VoiceRecorder* owner;
    mutable QMutex mutex;
    QByteArray pcm;
    bool requeue = false, accepting = false;
    int rate = 16000, channels = 1;
#ifdef Q_OS_WIN
    HWAVEIN input = nullptr;
    struct Buffer
    {
        WAVEHDR header {};
        std::array<char, 8192> bytes {};
    };
    std::array<Buffer, 4> buffers;
    void close(bool retain)
    {
        if (!input)
            return;
        {
            QMutexLocker lock(&mutex);
            requeue = false;
            if (!retain)
                accepting = false;
        }
        waveInStop(input);
        waveInReset(input);
        for (auto& buffer : buffers)
            if (buffer.header.dwFlags & WHDR_PREPARED)
                waveInUnprepareHeader(input, &buffer.header, sizeof(WAVEHDR));
        waveInClose(input);
        input = nullptr;
        QMutexLocker lock(&mutex);
        accepting = false;
    }
    static void CALLBACK receive(
        HWAVEIN input, UINT message, DWORD_PTR instance, DWORD_PTR parameter, DWORD_PTR)
    {
        if (message != WIM_DATA)
            return;
        auto* self = reinterpret_cast<Impl*>(instance);
        auto* header = reinterpret_cast<WAVEHDR*>(parameter);
        bool limit = false;
        QString error;
        int peak = 0;
        {
            QMutexLocker lock(&self->mutex);
            if (self->accepting && header->dwBytesRecorded)
            {
                if (self->pcm.size() + header->dwBytesRecorded <= maxPcmBytes)
                {
                    self->pcm.append(header->lpData, int(header->dwBytesRecorded));
                    for (DWORD i = 0; i + 1 < header->dwBytesRecorded; i += 2)
                        peak = std::max(peak,
                            std::abs(int(qFromLittleEndian<qint16>(
                                reinterpret_cast<const uchar*>(header->lpData + i)))));
                }
                else
                {
                    self->requeue = false;
                    limit = true;
                }
            }
            if (self->requeue)
            {
                header->dwBytesRecorded = 0;
                const auto status = waveInAddBuffer(input, header, sizeof(WAVEHDR));
                if (status != MMSYSERR_NOERROR)
                {
                    self->requeue = false;
                    error = QStringLiteral("录音设备中断，已采集的内容仍保留。");
                }
            }
        }
        emit self->owner->levelChanged(std::min(100, peak * 100 / 32768));
        if (limit)
            emit self->owner->recordingLimitReached();
        if (!error.isEmpty())
            emit self->owner->recordingError(error);
    }
#endif
};
VoiceRecorder::VoiceRecorder(QObject* parent)
    : QObject(parent)
    , m_impl(std::make_unique<Impl>())
{
    m_impl->owner = this;
}
VoiceRecorder::~VoiceRecorder()
{
#ifdef Q_OS_WIN
    m_impl->close(false);
#endif
}
QStringList VoiceRecorder::devices()
{
    QStringList result;
#ifdef Q_OS_WIN
    for (UINT id = 0; id < waveInGetNumDevs(); ++id)
    {
        WAVEINCAPSW caps {};
        if (waveInGetDevCapsW(id, &caps, sizeof(caps)) == MMSYSERR_NOERROR)
            result.append(QString::fromWCharArray(caps.szPname));
        else
            result.append(QStringLiteral("麦克风 %1").arg(id + 1));
    }
#endif
    return result;
}
bool VoiceRecorder::start(int device, QString* error)
{
    if (error)
        error->clear();
    if (isRecording())
        return true;
#ifdef Q_OS_WIN
    MMRESULT result = MMSYSERR_ERROR;
    for (int rate : { 16000, 48000, 44100 })
    {
        WAVEFORMATEX format {};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = 1;
        format.nSamplesPerSec = rate;
        format.wBitsPerSample = 16;
        format.nBlockAlign = 2;
        format.nAvgBytesPerSec = rate * 2;
        result = waveInOpen(&m_impl->input, device < 0 ? WAVE_MAPPER : UINT(device), &format,
            reinterpret_cast<DWORD_PTR>(&Impl::receive), reinterpret_cast<DWORD_PTR>(m_impl.get()),
            CALLBACK_FUNCTION);
        if (result == MMSYSERR_NOERROR)
        {
            m_impl->rate = rate;
            break;
        }
    }
    if (result != MMSYSERR_NOERROR)
    {
        m_impl->input = nullptr;
        if (error)
            *error = QStringLiteral("无法打开麦克风（%1）。请检查设备或 Windows 麦克风权限。").arg(result);
        return false;
    }
    {
        QMutexLocker lock(&m_impl->mutex);
        m_impl->pcm.clear();
        m_impl->accepting = true;
        m_impl->requeue = true;
    }
    for (auto& buffer : m_impl->buffers)
    {
        buffer.header = {};
        buffer.header.lpData = buffer.bytes.data();
        buffer.header.dwBufferLength = DWORD(buffer.bytes.size());
        if (waveInPrepareHeader(m_impl->input, &buffer.header, sizeof(WAVEHDR)) != MMSYSERR_NOERROR
            || waveInAddBuffer(m_impl->input, &buffer.header, sizeof(WAVEHDR)) != MMSYSERR_NOERROR)
        {
            m_impl->close(false);
            if (error)
                *error = QStringLiteral("录音缓冲区初始化失败。");
            return false;
        }
    }
    if (waveInStart(m_impl->input) != MMSYSERR_NOERROR)
    {
        m_impl->close(false);
        if (error)
            *error = QStringLiteral("麦克风未能开始录音。");
        return false;
    }
    return true;
#else
    if (error)
        *error = QStringLiteral("当前录音入口支持 Windows。");
    return false;
#endif
}
bool VoiceRecorder::stopToFile(const QString& path, QString* error)
{
    if (error)
        error->clear();
#ifdef Q_OS_WIN
    m_impl->close(true);
#endif
    QMutexLocker lock(&m_impl->mutex);
    if (m_impl->pcm.isEmpty())
    {
        if (error)
            *error = QStringLiteral("没有录到声音数据。");
        return false;
    }
    if (!VoiceAudio::writePcmWave(path, m_impl->pcm, m_impl->rate, m_impl->channels, error))
        return false;
    m_impl->pcm.clear();
    m_impl->pcm.squeeze();
    return true;
}
bool VoiceRecorder::isRecording() const
{
#ifdef Q_OS_WIN
    return m_impl->input != nullptr;
#else
    return false;
#endif
}
double VoiceRecorder::seconds() const
{
    QMutexLocker lock(&m_impl->mutex);
    return double(m_impl->pcm.size()) / (m_impl->rate * m_impl->channels * 2);
}

QString VoiceAudio::transcript(const QByteArray& output)
{
    QString result;
    for (const auto& line : output.split('\n'))
    {
        const int start = line.indexOf('{');
        if (start < 0)
            continue;
        QJsonParseError parse;
        const auto document = QJsonDocument::fromJson(line.mid(start).trimmed(), &parse);
        if (parse.error == QJsonParseError::NoError && document.isObject()
            && document.object().value("text").isString())
            result = document.object().value("text").toString();
    }
    return result.trimmed();
}
