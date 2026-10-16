/*
 * videoplayer.h — “事件录像”页面声明
 *
 * VideoPlayer 每 2 秒扫描 data/recordings：
 *   .h264 和标准容器交给 QMediaPlayer；
 *   历史 .mjpeg/.mjpg 文件由本类解析连续 JPEG 帧并定时显示。
 */
#ifndef VIDEOPLAYER_H // 防止本头文件被重复包含。
#define VIDEOPLAYER_H // 定义头文件保护宏。

#include <QMediaPlayer> // 使用 QMediaPlayer::State 类型并播放 H.264/容器录像。
#include <QByteArray>   // 保存从旧 MJPEG 文件提取出的单帧 JPEG 字节。
#include <QStringList>  // 保存录像文件绝对路径列表。
#include <QVector>      // 保存全部旧 MJPEG 帧。
#include <QWidget>      // VideoPlayer 的 Qt 页面基类。

class QLabel;          // 前向声明画面、时间和状态标签。
class QListWidget;     // 前向声明录像列表控件。
class QListWidgetItem; // 前向声明用户点击的录像列表项。
class QMediaPlaylist;  // 前向声明 Qt 媒体播放列表。
class QPushButton;     // 前向声明播放控制按钮。
class QResizeEvent;    // 前向声明页面尺寸变化事件。
class QSlider;         // 前向声明进度和音量滑块。
class QStackedWidget;  // 前向声明两种视频显示控件的切换容器。
class QTimer;          // 前向声明目录扫描和 MJPEG 播放定时器。
class QVideoWidget;    // 前向声明 QMediaPlayer 视频输出控件。

class VideoPlayer final : public QWidget // final 表示该页面不允许继续派生。
{
    Q_OBJECT // 启用 Qt 元对象系统和 private slots。

public:
    /**
     * 创建录像播放 UI、媒体播放器和两个定时器，并扫描已有录像。
     * @param parent Qt 父控件，负责页面在对象树中的生命周期。
     */
    explicit VideoPlayer(QWidget *parent = nullptr);

    /** 使用 QWidget 默认析构逻辑，所有子对象由 Qt 自动回收。 */
    ~VideoPlayer() override = default;

protected:
    /**
     * 页面大小改变时重新缩放当前 MJPEG 帧。
     * @param event Qt 传入的尺寸变化事件。
     */
    void resizeEvent(QResizeEvent *event) override;

private slots:
    /** 根据当前状态播放或暂停 QMediaPlayer/MJPEG。 */
    void togglePlayback();

    /** 循环切换到 recordingPaths 中的下一段录像。 */
    void playNextVideo();

    /** 将当前音量增加 5，最大不超过 100。 */
    void increaseVolume();

    /** 将当前音量减少 5，最小不低于 0。 */
    void decreaseVolume();

    /** 切换右侧录像列表面板的显示状态。 */
    void togglePlaylistPanel();

    /**
     * 根据 QMediaPlayer 状态同步播放按钮。
     * @param state 当前播放器状态：Stopped、Playing 或 Paused。
     */
    void handlePlayerStateChanged(QMediaPlayer::State state);

    /**
     * 播放用户双击的录像列表项。
     * @param item 被双击的 QListWidgetItem；可能为空。
     */
    void playSelectedVideo(QListWidgetItem *item);

    /**
     * 同步播放列表当前索引和页面录像索引。
     * @param index QMediaPlaylist 当前条目下标。
     */
    void handlePlaylistIndexChanged(int index);

    /**
     * 媒体总时长更新时刷新进度条范围和总时间。
     * @param durationMilliseconds 总时长，单位毫秒。
     */
    void handleDurationChanged(qint64 durationMilliseconds);

    /**
     * 播放位置更新时刷新进度条和当前时间。
     * @param positionMilliseconds 当前播放位置，单位毫秒。
     */
    void handlePositionChanged(qint64 positionMilliseconds);

    /** 用户松开进度滑块后跳转到所选时间或 MJPEG 帧。 */
    void seekToSliderPosition();

    /** 把音量滑块当前值应用到 QMediaPlayer。 */
    void applySliderVolume();

    /** 立即重新扫描录像目录，并在需要时选择最新录像。 */
    void refreshRecordings();

    /** MJPEG 定时器槽：显示下一帧，到末尾后循环。 */
    void showNextMjpegFrame();

private:
    /** 给当前页面安装暗色 QSS。 */
    void applyStyleSheet();

    /** 创建播放区、控制栏、进度条和录像列表布局。 */
    void initializeLayout();

    /** 创建 QMediaPlayer/QMediaPlaylist 并连接播放器信号。 */
    void initializeMediaPlayer();

    /**
     * 扫描 data/recordings 并刷新 recordingPaths 和列表控件。
     * @param selectLatest true 表示没有当前录像时自动播放最新文件。
     */
    void scanVideoFiles(bool selectLatest = false);

    /**
     * 根据后缀选择 QMediaPlayer 或旧 MJPEG 播放路径。
     * @param row recordingPaths 中要播放的下标。
     */
    void playRecordingAtRow(int row);

    /**
     * 从旧 MJPEG 文件中提取连续 JPEG 帧。
     * @param filePath .mjpeg 或 .mjpg 文件完整路径。
     * @return 至少提取到一帧时返回 true，否则返回 false。
     */
    bool loadMjpegRecording(const QString &filePath);

    /** 停止 MJPEG 定时器，并清空帧缓存和索引状态。 */
    void stopMjpegPlayback();

    /** 解码并缩放当前 mjpegFrameIndex 指向的 JPEG 帧。 */
    void updateMjpegFrame();

    /**
     * 把毫秒数格式化为 MM:SS。
     * @param milliseconds 非负时间长度，单位毫秒。
     * @return 两位分钟和秒组成的文本。
     */
    static QString formatDuration(qint64 milliseconds);

    QMediaPlayer *mediaPlayer; // Qt 标准媒体播放器，负责 H.264 和容器格式。
    QMediaPlaylist *playlist;  // QMediaPlayer 使用的媒体播放列表。
    QVideoWidget *videoOutput; // QMediaPlayer 的视频画面输出控件。

    QLabel *mjpegOutput;        // 显示旧 MJPEG 当前解码帧或无画面提示。
    QStackedWidget *videoStack; // 在 videoOutput 和 mjpegOutput 之间切换。

    QListWidget *videoList; // 按修改时间从新到旧显示所有录像。

    QSlider *positionSlider; // 标准录像表示毫秒位置，MJPEG 模式表示帧下标。
    QSlider *volumeSlider;   // 0～100 的媒体播放音量。

    QPushButton *playPauseButton;      // 播放/暂停切换按钮。
    QPushButton *nextButton;           // 播放下一段录像。
    QPushButton *volumeDownButton;     // 音量减少 5。
    QPushButton *volumeUpButton;       // 音量增加 5。
    QPushButton *playlistToggleButton; // 显示或隐藏录像列表面板。
    QPushButton *refreshButton;        // 手动触发目录扫描。

    QWidget *playlistPanel;   // 包含标题、状态、列表和刷新按钮的右侧面板。
    QWidget *controlsOverlay; // 预留控制层成员，当前实现没有使用。

    QLabel *currentTimeLabel;     // 显示当前时间 MM:SS。
    QLabel *totalTimeLabel;       // 显示总时长 /MM:SS。
    QLabel *recordingStatusLabel; // 显示扫描结果或当前播放文件。

    QTimer *recordingScanTimer; // 每 2000ms 自动扫描一次录像目录。
    QTimer *mjpegPlaybackTimer; // 每 66ms 显示一帧旧 MJPEG，约 15 FPS。

    QStringList recordingPaths;        // 当前发现的全部录像绝对路径。
    QVector<QByteArray> mjpegFrames;   // 当前旧 MJPEG 文件内全部 JPEG 帧字节。
    QString loadedLatestRecordingPath; // 防止同一段最新录像被定时器重复自动加载。
    int currentRecordingIndex;         // 当前录像在 recordingPaths 中的下标，-1 表示未选择。
    int mjpegFrameIndex;               // 下一次 updateMjpegFrame() 使用的帧下标。
    bool rawMjpegMode;                 // true 表示当前绕过 QMediaPlayer 使用旧 MJPEG 播放。
};

#endif // VIDEOPLAYER_H：结束头文件保护。
