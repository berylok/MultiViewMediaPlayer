#ifndef MULTIVIEWPLAYER_H
#define MULTIVIEWPLAYER_H

#include <QMainWindow>
#include <QList>
#include <QGridLayout>
#include <QSlider>
#include <QToolButton>

#include <QElapsedTimer>  // 添加这一行
#include <QTimer>

class VideoRenderWidget;

class MultiViewPlayer : public QMainWindow
{
    Q_OBJECT

public:
    explicit MultiViewPlayer(QWidget *parent = nullptr);
    ~MultiViewPlayer() override;

void addVideoWidget(const QString &filePath = QString());

    void removeVideoWidget(VideoRenderWidget *widget);
    void setActiveVideo(VideoRenderWidget *widget);

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    bool eventFilter(QObject *obj, QEvent *event) override;

private slots:
    void addVideo();
    void toggleFullScreen(VideoRenderWidget *widget);
    void onGlobalVolumeChanged(int value);
    void toggleGlobalMute();

private:
    void setupUi();
    void setupGlobalActions();
    void updateLayout();

    // 设置为 3行 x 6列 = 最多18个视频
    static constexpr int MAX_ROWS = 2;      // 3行
    static constexpr int MAX_COLS = 9;      // 5列
    static constexpr int MAX_VIDEOS = MAX_ROWS * MAX_COLS;  // 15个

    QWidget *m_central = nullptr;
    QGridLayout *m_grid = nullptr;
    QList<VideoRenderWidget*> m_videoWidgets;
    VideoRenderWidget *m_activeVideo = nullptr;

    // 工具栏
    QToolBar *m_toolBar = nullptr;
    QAction *m_addAction = nullptr;
    QAction *m_closeAction = nullptr;
    QAction *m_fullScreenAction = nullptr;

    // 音量控制
    QSlider *m_globalVolumeSlider = nullptr;
    QToolButton *m_muteButton = nullptr;
    float m_globalVolume = 0.5f;

    int m_currentCols = MAX_COLS;  // 当前使用的列数
    bool m_muted = false;  // 添加静音状态

private:
    bool m_updatingLayout = false;  // 是否正在批量更新

    // 在 private slots: 或 private: 部分添加
private:
    void openEighteenVideos();  // 添加新函数声明
    QAction *m_bulkOpenAction;  // 新增按钮

    QAction *m_closeAllAction;
    void closeAllVideos();

private:
    void replaceOldestVideo(const QString &filePath);
    QAction *m_cycleReplaceAction;
    bool m_cycleReplaceEnabled = true;  // 循环替换开关
    QList<VideoRenderWidget*> m_videoPlayOrder;

private slots:
    void onOpenVideosClicked();   // 新加，菜单/按钮触发
    void openVideos(int count);   // 实际干活

private:
    QAction *m_open18Action = nullptr;    // 直接开 18 路
    QAction *m_openMultiAction = nullptr; // 弹对话框选路数

};

#endif // MULTIVIEWPLAYER_H
