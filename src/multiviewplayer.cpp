#include "multiviewplayer.h"
#include "videorenderwidget.h"
#include <QFileDialog>
#include <QMimeData>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QUrl>
#include <QShortcut>
#include <QSettings>
#include <QMessageBox>
#include <QApplication>
#include <QStatusBar>
#include <QStyle>
#include <QLabel>
#include <QTimer>
#include <QDebug>
#include <QToolBar>
#include <QToolButton>
#include <QSlider>
#include <QAction>

static const QString MEDIA_FILTER =
    "视频文件 (*.mp4 *.avi *.mkv *.mov *.webm *.ts);;"
    "图片文件 (*.jpg *.jpeg *.png *.bmp *.gif *.tiff *.tif);;"
    "所有文件 (*.*)";

MultiViewPlayer::MultiViewPlayer(QWidget *parent) : QMainWindow(parent)
{
    setAcceptDrops(true);
    setupUi();
    setupGlobalActions();

    QSettings settings("VideoPlayer", "MultiView");
    restoreGeometry(settings.value("geometry").toByteArray());

    // 恢复音量设置
    m_globalVolume = settings.value("volume", 1.0f).toFloat();
    m_muted = settings.value("muted", false).toBool();

    // 恢复音量控件状态
    m_globalVolumeSlider->setValue(static_cast<int>(m_globalVolume * 100));
    m_globalVolumeSlider->setEnabled(!m_muted);

    if (m_muted) {
        m_muteButton->setIcon(style()->standardIcon(QStyle::SP_MediaVolumeMuted));
    } else {
        m_muteButton->setIcon(style()->standardIcon(QStyle::SP_MediaVolume));
    }

    // if (m_videoWidgets.isEmpty()) {
    //     addVideoWidget();
    // }
}

MultiViewPlayer::~MultiViewPlayer()
{
    // 保存音量设置
    QSettings settings("VideoPlayer", "MultiView");
    settings.setValue("volume", m_globalVolume);
    settings.setValue("muted", m_muted);

    // 先停止所有视频播放
    for (auto *widget : m_videoWidgets) {
        if (widget) {
            widget->stop();
        }
    }

    settings.setValue("geometry", saveGeometry());
}

void MultiViewPlayer::setupUi()
{
    m_central = new QWidget(this);
    setCentralWidget(m_central);
    m_grid = new QGridLayout(m_central);
    m_grid->setContentsMargins(0, 0, 0, 0);
    m_grid->setSpacing(0);

    // 工具栏
    m_toolBar = addToolBar("工具");
    m_toolBar->setMovable(false);
    m_addAction = m_toolBar->addAction("➕ 添加视频");
    m_closeAction = m_toolBar->addAction("❌ 关闭当前");
    m_fullScreenAction = m_toolBar->addAction("⛶ 全屏当前");
    // 【新增】添加批量打开18个视频的按钮
    m_bulkOpenAction = m_toolBar->addAction("📁 打开18个视频");
    // 在工具栏末尾添加“关闭全部”按钮
    m_closeAllAction = m_toolBar->addAction("🚫 关闭全部");
    connect(m_closeAllAction, &QAction::triggered, this, &MultiViewPlayer::closeAllVideos);


    connect(m_addAction, &QAction::triggered, this, &MultiViewPlayer::addVideo);
    connect(m_closeAction, &QAction::triggered, this, [this]() {
        if (m_activeVideo) {
            removeVideoWidget(m_activeVideo);
        }
    });
    connect(m_fullScreenAction, &QAction::triggered, this, [this]() {
        if (m_activeVideo) {
            toggleFullScreen(m_activeVideo);
        }
    });
    connect(m_bulkOpenAction, &QAction::triggered, this, &MultiViewPlayer::openEighteenVideos);  // 新连接

    // 音量控制
    m_globalVolumeSlider = new QSlider(Qt::Horizontal, this);
    m_globalVolumeSlider->setRange(0, 100);
    m_globalVolumeSlider->setValue(static_cast<int>(m_globalVolume * 100));
    m_globalVolumeSlider->setFixedWidth(100);
    m_globalVolumeSlider->setToolTip("全局音量");

    m_muteButton = new QToolButton(this);
    m_muteButton->setIcon(style()->standardIcon(QStyle::SP_MediaVolume));
    m_muteButton->setToolTip("全局静音");
    m_muteButton->setCheckable(true);
    m_muteButton->setChecked(m_muted);

    m_toolBar->addSeparator();
    m_toolBar->addWidget(new QLabel(" 音量 "));
    m_toolBar->addWidget(m_globalVolumeSlider);
    m_toolBar->addWidget(m_muteButton);

    connect(m_globalVolumeSlider, &QSlider::valueChanged,
            this, &MultiViewPlayer::onGlobalVolumeChanged);
    connect(m_muteButton, &QToolButton::clicked,
            this, &MultiViewPlayer::toggleGlobalMute);

    m_central->installEventFilter(this);
}

void MultiViewPlayer::setupGlobalActions()
{
    QShortcut *addShortcut = new QShortcut(QKeySequence("Ctrl+N"), this);
    connect(addShortcut, &QShortcut::activated, this, &MultiViewPlayer::addVideo);

    QShortcut *delShortcut = new QShortcut(QKeySequence("Ctrl+W"), this);
    connect(delShortcut, &QShortcut::activated, this, [this]() {
        if (m_activeVideo) removeVideoWidget(m_activeVideo);
    });

    QShortcut *fullShortcut = new QShortcut(QKeySequence("F11"), this);
    connect(fullShortcut, &QShortcut::activated, this, [this]() {
        if (m_activeVideo) toggleFullScreen(m_activeVideo);
    });
}





void MultiViewPlayer::toggleFullScreen(VideoRenderWidget *widget)
{
    if (!widget) return;

    if (widget->isFullScreen()) {
        widget->setWindowFlags(Qt::Widget);
        widget->showNormal();

        QTimer::singleShot(50, this, [this, widget]() {
            widget->setParent(m_central);
            updateLayout();

            for (auto *w : m_videoWidgets) {
                w->show();
            }

            m_central->update();
        });
    } else {
        for (auto *w : m_videoWidgets) {
            if (w != widget) {
                w->hide();
            }
        }

        m_grid->removeWidget(widget);
        widget->setParent(nullptr);
        widget->setWindowFlags(Qt::Window);
        widget->showFullScreen();
    }
}

bool MultiViewPlayer::eventFilter(QObject *obj, QEvent *event)
{
    if (event->type() == QEvent::MouseButtonPress) {
        QWidget *clickedWidget = qobject_cast<QWidget*>(obj);
        while (clickedWidget && !qobject_cast<VideoRenderWidget*>(clickedWidget)) {
            clickedWidget = clickedWidget->parentWidget();
        }
        VideoRenderWidget *vid = qobject_cast<VideoRenderWidget*>(clickedWidget);
        if (vid && m_videoWidgets.contains(vid)) {
            setActiveVideo(vid);
            return false;
        }
    }
    return QMainWindow::eventFilter(obj, event);
}


void MultiViewPlayer::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
    }
}

void MultiViewPlayer::dropEvent(QDropEvent *event)
{
    const QMimeData *mime = event->mimeData();
    if (mime->hasUrls()) {
        for (const QUrl &url : mime->urls()) {
            QString file = url.toLocalFile();
            if (!file.isEmpty() && QFile::exists(file)) {
                addVideoWidget(file);
            }
        }
    }
}

void MultiViewPlayer::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    updateLayout();
}

void MultiViewPlayer::closeEvent(QCloseEvent *event)
{
    // 保存音量设置
    QSettings settings("VideoPlayer", "MultiView");
    settings.setValue("volume", m_globalVolume);
    settings.setValue("muted", m_muted);

    for (auto *widget : m_videoWidgets) {
        if (widget) {
            widget->stop();
        }
    }

    settings.setValue("geometry", saveGeometry());
    QMainWindow::closeEvent(event);
}

void MultiViewPlayer::onGlobalVolumeChanged(int value)
{
    m_globalVolume = value / 100.0f;

    // 如果未静音，更新当前活跃窗口的音量
    if (!m_muted && m_activeVideo) {
        m_activeVideo->setVolume(m_globalVolume);
    }

    if (value > 0 && m_muted) {
        // 如果音量被调整且当前是静音状态，自动取消静音
        if (m_muted) {
            m_muted = false;
            m_muteButton->setChecked(false);
            m_muteButton->setIcon(style()->standardIcon(QStyle::SP_MediaVolume));
            m_globalVolumeSlider->setEnabled(true);
            if (m_activeVideo) {
                m_activeVideo->setMuted(false);
                m_activeVideo->setVolume(m_globalVolume);
            }
        }
    }
}

void MultiViewPlayer::toggleGlobalMute()
{
    m_muted = !m_muted;
    m_muteButton->setChecked(m_muted);

    if (m_muted) {
        m_globalVolumeSlider->setEnabled(false);
        m_muteButton->setIcon(style()->standardIcon(QStyle::SP_MediaVolumeMuted));
        if (m_activeVideo) {
            m_activeVideo->setVolume(0.0f);
            m_activeVideo->setMuted(true);
        }
    } else {
        m_globalVolumeSlider->setEnabled(true);
        m_muteButton->setIcon(style()->standardIcon(QStyle::SP_MediaVolume));
        if (m_activeVideo) {
            m_activeVideo->setMuted(false);
            m_activeVideo->setVolume(m_globalVolume);
        }
    }
}


void MultiViewPlayer::updateLayout()
{
    if (!m_grid || m_updatingLayout) return;

    // 清空布局项，但不删除控件
    while (QLayoutItem *item = m_grid->takeAt(0)) {
        delete item;
    }

    int count = m_videoWidgets.size();
    if (count == 0) {
        // 没有视频时清空布局，显示空白或提示
        return;
    }

    // 每行最多 9 列
    const int MAX_COLS_PER_ROW = 9;
    int cols = qMin(count, MAX_COLS_PER_ROW);   // 列数 = 最多9，但不能超过视频总数
    int rows = (count + cols - 1) / cols;       // 行数自动计算

    qDebug() << "Layout: count=" << count << "rows=" << rows << "cols=" << cols;

    // 重新添加所有视频到网格
    for (int i = 0; i < count; ++i) {
        int row = i / cols;
        int col = i % cols;
        if (row < rows && m_videoWidgets[i]) {
            m_grid->addWidget(m_videoWidgets[i], row, col);
            m_videoWidgets[i]->show();
        }
    }

    // 设置拉伸因子，让所有行列均匀填满窗口
    for (int r = 0; r < rows; ++r) {
        m_grid->setRowStretch(r, 1);
    }
    for (int c = 0; c < cols; ++c) {
        m_grid->setColumnStretch(c, 1);
    }

    // 多余的行/列拉伸设为0（避免占空间）
    for (int r = rows; r < MAX_ROWS; ++r) {
        m_grid->setRowStretch(r, 0);
    }
    for (int c = cols; c < MAX_COLS; ++c) {
        m_grid->setColumnStretch(c, 0);
    }

    m_grid->setSpacing(0);
    m_grid->setContentsMargins(0, 0, 0, 0);
    m_central->update();
}


void MultiViewPlayer::addVideo()
{
    QFileDialog dialog(this);
    dialog.setWindowTitle("选择媒体文件");
    dialog.setFileMode(QFileDialog::ExistingFiles);
    dialog.setOption(QFileDialog::DontUseNativeDialog, false);
    dialog.setNameFilter(MEDIA_FILTER);   // 替换原先的过滤器

    if (dialog.exec()) {
        QStringList files = dialog.selectedFiles();
        for (const QString& file : files) {
            addVideoWidget(file);
        }
    }
}

void MultiViewPlayer::addVideoWidget(const QString &filePath)
{
    if (m_videoWidgets.size() >= MAX_VIDEOS) {
        // 已满，替换最早打开的视频
        replaceOldestVideo(filePath);
        return;
    }

    auto *vid = new VideoRenderWidget(this);
    bool isFirstVideo = m_videoWidgets.isEmpty();

    // 音频设置
    if (m_muted || !isFirstVideo) {
        vid->setVolume(0.0f);
        vid->setMuted(true);
    } else {
        vid->setVolume(m_globalVolume);
        vid->setMuted(false);
    }

    vid->setPaused(false);

    connect(vid, &VideoRenderWidget::closeRequested, this, [this, vid]() {
        removeVideoWidget(vid);
    });
    connect(vid, &VideoRenderWidget::fullScreenRequested, this, [this, vid]() {
        toggleFullScreen(vid);
    });
    vid->installEventFilter(this);

    // 布局
    int totalCount = m_videoWidgets.size();
    int row = totalCount / MAX_COLS;
    int col = totalCount % MAX_COLS;

    m_grid->addWidget(vid, row, col);
    m_videoWidgets.append(vid);
    m_videoPlayOrder.append(vid);  // 添加到播放顺序列表

    // 延迟播放
    if (!filePath.isEmpty()) {
        QTimer::singleShot(0, this, [vid, filePath]() {
            vid->playFile(filePath);
        });
    }


    updateLayout();

    qDebug() << "Added new video, total:" << m_videoWidgets.size() << "/" << MAX_VIDEOS;
}

void MultiViewPlayer::openEighteenVideos()
{
    // 选择要打开的源视频文件
    QFileDialog dialog(this);
    dialog.setWindowTitle("选择源视频文件（将同步打开18个实例）");
    dialog.setFileMode(QFileDialog::ExistingFile);
    dialog.setOption(QFileDialog::DontUseNativeDialog, false);
    dialog.setNameFilter(MEDIA_FILTER);

    if (!dialog.exec()) {
        return;
    }

    QString sourceFile = dialog.selectedFiles().first();

    // 禁用批量更新，避免频繁重建布局
    m_updatingLayout = true;

    // 先关闭现有的所有视频
    while (!m_videoWidgets.isEmpty()) {
        VideoRenderWidget *widget = m_videoWidgets.first();
        widget->stop();
        m_grid->removeWidget(widget);
        m_videoWidgets.removeOne(widget);
        widget->deleteLater();
    }
    m_activeVideo = nullptr;

    // 批量添加18个视频
    const int TARGET_COUNT = 18;
    int addedCount = 0;

    for (int i = 0; i < TARGET_COUNT && m_videoWidgets.size() < MAX_VIDEOS; ++i) {
        auto *vid = new VideoRenderWidget(this);
        bool isFirstVideo = m_videoWidgets.isEmpty();



        vid->setPaused(false);

        connect(vid, &VideoRenderWidget::closeRequested, this, [this, vid]() {
            removeVideoWidget(vid);
        });
        connect(vid, &VideoRenderWidget::fullScreenRequested, this, [this, vid]() {
            toggleFullScreen(vid);
        });
        vid->installEventFilter(this);

        m_videoWidgets.append(vid);
        addedCount++;

        // // 延迟播放，避免同时加载导致卡顿
        // QTimer::singleShot(i * 0, this, [vid, sourceFile]() {
        //     vid->playFile(sourceFile);
        // });

        vid->playFile(sourceFile);
    }

    // // 设置第一个视频为活动窗口（在主视频创建时直接设置）
    // if (!m_videoWidgets.isEmpty() && !m_muted) {
    //     m_activeVideo = m_videoWidgets.first();
    //     m_activeVideo->setMuted(false);
    //     m_activeVideo->setVolume(m_globalVolume);
    //     m_activeVideo->setAudioEnabled(true);  // 确保音频已启用
    //     qDebug() << "Set first video as active with volume:" << m_globalVolume;
    // } else if (!m_videoWidgets.isEmpty() && m_muted) {
    //     m_activeVideo = m_videoWidgets.first();
    //     m_activeVideo->setMuted(true);
    //     m_activeVideo->setVolume(0.0f);
    // }

    // 启用布局更新并刷新
    m_updatingLayout = false;
    updateLayout();

    // 如果有活动视频，设置音频
    if (m_activeVideo && !m_muted) {
        m_activeVideo->setMuted(false);
        m_activeVideo->setVolume(m_globalVolume);
    }


}

void MultiViewPlayer::replaceOldestVideo(const QString &filePath)
{
    if (m_videoPlayOrder.isEmpty() || m_videoWidgets.isEmpty()) {
        return;
    }

    // 获取最早打开的视频（播放顺序列表的第一个）
    VideoRenderWidget *oldestVideo = nullptr;

    // 从播放顺序列表中找到第一个仍然存在的视频
    for (auto *vid : m_videoPlayOrder) {
        if (m_videoWidgets.contains(vid)) {
            oldestVideo = vid;
            break;
        }
    }

    if (!oldestVideo) {
        // 清理播放顺序列表中的无效指针
        m_videoPlayOrder.clear();
        for (auto *vid : m_videoWidgets) {
            m_videoPlayOrder.append(vid);
        }
        if (!m_videoPlayOrder.isEmpty()) {
            oldestVideo = m_videoPlayOrder.first();
        } else {
            return;
        }
    }

    qDebug() << "Replacing oldest video:" << oldestVideo << "with new file:" << filePath;

    // 保存位置信息
    int index = m_grid->indexOf(oldestVideo);
    int row = -1, col = -1, rowSpan = -1, colSpan = -1;
    if (index >= 0) {
        m_grid->getItemPosition(index, &row, &col, &rowSpan, &colSpan);
    }

    // 停止旧视频
    oldestVideo->stop();

    // 从布局中移除
    m_grid->removeWidget(oldestVideo);

    // 从列表中移除
    m_videoWidgets.removeOne(oldestVideo);
    m_videoPlayOrder.removeOne(oldestVideo);

    // 如果移除的是活动窗口，清除活动窗口
    if (m_activeVideo == oldestVideo) {
        m_activeVideo = nullptr;
    }

    // 删除旧控件
    oldestVideo->deleteLater();

    // 创建新视频控件
    auto *vid = new VideoRenderWidget(this);
    bool isFirstVideo = m_videoWidgets.isEmpty();

    // 音频设置
    if (m_muted || !isFirstVideo) {
        vid->setVolume(0.0f);
        vid->setMuted(true);
    } else {
        vid->setVolume(m_globalVolume);
        vid->setMuted(false);
        m_activeVideo = vid;
    }

    vid->setPaused(false);

    connect(vid, &VideoRenderWidget::closeRequested, this, [this, vid]() {
        removeVideoWidget(vid);
    });
    connect(vid, &VideoRenderWidget::fullScreenRequested, this, [this, vid]() {
        toggleFullScreen(vid);
    });
    vid->installEventFilter(this);

    // 添加到相同位置
    if (row >= 0 && col >= 0) {
        m_grid->addWidget(vid, row, col);
    } else {
        // 如果找不到位置，添加到末尾
        int totalCount = m_videoWidgets.size();
        row = totalCount / MAX_COLS;
        col = totalCount % MAX_COLS;
        m_grid->addWidget(vid, row, col);
    }

    m_videoWidgets.append(vid);
    m_videoPlayOrder.append(vid);  // 添加到播放顺序末尾

    // 播放新视频
    if (!filePath.isEmpty()) {
        QTimer::singleShot(0, this, [vid, filePath]() {
            vid->playFile(filePath);
        });
    }

    // 如果没有活动窗口，设置新视频为活动窗口
    if (!m_activeVideo) {
        m_activeVideo = vid;
        if (!m_muted) {
            vid->setVolume(m_globalVolume);
            vid->setMuted(false);
        }
    }

    updateLayout();


}



void MultiViewPlayer::removeVideoWidget(VideoRenderWidget *widget)
{
    if (!widget || !m_videoWidgets.contains(widget)) return;

    widget->stop();
    m_grid->removeWidget(widget);
    m_videoWidgets.removeOne(widget);
    m_videoPlayOrder.removeOne(widget);  // 从播放顺序中移除
    widget->deleteLater();

    if (m_activeVideo == widget) {
        m_activeVideo = m_videoWidgets.isEmpty() ? nullptr : m_videoWidgets.first();
        if (m_activeVideo) {
            setActiveVideo(m_activeVideo);
        }
    }

    updateLayout();
}

void MultiViewPlayer::setActiveVideo(VideoRenderWidget *widget)
{
    if (!widget || m_activeVideo == widget) return;

    // 动态禁用旧窗口音频
    if (m_activeVideo) {
        m_activeVideo->setAudioEnabled(false);
    }

    // 动态启用新窗口音频
    widget->setAudioEnabled(true);

    if (m_muted) {
        widget->setMuted(true);
        widget->setVolume(0.0f);
    } else {
        widget->setMuted(false);
        widget->setVolume(m_globalVolume);
    }

    m_activeVideo = widget;

    // 更新播放顺序（将当前活动窗口移到最近使用）
    m_videoPlayOrder.removeOne(widget);
    m_videoPlayOrder.append(widget);


}

void MultiViewPlayer::closeAllVideos()
{
    // 逐个移除所有视频，removeVideoWidget 会处理列表更新
    while (!m_videoWidgets.isEmpty()) {
        removeVideoWidget(m_videoWidgets.first());
    }

    // 清空播放顺序（removeVideoWidget 已经移除了每个，但保险起见）
    m_videoPlayOrder.clear();
    m_activeVideo = nullptr;

    // 更新布局（removeVideoWidget 内部已调用，但以防万一）
    updateLayout();


}

