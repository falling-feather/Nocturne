#include "VoiceAudio.h"
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtEndian>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
namespace
{
template <class T> struct ComObject
{
    T* value = nullptr;
    ~ComObject()
    {
        if (value)
            value->Release();
    }
    T** address()
    {
        return &value;
    }
    T* operator->() const
    {
        return value;
    }
};
struct MediaRuntime
{
    bool com = false, mf = false;
    ~MediaRuntime()
    {
        if (mf)
            MFShutdown();
        if (com)
            CoUninitialize();
    }
};
}
#endif
bool VoiceAudio::prepareWave(const QString& input, const QString& output, QString* error)
{
    if (error)
        error->clear();
    if (!QFileInfo(input).isFile() || QFileInfo(input).size() > 256 * 1024 * 1024)
    {
        if (error)
            *error = QStringLiteral("请选择不超过 256 MiB 的录音文件。");
        return false;
    }
#ifdef Q_OS_WIN
    MediaRuntime runtime;
    const auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    runtime.com = SUCCEEDED(com);
    if (FAILED(com) && com != RPC_E_CHANGED_MODE)
    {
        if (error)
            *error = QStringLiteral("无法初始化音频解码。");
        return false;
    }
    auto result = MFStartup(MF_VERSION);
    if (FAILED(result))
    {
        if (error)
            *error = QStringLiteral("Windows 媒体组件不可用。");
        return false;
    }
    runtime.mf = true;
    ComObject<IMFSourceReader> reader;
    result = MFCreateSourceReaderFromURL(reinterpret_cast<LPCWSTR>(input.utf16()), nullptr, reader.address());
    ComObject<IMFMediaType> type, actual;
    if (SUCCEEDED(result))
        result = reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    if (SUCCEEDED(result))
        result = reader->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
    if (SUCCEEDED(result))
        result = MFCreateMediaType(type.address());
    if (SUCCEEDED(result))
        result = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    if (SUCCEEDED(result))
        result = type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    if (SUCCEEDED(result))
        result = type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    if (SUCCEEDED(result))
        result = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, type.value);
    if (SUCCEEDED(result))
        result = reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, actual.address());
    UINT32 channels = 0, rate = 0, bits = 0;
    if (SUCCEEDED(result))
        result = actual->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
    if (SUCCEEDED(result))
        result = actual->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
    if (SUCCEEDED(result))
        result = actual->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);
    if (FAILED(result) || channels < 1 || channels > 8 || rate < 8000 || rate > 192000 || bits != 16)
    {
        if (error)
            *error = QStringLiteral("无法解码此音频格式，请使用 WAV、MP3 或 M4A（0x%1）。")
                         .arg(quint32(result), 8, 16, QChar('0'));
        return false;
    }
    QByteArray pcm;
    while (true)
    {
        ComObject<IMFSample> sample;
        DWORD flags = 0;
        LONGLONG timestamp = 0;
        result = reader->ReadSample(
            MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, &timestamp, sample.address());
        if (FAILED(result))
        {
            if (error)
                *error = QStringLiteral("读取音频数据失败。");
            return false;
        }
        if (sample.value)
        {
            ComObject<IMFMediaBuffer> buffer;
            result = sample->ConvertToContiguousBuffer(buffer.address());
            BYTE* bytes = nullptr;
            DWORD length = 0;
            if (SUCCEEDED(result))
                result = buffer->Lock(&bytes, nullptr, &length);
            if (FAILED(result))
            {
                if (error)
                    *error = QStringLiteral("音频数据不完整。");
                return false;
            }
            if (channels == 1)
                pcm.append(reinterpret_cast<const char*>(bytes), int(length));
            else
                for (DWORD i = 0; i + channels * 2 <= length; i += channels * 2)
                {
                    int total = 0;
                    for (UINT32 channel = 0; channel < channels; ++channel)
                        total += qFromLittleEndian<qint16>(bytes + i + channel * 2);
                    const auto value = qToLittleEndian(qint16(total / int(channels)));
                    pcm.append(reinterpret_cast<const char*>(&value), 2);
                }
            buffer->Unlock();
        }
        if (pcm.size() > 128 * 1024 * 1024 || double(pcm.size()) / (rate * 2) > 1800)
        {
            if (error)
                *error = QStringLiteral("当前支持 30 分钟以内的录音，请将长录音拆分后转写。");
            return false;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
            break;
    }
    if (pcm.isEmpty())
    {
        if (error)
            *error = QStringLiteral("录音中没有可解码的声音。");
        return false;
    }
    return writePcmWave(output, pcm, int(rate), 1, error);
#else
    if (error)
        *error = QStringLiteral("此音频解码入口支持 Windows。");
    return false;
#endif
}
int VoiceAudio::runPrepare(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const auto arguments = app.arguments();
    const int index = arguments.indexOf("--prepare-audio");
    if (index < 0 || index + 2 >= arguments.size())
        return 2;
    QString error;
    const bool ok = prepareWave(arguments[index + 1], arguments[index + 2], &error);
    QFile output;
    if (output.open(stdout, QIODevice::WriteOnly))
    {
        output.write(
            QJsonDocument(QJsonObject { { "ok", ok }, { "error", error } }).toJson(QJsonDocument::Compact)
            + '\n');
        output.flush();
    }
    return ok ? 0 : 2;
}
