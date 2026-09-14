#include "videorenderwidget.h"
#include "decoderthread.h"
#include <QPainter>
#include <QStyle>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QDragEnterEvent>
#include <QMimeData>
#include <QFileInfo>
#include <QDebug>
#include <QKeyEvent>

VideoRenderWidget::VideoRenderWidget(QWidget *parent) : QWidget(parent)
{
    setAcceptDrops(true);
    setMinimumSize(160, 90);
    setupUi();
    setFocusPolicy(Qt::StrongFocus);
    m_audioEnabled = false;  // 默认不启用音频


}

VideoRenderWidget::~VideoRenderWidget()
{
    stop();
}

void VideoRenderWidget::setupUi()
{
    // 创建控制栏
    m_controlBar = new QWidget(this);
    m_controlBar->setStyleSheet(
        "background-color: rgba(0,0,0,180);"
        "border-radius: 4px;"
        );
    m_controlBar->setFixedHeight(50);
    m_controlBar->hide();

    // 播放/暂停按钮
    m_playPauseBtn = new QToolButton(m_controlBar);
    m_playPauseBtn->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
    m_playPauseBtn->setIconSize(QSize(20, 20));
    connect(m_playPauseBtn, &QToolButton::clicked, this, &VideoRenderWidget::onPlaybackToggle);

    // 进度条
    m_progressSlider = new QSlider(Qt::Horizontal, m_controlBar);
    m_progressSlider->setRange(0, 1000);
    m_progressSlider->setValue(0);
    m_progressSlider->setToolTip("拖动进度");
    connect(m_progressSlider, &QSlider::sliderMoved, this, &VideoRenderWidget::onProgressSliderMoved);

    // 时间显示
    m_timeLabel = new QLabel("00:00 / 00:00", m_controlBar);
    m_timeLabel->setStyleSheet("color: white; font-size: 11px;");
    m_timeLabel->setFixedWidth(100);

    // 布局
    QHBoxLayout *barLayout = new QHBoxLayout(m_controlBar);
    barLayout->setContentsMargins(8, 5, 8, 5);
    barLayout->setSpacing(8);
    barLayout->addWidget(m_playPauseBtn);
    barLayout->addWidget(m_progressSlider, 1);  // 进度条拉伸
    barLayout->addWidget(m_timeLabel);

}

void VideoRenderWidget::playFile(const QString &path)
{
    stop();

    m_currentFile = path;
    m_thread = new DecoderThread(this);
    connect(m_thread, &DecoderThread::frameReady, this, &VideoRenderWidget::onFrameReady);
    connect(m_thread, &DecoderThread::error, this, [this](const QString &msg) {
        qWarning() << "Decoder error:" << msg;
    });
    connect(m_thread, &DecoderThread::durationChanged, this, &VideoRenderWidget::onDurationChanged);
    connect(m_thread, &DecoderThread::positionChanged, this, &VideoRenderWidget::onPositionChanged);
    connect(m_thread, &DecoderThread::timeChanged, this, &VideoRenderWidget::onTimeChanged);
    connect(m_thread, &DecoderThread::videoInfoReady, this,
            [this](const VideoMetaInfo &info) {
                m_info = info;
                m_hasInfo = true;
                emit videoInfoReady(info);   // 继续转发给 MultiViewPlayer
                qDebug() << ">>> received info:" << info.width << "x" << info.height
                         << info.codec << info.fps << "fps";
            });


    m_thread->setFile(path);

    // 根据当前音频状态设置
    if (m_audioEnabled) {
        m_thread->enableAudio();
        int volumeInt = static_cast<int>(m_volume * 100);
        m_thread->setVolume(volumeInt);
    } else {
        m_thread->disableAudio();
    }


    m_thread->start();
    m_playPauseBtn->setIcon(style()->standardIcon(QStyle::SP_MediaPause));
}

void VideoRenderWidget::stop()
{
    if (m_thread) {
        m_thread->requestStop();

        if (!m_thread->wait(2000)) {
            qWarning() << "VideoRenderWidget: Thread won't stop, terminating";
            m_thread->terminate();
            m_thread->wait();
        }

        m_thread->deleteLater();
        m_thread = nullptr;
    }

    {
        QMutexLocker locker(&m_frameMutex);
        m_currentFrame = QImage();
    }

    m_duration = 0.0;
    m_currentTime = 0.0;
    m_progressSlider->setValue(0);
    m_timeLabel->setText("00:00 / 00:00");

    update();
}

void VideoRenderWidget::setPaused(bool pause)
{
    if (m_thread) {
        m_thread->setPaused(pause);
        m_playPauseBtn->setIcon(style()->standardIcon(
            pause ? QStyle::SP_MediaPlay : QStyle::SP_MediaPause));
    }
}

bool VideoRenderWidget::isPaused() const
{
    return m_thread ? m_thread->isPaused() : true;
}

void VideoRenderWidget::seek(double position)
{
    if (m_thread) {
        m_thread->seek(position);
    }
}

double VideoRenderWidget::getDuration() const
{
    return m_duration;
}

double VideoRenderWidget::getCurrentTime() const
{
    return m_currentTime;
}

void VideoRenderWidget::enterFullScreen()
{
    if (m_isFullScreen) return;

    m_originalParent = parentWidget();
    m_originalPosition = pos();
    m_originalSize = size();
    m_originalFlags = windowFlags();

    if (m_originalParent && m_originalParent->layout()) {
        m_originalParent->layout()->removeWidget(this);
    }

    setParent(nullptr);
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    showFullScreen();

    m_isFullScreen = true;
    emit fullScreenStateChanged(true);
}

void VideoRenderWidget::exitFullScreen()
{
    if (!m_isFullScreen) return;

    setParent(m_originalParent);
    setWindowFlags(m_originalFlags);
    showNormal();
    setGeometry(m_originalPosition.x(), m_originalPosition.y(),
                m_originalSize.width(), m_originalSize.height());

    m_isFullScreen = false;
    emit fullScreenStateChanged(false);
    emit exitedFullScreen();
}

void VideoRenderWidget::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.fillRect(rect(), Qt::black);

    QMutexLocker locker(&m_frameMutex);
    if (!m_currentFrame.isNull()) {
        QSize imgSize = m_currentFrame.size();
        QSize widgetSize = size();
        QSize scaledSize = imgSize.scaled(widgetSize, Qt::KeepAspectRatio);
        QRect targetRect(QPoint(0, 0), scaledSize);
        targetRect.moveCenter(rect().center());
        painter.drawImage(targetRect, m_currentFrame);
    }
}

void VideoRenderWidget::onFrameReady(const QImage &img)
{
    if (!m_thread) return;

    {
        QMutexLocker locker(&m_frameMutex);
        m_currentFrame = img;
    }
    update();
}

void VideoRenderWidget::onPlaybackToggle()
{
    bool currentlyPaused = isPaused();
    setPaused(!currentlyPaused);
}

void VideoRenderWidget::onProgressSliderMoved(int value)
{
    if (m_duration > 0) {
        double position = value / 1000.0;
        seek(position);
        m_isSeeking = true;
    }
}

void VideoRenderWidget::onDurationChanged(double duration)
{
    m_duration = duration;
    QString timeStr;
    formatTime(duration, timeStr);
    QString currentStr;
    formatTime(m_currentTime, currentStr);
    m_timeLabel->setText(QString("%1 / %2").arg(currentStr, timeStr));
}

void VideoRenderWidget::onPositionChanged(double position)
{
    if (!m_isSeeking) {
        m_progressSlider->setValue(static_cast<int>(position * 1000));
    }
    m_isSeeking = false;
}

void VideoRenderWidget::onTimeChanged(double currentTime, double totalTime)
{
    m_currentTime = currentTime;
    QString currentStr, totalStr;
    formatTime(currentTime, currentStr);
    formatTime(totalTime, totalStr);
    m_timeLabel->setText(QString("%1 / %2").arg(currentStr, totalStr));
}

void VideoRenderWidget::formatTime(double seconds, QString &text)
{
    int totalSeconds = static_cast<int>(seconds);
    int hours = totalSeconds / 3600;
    int minutes = (totalSeconds % 3600) / 60;
    int secs = totalSeconds % 60;

    if (hours > 0) {
        text = QString("%1:%2:%3").arg(hours, 2, 10, QChar('0'))
                   .arg(minutes, 2, 10, QChar('0'))
                   .arg(secs, 2, 10, QChar('0'));
    } else {
        text = QString("%1:%2").arg(minutes, 2, 10, QChar('0'))
                   .arg(secs, 2, 10, QChar('0'));
    }
}

void VideoRenderWidget::enterEvent(QEnterEvent *)
{
    updateControlBarGeometry();
    // 鼠标进入时显示控制栏
    m_controlBar->show();
}

void VideoRenderWidget::leaveEvent(QEvent *)
{
    // 鼠标离开时立即隐藏控制栏
    m_controlBar->hide();
}

void VideoRenderWidget::resizeEvent(QResizeEvent *)
{
    updateControlBarGeometry();
}

void VideoRenderWidget::updateControlBarGeometry()
{
    if (m_controlBar) {
        m_controlBar->setGeometry(5, height() - m_controlBar->height() - 5,
                                  width() - 10, m_controlBar->height());
    }
}

void VideoRenderWidget::mouseDoubleClickEvent(QMouseEvent *)
{
    if (m_isFullScreen) {
        exitFullScreen();
    } else {
        emit fullScreenRequested();
    }
}

void VideoRenderWidget::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape && m_isFullScreen) {
        exitFullScreen();
        event->accept();
    } else if (event->key() == Qt::Key_Space) {
        onPlaybackToggle();
        event->accept();
    } else {
        QWidget::keyPressEvent(event);
    }
}

void VideoRenderWidget::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
    }
}

void VideoRenderWidget::dropEvent(QDropEvent *event)
{
    const QMimeData *mime = event->mimeData();
    if (mime->hasUrls()) {
        QString file = mime->urls().first().toLocalFile();
        if (!file.isEmpty()) {
            playFile(file);
        }
    }
}

void VideoRenderWidget::setVolume(float vol)
{
    m_volume = qBound(0.0f, vol, 1.0f);

    // 将音量传递给解码线程
    if (m_thread) {
        int volumeInt = static_cast<int>(m_volume * 100);
        m_thread->setVolume(volumeInt);
        qDebug() << "VideoRenderWidget::setVolume:" << vol << "volInt:" << volumeInt;
    }
}

float VideoRenderWidget::volume() const
{
    return m_volume;
}

void VideoRenderWidget::setMuted(bool muted)
{
    m_muted = muted;
    if (m_thread) {
        if (muted) {
            m_thread->setVolume(0);
            qDebug() << "VideoRenderWidget::setMuted: true (volume=0)";
        } else {
            int volumeInt = static_cast<int>(m_volume * 100);
            m_thread->setVolume(volumeInt);
            qDebug() << "VideoRenderWidget::setMuted: false, restoring volume to" << volumeInt;
        }
    }
}

// 添加方法
void VideoRenderWidget::setAudioEnabled(bool enabled)
{
    if (m_audioEnabled != enabled) {
        m_audioEnabled = enabled;
        if (m_thread) {
            if (enabled) {
                m_thread->enableAudio();
                // 设置音量
                int volumeInt = static_cast<int>(m_volume * 100);
                m_thread->setVolume(volumeInt);
            } else {
                m_thread->disableAudio();
            }
        }
        qDebug() << "VideoRenderWidget::setAudioEnabled:" << enabled;
    }
}

bool VideoRenderWidget::isAudioEnabled() const
{
    return m_audioEnabled;
}
