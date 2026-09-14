#ifndef VIDEORENDERWIDGET_H
#define VIDEORENDERWIDGET_H

#include "decoderthread.h"
#include <QWidget>
#include <QImage>
#include <QMutex>
#include <QTimer>
#include <QToolButton>
#include <QSlider>
#include <QLabel>

#include "videometainfo.h"


class DecoderThread;

class VideoRenderWidget : public QWidget
{
    Q_OBJECT

public:
    explicit VideoRenderWidget(QWidget *parent = nullptr);
    ~VideoRenderWidget() override;

    void playFile(const QString &path);
    void stop();
    void setPaused(bool pause);
    bool isPaused() const;
    void setVolume(float vol);
    float volume() const;
    void setMuted(bool muted);
    bool isMuted() const { return m_muted; }

    void enterFullScreen();
    void exitFullScreen();
    bool isVideoFullScreen() const { return m_isFullScreen; }

    // 进度控制
    void seek(double position);
    double getDuration() const;
    double getCurrentTime() const;

    // ★ 新增
    const VideoMetaInfo& info() const { return m_info; }
    bool hasInfo() const { return m_hasInfo; }



signals:
    void closeRequested();
    void fullScreenRequested();
    void fullScreenStateChanged(bool fullScreen);
    void exitedFullScreen();
signals:
    void videoInfoReady(const VideoMetaInfo &info);

protected:
    void paintEvent(QPaintEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private slots:
    void onFrameReady(const QImage &img);
    void onPlaybackToggle();
    void onProgressSliderMoved(int value);
    void onDurationChanged(double duration);
    void onPositionChanged(double position);
    void onTimeChanged(double currentTime, double totalTime);

private:
    void setupUi();
    void updateControlBarGeometry();
    void formatTime(double seconds, QString &text);

    DecoderThread *m_thread = nullptr;
    QImage m_currentFrame;
    QMutex m_frameMutex;

    // UI 控件
    QWidget *m_controlBar = nullptr;
    QToolButton *m_playPauseBtn = nullptr;
    QSlider *m_progressSlider = nullptr;  // 进度条
    QLabel *m_timeLabel = nullptr;         // 时间显示

    // 状态
    QString m_currentFile;
    float m_volume = 1.0f;
    bool m_muted = false;
    bool m_isFullScreen = false;
    double m_duration = 0.0;
    double m_currentTime = 0.0;
    bool m_isSeeking = false;

    // 全屏前保存的信息
    QWidget *m_originalParent = nullptr;
    QPoint m_originalPosition;
    QSize m_originalSize;
    Qt::WindowFlags m_originalFlags;
    // 在类中添加
public:
    void setAudioEnabled(bool enabled);
    bool isAudioEnabled() const;

private:
    bool m_audioEnabled;

    VideoMetaInfo m_info;
    bool m_hasInfo = false;


};

#endif // VIDEORENDERWIDGET_H
