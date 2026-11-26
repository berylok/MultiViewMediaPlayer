#include "decoderthread.h"
#include <QDebug>
#include <QCoreApplication>
#include <QImage>
#include <cmath>
#include <qaudiooutput.h>
#include <QAudioOutput>
#include <QAudioFormat>
#include <QAudioDevice>
#include <QMediaDevices>
#include <QAudioSink>
#include <QDateTime>
#include <atomic>

DecoderThread::DecoderThread(QObject *parent) : QThread(parent)
{
    m_stop = false;
    m_pause = false;
    m_duration = 0.0;
    m_currentTime = 0.0;
    m_seeking = false;
    m_seekTarget = 0.0;
    m_volume.storeRelaxed(100);
    m_audioEnabled.storeRelaxed(true);
    m_audioFlush.store(false);          // ★ 加
    m_muted = false;                     // ★ 加
    m_audioCodecCtx = nullptr;
    m_audioStreamIndex = -1;
    m_swrCtx = nullptr;
    m_audioSampleRate = 0;
    m_audioChannels = 0;
    m_audioSampleFmt = AV_SAMPLE_FMT_NONE;
    m_audioOutput = nullptr;
    m_audioDevice = nullptr;
    m_actualAudioSampleRate = 0;
    m_audioClock = 0.0;
    m_lastAudioWriteTime = 0;
}

DecoderThread::~DecoderThread()
{
    requestStop();

    if (isRunning()) {
        if (!wait(3000)) {
            qWarning() << "DecoderThread: Force terminating";
            terminate();
            wait();
        }
    }

    cleanupAudioOutput();
}

void DecoderThread::setFile(const QString &path)
{
    QMutexLocker locker(&m_mutex);
    m_filePath = path;
    m_stop = false;
    m_pause = false;
    m_duration = 0.0;
    m_currentTime = 0.0;
}

void DecoderThread::requestStop()
{
    m_stop = true;
    // 立即停止音频输出，解除可能的 write() 阻塞
    {
        QMutexLocker locker(&m_audioMutex);
        if (m_audioOutput) {
            m_audioOutput->stop();      // 停止播放
            m_audioOutput->reset();     // 清空缓冲区
        }
    }
    m_audioCond.wakeAll();  // 保留以防其他地方使用
}

void DecoderThread::setPaused(bool pause)
{
    m_pause = pause;
    if (!pause) {
        m_audioCond.wakeAll();
    }
}

bool DecoderThread::isPaused() const
{
    return m_pause;
}

void DecoderThread::seek(double position)
{
    QMutexLocker locker(&m_seekMutex);
    m_seeking = true;
    m_seekTarget = qBound(0.0, position, 1.0);
}

double DecoderThread::getDuration() const
{
    return m_duration;
}

double DecoderThread::getCurrentTime() const
{
    return m_currentTime;
}

void DecoderThread::setVolume(int volume)
{
    int newVol = qBound(0, volume, 100);
    m_volume.storeRelaxed(newVol);       // 无条件更新
    emit volumeChanged(newVol);

    QMutexLocker locker(&m_audioMutex);
    if (m_audioOutput) {
        int applied = m_muted ? 0 : newVol;
        m_audioOutput->setVolume(applied / 100.0f);
    }
}

// 动态启用音频
// 修改 enableAudio() 函数
void DecoderThread::enableAudio()
{
    if (m_audioEnabled.loadRelaxed()) {
        qDebug() << "enableAudio() - Audio already enabled, skipping";
        return;
    }
    qDebug() << "DecoderThread::enableAudio() - Enabling audio dynamically";
    m_audioEnabled.storeRelaxed(true);

    QMutexLocker locker(&m_audioMutex);
    if (m_audioOutput) {
        int vol = m_muted ? 0 : m_volume.loadRelaxed();
        m_audioOutput->setVolume(vol / 100.0f);
        if (m_audioOutput->state() == QAudio::SuspendedState) {
            m_audioOutput->resume();
        } else if (m_audioOutput->state() == QAudio::StoppedState) {
            m_audioDevice = m_audioOutput->start();
        }
        qDebug() << "Audio enabled, vol:" << vol
                 << "state:" << m_audioOutput->state();
    } else {
        qDebug() << "Audio enabled, no sink yet (will be created in main loop)";
    }
    // ★ 删掉 m_audioFlush.store(true); —— enable 不该触发严格 flush
    emit audioStateChanged(true);
}

// 修改 disableAudio() 函数
void DecoderThread::disableAudio()
{
    if (!m_audioEnabled.loadRelaxed()) {
        qDebug() << "disableAudio() - Audio already disabled, skipping";
        return;
    }
    qDebug() << "DecoderThread::disableAudio() - Disabling audio dynamically";
    m_audioEnabled.storeRelaxed(false);

    QMutexLocker locker(&m_audioMutex);
    if (m_audioOutput) {
        m_audioOutput->setVolume(0.0f);
        m_audioOutput->suspend();       // ★ 无条件 suspend
        qDebug() << "Audio disabled, state:" << m_audioOutput->state();
    }
    emit audioStateChanged(false);
}

bool DecoderThread::isAudioEnabled() const
{
    return m_audioEnabled.loadRelaxed();
}

bool DecoderThread::initAudioOutput()
{
    if (!m_audioCodecCtx || !m_swrCtx) {
        qWarning() << "Cannot init audio output - no codec or resampler";
        return false;
    }

    QAudioFormat format;
    format.setSampleRate(m_actualAudioSampleRate);
    format.setChannelCount(2);
    format.setSampleFormat(QAudioFormat::Int16);

    QAudioDevice device = QMediaDevices::defaultAudioOutput();
    if (!device.isFormatSupported(format)) {
        qWarning() << "Audio format not supported:" << m_actualAudioSampleRate << "Hz";
        return false;
    }

    QMutexLocker locker(&m_audioMutex);

    // ★ 只要 sink 存在就复用，不比较 format（PipeWire 会 resample，比较必然不等）
    if (m_audioOutput) {
        int vol = m_muted ? 0 : m_volume.loadRelaxed();
        m_audioOutput->setVolume(vol / 100.0f);
        if (m_audioOutput->state() == QAudio::SuspendedState) {
            m_audioOutput->resume();
        } else if (m_audioOutput->state() == QAudio::StoppedState) {
            m_audioDevice = m_audioOutput->start();
        }
        qDebug() << "Reusing audio sink, state:" << m_audioOutput->state()
                 << "sinkRate:" << m_audioOutput->format().sampleRate()
                 << "reqRate:" << format.sampleRate();
        return true;
    }

    // 首次创建
    m_audioOutput = new QAudioSink(device, format);
    int initVol = m_muted ? 0 : m_volume.loadRelaxed();
    m_audioOutput->setVolume(initVol / 100.0f);
    m_audioDevice = m_audioOutput->start();

    if (m_audioDevice) {
        qDebug() << "Audio output created:" << m_actualAudioSampleRate
                 << "Hz, vol:" << initVol
                 << "actual:" << m_audioOutput->format().sampleRate();
        return true;
    }

    qWarning() << "Failed to start audio output";
    m_audioOutput->stop();
    delete m_audioOutput;
    m_audioOutput = nullptr;
    m_audioDevice = nullptr;
    return false;
}

void DecoderThread::cleanupAudioOutput()
{

    QMutexLocker locker(&m_audioMutex);  // 新增互斥量，保护 m_audioOutput
    if (m_audioOutput) {
        m_audioOutput->stop();
        delete m_audioOutput;
        m_audioOutput = nullptr;
        m_audioDevice = nullptr;
    }

    if (m_swrCtx) {
        swr_free(&m_swrCtx);
        m_swrCtx = nullptr;
    }
    if (m_audioCodecCtx) {
        avcodec_free_context(&m_audioCodecCtx);
        m_audioCodecCtx = nullptr;
    }
    m_audioStreamIndex = -1;
}

void DecoderThread::run()
{
    // 获取文件路径
    QString localPath;
    {
        QMutexLocker locker(&m_mutex);
        localPath = m_filePath;
    }

    if (localPath.isEmpty()) {
        emit error("No file specified");
        return;
    }

    // FFmpeg 初始化

    AVCodecContext *videoCodecCtx = nullptr;
    SwsContext *swsCtx = nullptr;
    AVFrame *videoFrame = nullptr;
    AVFrame *rgbFrame = nullptr;
    AVFrame *audioFrame = nullptr;
    AVPacket *packet = nullptr;
    int videoIdx = -1;
    int audioIdx = -1;

    // 1. 打开文件
    // 在 run() 中，原 avformat_open_input 部分替换为：
    AVFormatContext *fmtCtx = avformat_alloc_context();  // 手动分配
    if (!fmtCtx) {
        emit error("Failed to allocate format context");
        return;
    }

    // 设置中断回调
    fmtCtx->interrupt_callback.callback = [](void *opaque) -> int {
        DecoderThread *self = static_cast<DecoderThread*>(opaque);
        // 返回 1 表示中断，0 表示继续
        return self->m_stop ? 1 : 0;
    };
    fmtCtx->interrupt_callback.opaque = this;


    AVDictionary *options = nullptr;
    av_dict_set(&options, "probesize", "10000000", 0);        // 10 MB
    av_dict_set(&options, "analyzeduration", "10000000", 0);  // 10 秒
    int ret = avformat_open_input(&fmtCtx, localPath.toUtf8().constData(), nullptr, &options);
    av_dict_free(&options);

    // 现在打开输入，传入已分配的 fmtCtx
    if (ret < 0) {
        emit error("Cannot open file: " + localPath);
        //avformat_free_context(fmtCtx);  // 注意，open_input 失败时 fmtCtx 可能为 nullptr，但此处我们传入了指针，需判断
        return;
    }

    // 2. 获取流信息
    if (avformat_find_stream_info(fmtCtx, nullptr) < 0) {
        emit error("Cannot find stream info");
        avformat_close_input(&fmtCtx);
        return;
    }




    // 3. 查找视频和音频流
    for (unsigned i = 0; i < fmtCtx->nb_streams; ++i) {
        if (fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && videoIdx < 0) {
            videoIdx = i;
        } else if (fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && audioIdx < 0) {
            audioIdx = i;
        }
    }
    // 在 videoIdx 找到后，获取流信息
    AVStream *videoStream = fmtCtx->streams[videoIdx];
    int64_t nbFrames = videoStream->nb_frames;

    // 获取总时长
    if (fmtCtx->duration != AV_NOPTS_VALUE) {
        m_duration = fmtCtx->duration / (double)AV_TIME_BASE;
        emit durationChanged(m_duration);
    }

    // ========== ★ 收集视频信息，发给 UI ==========
    if (videoIdx >= 0) {
        AVStream *vst = fmtCtx->streams[videoIdx];
        AVCodecParameters *par = vst->codecpar;

        VideoMetaInfo info;
        info.width  = par->width;
        info.height = par->height;
        info.codec  = QString::fromUtf8(avcodec_get_name(par->codec_id));

        AVRational fr = vst->avg_frame_rate;
        if (fr.num <= 0) fr = vst->r_frame_rate;
        info.fps = av_q2d(fr);
        if (info.fps <= 0) info.fps = 25.0;

        if (par->bit_rate > 0) {
            info.bitrate = par->bit_rate;
        } else if (fmtCtx->bit_rate > 0) {
            info.bitrate = fmtCtx->bit_rate;
        }

        emit videoInfoReady(info);

        qDebug() << ">>> DecoderThread emit videoInfoReady:"
                 << info.width << "x" << info.height
                 << info.codec << info.fps << "fps"
                 << info.bitrate / 1000 << "kbps";
    }
    // =================================================


    // ========== 初始化视频解码器 ==========
    if (videoIdx >= 0) {
        AVCodecParameters *codecPar = fmtCtx->streams[videoIdx]->codecpar;
        const AVCodec *codec = avcodec_find_decoder(codecPar->codec_id);
        if (codec) {
            videoCodecCtx = avcodec_alloc_context3(codec);
            if (videoCodecCtx) {
                avcodec_parameters_to_context(videoCodecCtx, codecPar);
                if (avcodec_open2(videoCodecCtx, codec, nullptr) >= 0) {




                    // ★ 关键修复：取 SAM（采样宽高比），把非方形像素转为方形像素
                    AVRational sar = av_guess_sample_aspect_ratio(fmtCtx, fmtCtx->streams[videoIdx], nullptr);
                    if (sar.num <= 0 || sar.den <= 0) {
                        sar = av_make_q(1, 1);      // 无效则按方形像素处理
                    }
                    videoCodecCtx->sample_aspect_ratio = sar;   // 部分解码器依赖它

                    const int codedWidth  = videoCodecCtx->width;
                    const int codedHeight = videoCodecCtx->height;

                    // 显示宽度 = 编码宽度 × SAR；高度不变
                    int displayW = static_cast<int>(codedWidth * av_q2d(sar) + 0.5);
                    int displayH = codedHeight;

                    const int MAX_W = 1280, MAX_H = 720;
                    if (displayW > MAX_W || displayH > MAX_H) {
                        double scale = qMin((double)MAX_W / displayW, (double)MAX_H / displayH);
                        m_displayWidth  = static_cast<int>(displayW * scale);
                        m_displayHeight = static_cast<int>(displayH * scale);
                        m_sizeChanged = true;
                    } else {
                        m_displayWidth  = displayW;
                        m_displayHeight = displayH;
                        m_sizeChanged = false;
                    }

                    m_actualOutWidth  = m_displayWidth;
                    m_actualOutHeight = m_displayHeight;

                    qDebug() << ">>> Video: coded" << codedWidth << "x" << codedHeight
                             << "SAR" << sar.num << "/" << sar.den
                             << "-> display" << m_displayWidth << "x" << m_displayHeight;

                    videoFrame = av_frame_alloc();
                    rgbFrame   = av_frame_alloc();

                    enum AVPixelFormat srcFmt = videoCodecCtx->pix_fmt;
                    if (srcFmt == AV_PIX_FMT_NONE && codecPar->format != AV_PIX_FMT_NONE) {
                        srcFmt = (enum AVPixelFormat)codecPar->format;
                        videoCodecCtx->pix_fmt = srcFmt;
                    }

                    if (srcFmt != AV_PIX_FMT_NONE && m_displayWidth > 0 && m_displayHeight > 0) {
                        swsCtx = sws_getContext(
                            videoCodecCtx->width, videoCodecCtx->height, srcFmt,
                            m_displayWidth,       m_displayHeight,       AV_PIX_FMT_RGB32,
                            SWS_BICUBIC, nullptr, nullptr, nullptr);
                        // SWS_FAST_BILINEAR SWS_BILINEAR 可能更快，
                        // 但实际感受不到优势并且放大后有条纹影响观感

                        if (!swsCtx) {
                            qWarning() << "sws_getContext failed";
                            // 统一清理后 return（见上）
                        } else {
                            int ret = av_image_alloc(rgbFrame->data, rgbFrame->linesize,
                                                     m_displayWidth, m_displayHeight,
                                                     AV_PIX_FMT_RGB32, 32);
                            if (ret < 0) {
                                qWarning() << "av_image_alloc failed:" << ret;
                                sws_freeContext(swsCtx);
                                swsCtx = nullptr;
                                // 统一清理后 return
                            }
                        }
                    } else {
                        qWarning() << "Pixel format unknown, will create swsCtx at first decoded frame";
                    }
                }
            }
        }
    }

    // ========== 初始化音频解码器（总是初始化解码器，但输出设备可动态开关）==========
    bool audioDecoderReady = false;
    m_actualAudioSampleRate = 0;

    if (audioIdx >= 0) {
        AVCodecParameters *codecPar = fmtCtx->streams[audioIdx]->codecpar;
        const AVCodec *codec = avcodec_find_decoder(codecPar->codec_id);
        if (codec) {
            m_audioCodecCtx = avcodec_alloc_context3(codec);
            if (m_audioCodecCtx) {
                avcodec_parameters_to_context(m_audioCodecCtx, codecPar);
                if (avcodec_open2(m_audioCodecCtx, codec, nullptr) >= 0) {
                    m_audioStreamIndex = audioIdx;
                    m_audioSampleRate = m_audioCodecCtx->sample_rate;
                    m_audioChannels = m_audioCodecCtx->ch_layout.nb_channels;
                    m_audioSampleFmt = m_audioCodecCtx->sample_fmt;

                    m_actualAudioSampleRate = m_audioSampleRate;

                    // 总是初始化重采样器（解码需要）
                    AVChannelLayout targetLayout = AV_CHANNEL_LAYOUT_STEREO;
                    int ret = swr_alloc_set_opts2(&m_swrCtx,
                                                  &targetLayout, AV_SAMPLE_FMT_S16, m_actualAudioSampleRate,
                                                  &m_audioCodecCtx->ch_layout, m_audioSampleFmt, m_audioSampleRate,
                                                  0, nullptr);
                    if (ret >= 0 && m_swrCtx) {
                        swr_init(m_swrCtx);
                        audioDecoderReady = true;

                        // 预分配音频缓冲区
                        int maxOutSamples = av_rescale_rnd(192000, m_actualAudioSampleRate, m_audioSampleRate, AV_ROUND_UP);
                        m_audioBuffer.resize(maxOutSamples * 2 * sizeof(int16_t));

                        // 检查音频格式支持
                        QAudioFormat format;
                        format.setSampleRate(m_actualAudioSampleRate);
                        format.setChannelCount(2);
                        format.setSampleFormat(QAudioFormat::Int16);

                        QAudioDevice device = QMediaDevices::defaultAudioOutput();

                        if (!device.isFormatSupported(format)) {
                            qWarning() << "Format not supported for" << m_actualAudioSampleRate << "Hz, trying 44100 Hz";
                            m_actualAudioSampleRate = 44100;
                            format.setSampleRate(m_actualAudioSampleRate);
                            if (!device.isFormatSupported(format)) {
                                m_actualAudioSampleRate = 48000;
                                format.setSampleRate(m_actualAudioSampleRate);
                                qWarning() << "Trying 48000 Hz";
                            }
                            if (!device.isFormatSupported(format)) {
                                qWarning() << "No compatible audio format";
                                audioDecoderReady = false;
                            } else {
                                // 重新初始化重采样器
                                swr_free(&m_swrCtx);
                                m_swrCtx = nullptr;
                                AVChannelLayout targetLayout2 = AV_CHANNEL_LAYOUT_STEREO;
                                int ret2 = swr_alloc_set_opts2(&m_swrCtx,
                                                               &targetLayout2, AV_SAMPLE_FMT_S16, m_actualAudioSampleRate,
                                                               &m_audioCodecCtx->ch_layout, m_audioSampleFmt, m_audioSampleRate,
                                                               0, nullptr);
                                if (ret2 >= 0 && m_swrCtx) {
                                    swr_init(m_swrCtx);
                                    audioDecoderReady = true;
                                }
                            }
                        }

                        m_audioSampleRate = m_actualAudioSampleRate;
                    } else {
                        qWarning() << "Failed to init resampler";
                    }

                    audioFrame = av_frame_alloc();
                }
            }
        }
    }

    // 如果音频解码器准备好且初始状态是启用，则初始化输出
    if (audioDecoderReady && m_audioEnabled.loadRelaxed()) {
        initAudioOutput();
    } else {
        qDebug() << "Audio output not initialized (disabled or decoder not ready)";
    }

    // 分配包
    packet = av_packet_alloc();

    // 计算视频帧率
    double fps = 25.0;
    if (videoIdx >= 0 && fmtCtx->streams[videoIdx]->avg_frame_rate.num > 0) {
        fps = av_q2d(fmtCtx->streams[videoIdx]->avg_frame_rate);
    }
    int frameDelay = static_cast<int>(1000.0 / fps);

    // 时间基准
    AVRational videoTimeBase = videoIdx >= 0 ? fmtCtx->streams[videoIdx]->time_base : av_make_q(1, 1);
    AVRational audioTimeBase = audioIdx >= 0 ? fmtCtx->streams[audioIdx]->time_base : av_make_q(1, 1);

    // 音频同步变量
    double audioClock = 0.0;
    qint64 lastFrameTime = 0;

    // 音频状态跟踪
    // 在主循环开始，定义局部变量跟踪状态
    bool audioInitAttempted = false;  // 防止重复初始化
    int audioReinitCounter = 0;  // 防止反复重试

    // ==================== 主解码循环 ====================
    // 在主循环中修改音频状态检查
    while (!m_stop) {
        // 【优化】动态检查音频状态变化，防止频繁切换
        bool currentAudioEnabled = m_audioEnabled.loadRelaxed();

        // 主循环只负责：sink 还不存在、但音频需要启用时，创建它
        // suspend/resume 全部由 enableAudio/disableAudio 在 UI 线程立即执行
        if (m_audioEnabled.loadRelaxed() && !m_audioOutput && audioDecoderReady) {
            qDebug() << "Main loop: Initializing audio output (attempt" << audioReinitCounter + 1 << ")";
            if (initAudioOutput()) {
                audioReinitCounter = 0;
            } else {
                audioReinitCounter++;
                if (audioReinitCounter >= 3) {
                    qWarning() << "Audio init failed 3 times, disabling";
                    m_audioEnabled.storeRelaxed(false);
                }
            }
        }


        // 处理跳转
        // 在 run() 函数中，替换原有的 seek 处理部分
        if (m_seeking) {
            double target;
            {
                QMutexLocker locker(&m_seekMutex);
                target = m_seekTarget;
                m_seeking = false;
            }

            qDebug() << "Seeking to position:" << target;

            int64_t seekTarget = static_cast<int64_t>(target * m_duration * AV_TIME_BASE);
            int seekFlags = AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_FRAME;

            if (avformat_seek_file(fmtCtx, -1, INT64_MIN, seekTarget, seekTarget, seekFlags) >= 0) {
                if (videoCodecCtx)   avcodec_flush_buffers(videoCodecCtx);
                if (m_audioCodecCtx) avcodec_flush_buffers(m_audioCodecCtx);

                // 【关键】只 reset，不 delete / 不重建 QAudioSink
                {
                    QMutexLocker locker(&m_audioMutex);
                    if (m_audioOutput) {
                        // m_audioOutput->reset();
                        m_audioFlush.store(true);   // 只在有输出时置位
                    }
                }

                lastFrameTime = 0;
                m_currentTime = target * m_duration;
                m_audioClock  = 0.0;
                audioClock    = 0.0;

                emit positionChanged(target);
                emit timeChanged(m_currentTime, m_duration);

                qDebug() << "Seek completed, new time:" << m_currentTime;
            } else {
                qWarning() << "Seek failed for target:" << seekTarget;
            }

            continue;
        }

        // 暂停/恢复处理
        if (m_pause) {
            QThread::msleep(10);
            continue;
        }
        // ★ 不再有 else 分支操作 sink

        int ret = av_read_frame(fmtCtx, packet);
        if (ret < 0) {
            if (ret == AVERROR_EXIT) {   // 由中断回调触发
                qDebug() << "Read interrupted due to stop request";
                break;   // 退出 while (!m_stop) 循环
            }
            // 如果从未解码出视频帧或只有一帧，且没有音频流 → 视为图片，暂停
            bool isImage = (m_decodedVideoFrames <= 1 && audioIdx < 0);
            if (isImage) {
                m_pause = true;           // 暂停播放，保持最后一帧显示
                continue;                 // 进入暂停分支，不再循环
            }

            // 循环播放
            av_seek_frame(fmtCtx, -1, 0, AVSEEK_FLAG_BACKWARD);
            if (videoCodecCtx) avcodec_flush_buffers(videoCodecCtx);
            if (m_audioCodecCtx) avcodec_flush_buffers(m_audioCodecCtx);


            if (m_audioOutput) {
                QMutexLocker locker(&m_audioMutex);
                if (m_audioOutput) {
                    m_audioOutput->reset();   // 只丢弃待播数据，流保留
                    if (m_audioOutput->state() == QAudio::StoppedState) {
                        m_audioDevice = m_audioOutput->start();  // 仅在确实停掉时才重启
                    }
                }
            }
            lastFrameTime = 0;
            audioClock = 0.0;
            m_currentTime = 0.0;
            emit positionChanged(0.0);
            emit timeChanged(0.0, m_duration);
            continue;
        }

        // ========== 处理视频包 ==========
        if (packet->stream_index == videoIdx && videoCodecCtx) {
            ret = avcodec_send_packet(videoCodecCtx, packet);
            if (ret >= 0) {
                while (ret >= 0 && !m_stop) {
                    ret = avcodec_receive_frame(videoCodecCtx, videoFrame);
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                    if (ret < 0) break;

                    if (videoFrame->pts != AV_NOPTS_VALUE) {
                        double videoPts = videoFrame->pts * av_q2d(videoTimeBase);

                        // 【优化】根据是否有音频输出选择不同的同步策略
                        if (m_audioOutput && audioClock > 0) {
                            double diff = videoPts - m_audioClock;
                            if (diff > 0.02) {
                                // 视频超前，限制最大等待时间
                                int waitMs = qMin(500, static_cast<int>(diff * 1000));
                                QThread::msleep(waitMs);
                            } else if (diff < -0.2) {
                                // 视频落后太多，丢弃该帧以追赶
                                av_frame_unref(videoFrame);
                                continue;
                            }
                        } else {
                            // 没有音频：简单的帧率控制
                            qint64 now = QDateTime::currentMSecsSinceEpoch();
                            if (lastFrameTime != 0) {
                                qint64 elapsed = now - lastFrameTime;
                                if (elapsed < frameDelay) {
                                    QThread::msleep(frameDelay - elapsed);
                                }
                            }
                            lastFrameTime = QDateTime::currentMSecsSinceEpoch();
                        }

                        m_currentTime = videoPts;
                        double position = m_duration > 0 ? m_currentTime / m_duration : 0;
                        emit positionChanged(position);
                        emit timeChanged(m_currentTime, m_duration);
                    }


                    // 转换和显示
                    if (swsCtx && rgbFrame && rgbFrame->data[0]) {
                        sws_scale(swsCtx,
                                  videoFrame->data, videoFrame->linesize,
                                  0,
                                  videoCodecCtx->height,       // ← 源高度 ✅ 正确
                                  rgbFrame->data, rgbFrame->linesize);

                        // QImage img(rgbFrame->data[0], videoCodecCtx->width, videoCodecCtx->height,
                        // rgbFrame->linesize[0], QImage::Format_RGB32);
                        // // 使用实际输出尺寸（即 m_actualOutWidth, m_actualOutHeight）
                        QImage img(rgbFrame->data[0], m_actualOutWidth, m_actualOutHeight,
                                   rgbFrame->linesize[0], QImage::Format_RGB32);
                        emit frameReady(img.copy());
                        // ✅ 在这里递增计数
                        m_decodedVideoFrames++;
                    }

                    if (m_stop) break;
                }
            }
        }
        // ========== 处理音频包（优化版本）==========
        // ========== 处理音频包 ==========
        else if (packet->stream_index == audioIdx && m_audioCodecCtx) {
            QIODevice *dev = nullptr;
            {
                QMutexLocker locker(&m_audioMutex);
                if (m_audioOutput) dev = m_audioDevice;
            }
            if (!dev) {
                av_packet_unref(packet);
                continue;
            }

            ret = avcodec_send_packet(m_audioCodecCtx, packet);
            if (ret < 0) {
                av_packet_unref(packet);
                continue;
            }

            while (ret >= 0 && !m_stop) {
                if (!audioFrame) break;
                ret = avcodec_receive_frame(m_audioCodecCtx, audioFrame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                if (ret < 0) break;

                if (audioFrame->pts != AV_NOPTS_VALUE) {
                    m_audioClock = audioFrame->pts * av_q2d(audioTimeBase);
                } else {
                    m_audioClock += (double)audioFrame->nb_samples / m_actualAudioSampleRate;
                }

                // flush：seek 后只丢明显落后的帧，有帧立即关闭
                if (m_audioFlush.load()) {
                    if (m_audioClock < m_currentTime - 1.0) {
                        av_frame_unref(audioFrame);
                        continue;
                    }
                    m_audioFlush.store(false);
                }

                if (m_swrCtx) {
                    int outSamples = av_rescale_rnd(
                        swr_get_delay(m_swrCtx, m_actualAudioSampleRate) + audioFrame->nb_samples,
                        m_actualAudioSampleRate,
                        m_actualAudioSampleRate,
                        AV_ROUND_UP);

                    int bufferSize = outSamples * 2 * sizeof(int16_t);
                    if (m_audioBuffer.size() < bufferSize) {
                        m_audioBuffer.resize(bufferSize);
                    }

                    uint8_t *outData[1] = { reinterpret_cast<uint8_t*>(m_audioBuffer.data()) };
                    int samplesConverted = swr_convert(
                        m_swrCtx, outData, outSamples,
                        (const uint8_t **)audioFrame->data,
                        audioFrame->nb_samples);

                    if (samplesConverted > 0) {
                        int dataSize = samplesConverted * 2 * sizeof(int16_t);

                        QIODevice *cur = nullptr;
                        bool shouldWrite = false;
                        {
                            QMutexLocker locker(&m_audioMutex);
                            if (m_audioOutput && m_audioDevice == dev) {
                                cur = dev;
                                QAudio::State s = m_audioOutput->state();
                                // ★ IdleState 等数据，ActiveState 正在播，两者都要写
                                shouldWrite = (s == QAudio::IdleState || s == QAudio::ActiveState);
                            }
                        }
                        if (cur && shouldWrite) {
                            cur->write(m_audioBuffer.constData(), dataSize);
                            m_lastAudioWriteTime = QDateTime::currentMSecsSinceEpoch();
                        }
                    }
                }

                av_frame_unref(audioFrame);
            }
        }

        av_packet_unref(packet);
    }

    // ==================== 清理资源 ====================
    cleanupAudioOutput();

    if (rgbFrame && rgbFrame->data[0]) {
        av_freep(&rgbFrame->data[0]);
    }
    av_frame_free(&videoFrame);
    av_frame_free(&rgbFrame);
    av_frame_free(&audioFrame);
    av_packet_free(&packet);
    sws_freeContext(swsCtx);
    avcodec_free_context(&videoCodecCtx);
    if (fmtCtx) {
        avformat_close_input(&fmtCtx);
    }
}

void DecoderThread::setMuted(bool muted)
{
    m_muted = muted;
    QMutexLocker locker(&m_audioMutex);  // 新增互斥量，保护 m_audioOutput
    if (m_audioOutput) {
        if (muted) {
            // 静音：将音量设为 0
            m_audioOutput->setVolume(0.0);
            qDebug() << "Muted (volume set to 0)";
        } else {
            // 取消静音：恢复到之前的音量
            int vol = m_volume.loadRelaxed();
            m_audioOutput->setVolume(vol / 100.0f);
            qDebug() << "Unmuted (volume restored to" << vol << ")";
        }
    }
}

