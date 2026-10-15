/*
 * ============================================================================
 * videoplayer.h — 事件录像播放器页面
 * ============================================================================
 *
 * VideoPlayer 是"事件录像"导航页的实现。
 * 它自动扫描 data/recordings/ 目录下的录像文件，支持：
 *   - 标准容器格式 (mp4/mkv/avi/wmv)：使用 QMediaPlayer + QVideoWidget 播放
 *   - 原始 MJPEG 格式 (.mjpeg/.mjpg)：自定义解析播放（逐帧解析 JPEG 帧）
 *
 * 自动发现机制：
 *   - 每 2 秒扫描一次录像目录（recordingScanTimer）
 *   - 发现新录像时自动切换到最新文件播放
 *   - 录像列表按修改时间倒序排列（最新的在最上面）
 *
 * MJPEG 播放原理：
 *   .mjpeg 文件是连续的 JPEG 帧，每帧以 FF D8 开头、FF D9 结尾。
 *   加载时扫描整个文件，提取所有 JPEG 帧到 mjpegFrames 列表中，
 *   然后用 QTimer 每 66ms（约 15 FPS）显示一帧。
 *
 * 画面显示策略：
 *   使用 QStackedWidget 在 videoOutput（QVideoWidget）和
 *   mjpegOutput（QLabel）之间切换，根据文件格式选择不同的渲染方式。
 */
#ifndef VIDEOPLAYER_H
#define VIDEOPLAYER_H

#include <QMediaPlayer>
#include <QByteArray>
#include <QStringList>
#include <QVector>
#include <QWidget>

class QLabel;
class QListWidget;
class QListWidgetItem;
class QMediaPlaylist;
class QPushButton;
class QResizeEvent;
class QSlider;
class QStackedWidget;
class QTimer;
class QVideoWidget;

class VideoPlayer final : public QWidget
{
    Q_OBJECT

public:
    explicit VideoPlayer(QWidget *parent = nullptr);
    ~VideoPlayer() override = default;

protected:
    void resizeEvent(QResizeEvent *event) override;

private slots:
    /* 播放控制 */
    void togglePlayback();      // 播放/暂停切换
    void playNextVideo();       // 播放下一段录像
    void increaseVolume();      // 音量 +5
    void decreaseVolume();      // 音量 -5
    void togglePlaylistPanel(); // 显示/隐藏录像列表侧栏

    /* 播放器状态回调 */
    void handlePlayerStateChanged(QMediaPlayer::State state); // 播放状态变化
    void playSelectedVideo(QListWidgetItem *item);            // 用户点击列表项播放
    void handlePlaylistIndexChanged(int index);               // 播放列表索引变化
    void handleDurationChanged(qint64 durationMilliseconds);  // 总时长变化（更新进度条范围）
    void handlePositionChanged(qint64 positionMilliseconds);  // 播放位置变化（更新进度条）

    /* 进度条和音量条交互 */
    void seekToSliderPosition(); // 用户拖动进度条跳转
    void applySliderVolume();    // 用户拖动音量条

    /* 录像扫描 */
    void refreshRecordings(); // 重新扫描录像目录

    /* MJPEG 逐帧播放 */
    void showNextMjpegFrame(); // 显示下一帧（66ms 定时器触发）

private:
    /* 初始化方法 */
    void applyStyleSheet();       // 设置暗色主题样式表
    void initializeLayout();      // 构建 UI 布局（播放区 + 控制栏 + 录像列表）
    void initializeMediaPlayer(); // 创建 QMediaPlayer + QMediaPlaylist

    /* 录像管理 */
    void scanVideoFiles(bool selectLatest = false); // 扫描 data/recordings/ 目录
    void playRecordingAtRow(int row);               // 播放指定行的录像

    /* MJPEG 格式支持 */
    bool loadMjpegRecording(const QString &filePath); // 加载 MJPEG 文件并提取所有 JPEG 帧
    void stopMjpegPlayback();                         // 停止 MJPEG 播放并清空帧缓存
    void updateMjpegFrame();                          // 将当前帧显示到 QLabel

    /* 工具函数 */
    static QString formatDuration(qint64 milliseconds); // 毫秒 → "MM:SS" 格式

    /* ===== 媒体播放组件（标准容器格式） ===== */
    QMediaPlayer *mediaPlayer; // Qt 多媒体播放器
    QMediaPlaylist *playlist;  // 播放列表
    QVideoWidget *videoOutput; // 视频渲染控件（QMediaPlayer 输出目标）

    /* ===== MJPEG 播放组件 ===== */
    QLabel *mjpegOutput;        // MJPEG 帧显示标签（替代 videoOutput）
    QStackedWidget *videoStack; // 视频渲染栈，在 videoOutput / mjpegOutput 间切换

    /* ===== 录像列表 ===== */
    QListWidget *videoList; // 录像文件列表（右侧或可隐藏面板）

    /* ===== 播放控制条 ===== */
    QSlider *positionSlider; // 播放进度条
    QSlider *volumeSlider;   // 音量滑块（0-100）

    /* ===== 控制按钮 ===== */
    QPushButton *playPauseButton;      // 播放/暂停
    QPushButton *nextButton;           // 下一段
    QPushButton *volumeDownButton;     // 音量 -
    QPushButton *volumeUpButton;       // 音量 +
    QPushButton *playlistToggleButton; // 显示/隐藏录像列表
    QPushButton *refreshButton;        // 手动刷新录像列表

    /* ===== 布局容器 ===== */
    QWidget *playlistPanel;   // 录像列表面板（包含标题、列表、刷新按钮）
    QWidget *controlsOverlay; // 控制条覆盖层（未在当前实现中使用，保留）

    /* ===== 状态标签 ===== */
    QLabel *currentTimeLabel;     // 当前播放时间 "MM:SS"
    QLabel *totalTimeLabel;       // 总时长 "/MM:SS"
    QLabel *recordingStatusLabel; // 录像扫描状态文字

    /* ===== 定时器 ===== */
    QTimer *recordingScanTimer; // 录像目录扫描定时器（2 秒间隔）
    QTimer *mjpegPlaybackTimer; // MJPEG 逐帧播放定时器（66ms ≈ 15 FPS）

    /* ===== 录像数据 ===== */
    QStringList recordingPaths;        // 所有录像文件的绝对路径列表
    QVector<QByteArray> mjpegFrames;   // MJPEG 模式下提取的所有 JPEG 帧数据
    QString loadedLatestRecordingPath; // 最近一次自动加载的录像路径（防止重复加载）
    int currentRecordingIndex;         // 当前播放的录像在 recordingPaths 中的索引
    int mjpegFrameIndex;               // MJPEG 当前播放帧索引
    bool rawMjpegMode;                 // 是否处于 MJPEG 原始播放模式
};

#endif // VIDEOPLAYER_H