#ifndef DECODERTHREAD_H
#define DECODERTHREAD_H

#include <QThread>
#include <QMutex>
#include <QWaitCondition>
#include <QAtomicInt>
#include <QImage>
#include <QDateTime>
#include <qaudiosink.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/imgutils.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
}

class DecoderThread : public QThread
{
    Q_OBJECT

public:
    explicit DecoderThread(QObject *parent = nullptr);
    ~DecoderThread();

    void setFile(const QString &path);
    void requestStop();
    void setPaused(bool pause);
    bool isPaused() const;
    void seek(double position);
    double getDuration() const;
    double getCurrentTime() const;
    void setVolume(int volume);

    // 动态音频控制
    void enableAudio();    // 动态启用音频
    void disableAudio();   // 动态禁用音频
    bool isAudioEnabled() const;

signals:
    void frameReady(QImage image);
    void durationChanged(double duration);
    void positionChanged(double position);
    void timeChanged(double currentTime, double totalTime);
    void error(QString message);
    void volumeChanged(int volume);
    void audioStateChanged(bool enabled);  // 音频状态变化信号

protected:
    void run() override;

private:
    void cleanupAudioOutput();
    bool initAudioDecoder(AVFormatContext *fmtCtx, int audioIdx);
    bool initAudioOutput();

    QMutex m_mutex;
    QMutex m_seekMutex;
    QWaitCondition m_audioCond;

    bool m_stop;
    bool m_pause;
    bool m_seeking;
    double m_seekTarget;
    double m_duration;
    double m_currentTime;
    QString m_filePath;

    QAtomicInt m_volume;
    QAtomicInt m_audioEnabled;  // 改为原子变量，支持动态切换

    // 音频相关
    AVCodecContext *m_audioCodecCtx;
    int m_audioStreamIndex;
    SwrContext *m_swrCtx;
    int m_audioSampleRate;
    int m_audioChannels;
    AVSampleFormat m_audioSampleFmt;

    // 音频输出相关（可以在运行时动态创建/销毁）
    QAudioSink *m_audioOutput;
    QIODevice *m_audioDevice;
    int m_actualAudioSampleRate;

    double m_audioClock;  // 音频时钟（移到类成员）
    qint64 m_lastAudioWriteTime;  // 最后写入音频的时间
    QByteArray m_audioBuffer;  // 音频缓冲区复用

    QByteArray m_audioResampleBuffer;  // 重采样缓冲区
    qint64 m_lastAudioPts;             // 最后播放的音频 PTS
    int m_audioFrameCount;              // 音频帧计数（用于调试）
    // decoderthread.h
private:
    int m_decodedVideoFrames = 0;   // 实际解码的视频帧数
public:
    void setMuted(bool muted);

private:
    std::atomic<bool> m_muted{false};
private:
    QMutex m_audioMutex;   // 保护 m_audioOutput 的访问

    qint64 m_lastAudioToggleTime = 0;          // 上次切换时间戳（毫秒）
    static constexpr qint64 MIN_TOGGLE_INTERVAL_MS = 200; // 最小间隔 200ms

public:
    void setDisplaySize(int width, int height);  // 由 UI 调用

private:
    QMutex m_sizeMutex;
    int m_displayWidth = 0;
    int m_displayHeight = 0;
    bool m_sizeChanged = false;   // 标记是否需要更新

    int m_actualOutWidth;              // 当前实际输出宽度
    int m_actualOutHeight;             // 当前实际输出高度


};

#endif // DECODERTHREAD_H
