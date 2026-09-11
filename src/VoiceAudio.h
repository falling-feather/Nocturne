#pragma once
#include <QObject>
#include <QStringList>
#include <memory>

class VoiceRecorder final : public QObject
{
    Q_OBJECT
public:
    explicit VoiceRecorder(QObject* parent = nullptr);
    ~VoiceRecorder() override;
    static QStringList devices();
    bool start(int device, QString* error = nullptr);
    bool stopToFile(const QString& path, QString* error = nullptr);
    bool isRecording() const;
    double seconds() const;
signals:
    void levelChanged(int percent);
    void recordingError(const QString& message);
    void recordingLimitReached();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
namespace VoiceAudio
{
bool writePcmWave(
    const QString& path, const QByteArray& pcm, int rate, int channels, QString* error = nullptr);
bool prepareWave(const QString& input, const QString& output, QString* error = nullptr);
int runPrepare(int argc, char** argv);
QString transcript(const QByteArray& output);
}
