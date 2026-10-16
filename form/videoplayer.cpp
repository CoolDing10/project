/*
 * videoplayer.cpp — 事件录像播放器页面实现
 *
 * 播放路径分为两类：
 *   1. .h264 和标准容器交给 QMediaPlayer/QVideoWidget；
 *   2. 历史 .mjpeg/.mjpg 由本类搜索 JPEG 起止标记并定时显示。
 */
#include "videoplayer.h" // 引入 VideoPlayer 类、成员和槽函数声明。

#include <QCoreApplication> // 获取可执行程序目录，定位 data/recordings。
#include <QDebug>           // 输出录像读取和解析错误。
#include <QDateTime>        // 支持文件时间相关 Qt 类型。
#include <QDir>             // 创建并扫描事件录像目录。
#include <QFile>            // 读取旧 MJPEG 录像全部字节。
#include <QFileInfo>        // 获取录像文件名、后缀、大小和修改时间。
#include <QFont>            // 配置播放时间标签字体。
#include <QHBoxLayout>      // 水平排列播放器、列表和控制项。
#include <QLabel>           // 显示 MJPEG 帧、时间和状态文字。
#include <QListWidget>      // 显示事件录像列表。
#include <QMediaPlaylist>   // 提供 Qt 媒体播放列表对象。
#include <QMediaContent>    // 将本地录像 URL 包装成 QMediaPlayer 输入。
#include <QPalette>         // 设置时间标签文字颜色。
#include <QPushButton>      // 创建播放、音量、列表和刷新按钮。
#include <QResizeEvent>     // 页面缩放时重新绘制 MJPEG 帧。
#include <QSettings>        // 读取边缘监控页保存的最新录像路径。
#include <QSlider>          // 创建播放进度和音量滑块。
#include <QStackedWidget>   // 在 QVideoWidget 和 MJPEG QLabel 间切换。
#include <QTimer>           // 周期扫描目录和播放 MJPEG 帧。
#include <QUrl>             // 将本地文件路径转换为媒体 URL。
#include <QVBoxLayout>      // 垂直排列播放画面、进度和控制栏。
#include <QVideoWidget>     // 显示 QMediaPlayer 解码后的视频画面。

/**
 * 创建事件录像页面。
 * @param parent Qt 父控件；主窗口页面栈负责本页面生命周期。
 */
VideoPlayer::VideoPlayer(QWidget *parent)
    : QWidget(parent),                         // 初始化 QWidget 基类。
      mediaPlayer(nullptr),                    // initializeMediaPlayer() 中创建。
      playlist(nullptr),                       // initializeMediaPlayer() 中创建。
      videoOutput(nullptr),                    // initializeLayout() 中创建。
      mjpegOutput(nullptr),                    // initializeLayout() 中创建。
      videoStack(nullptr),                     // initializeLayout() 中创建。
      videoList(nullptr),                      // initializeLayout() 中创建。
      positionSlider(nullptr),                 // initializeLayout() 中创建。
      volumeSlider(nullptr),                   // initializeLayout() 中创建。
      playPauseButton(nullptr),                // initializeLayout() 中创建。
      nextButton(nullptr),                     // initializeLayout() 中创建。
      volumeDownButton(nullptr),               // initializeLayout() 中创建。
      volumeUpButton(nullptr),                 // initializeLayout() 中创建。
      playlistToggleButton(nullptr),           // initializeLayout() 中创建。
      refreshButton(nullptr),                  // initializeLayout() 中创建。
      playlistPanel(nullptr),                  // initializeLayout() 中创建。
      controlsOverlay(nullptr),                // 当前实现保留但未创建或使用。
      currentTimeLabel(nullptr),               // initializeLayout() 中创建。
      totalTimeLabel(nullptr),                 // initializeLayout() 中创建。
      recordingStatusLabel(nullptr),           // initializeLayout() 中创建。
      recordingScanTimer(new QTimer(this)),    // 创建 2 秒录像目录扫描定时器。
      mjpegPlaybackTimer(new QTimer(this)),    // 创建旧 MJPEG 逐帧播放定时器。
      currentRecordingIndex(-1),               // -1 表示启动时尚未选择录像。
      mjpegFrameIndex(0),                      // 旧 MJPEG 从第 0 帧开始。
      rawMjpegMode(false)                      // 默认走 QMediaPlayer 路径。
{
    applyStyleSheet();                         // 安装页面暗色样式。
    initializeLayout();                        // 创建并排列全部可见控件。
    initializeMediaPlayer();                   // 创建 Qt 媒体播放器并连接信号。
    scanVideoFiles(true);                      // 首次扫描并选择最新录像。
    recordingScanTimer->setInterval(2000);     // 每 2000ms 检查是否产生新录像。
    connect(recordingScanTimer, &QTimer::timeout, // 目录扫描定时器超时信号。
            this, &VideoPlayer::refreshRecordings); // 触发一次重新扫描。
    recordingScanTimer->start();               // 启动持续目录监视。
    mjpegPlaybackTimer->setInterval(66);       // 66ms 一帧约等于 15 FPS。
    connect(mjpegPlaybackTimer, &QTimer::timeout, // MJPEG 帧定时器超时信号。
            this, &VideoPlayer::showNextMjpegFrame); // 显示下一张 JPEG。
}

/** 给事件录像页面和内部控件安装暗色样式。 */
void VideoPlayer::applyStyleSheet()
{
    setObjectName(QStringLiteral("recordingPlayer")); // 设置根对象名供 #recordingPlayer 选择。
    setStyleSheet(QStringLiteral(                     // 把下面拼接的 QSS 应用于本页面。
        "#recordingPlayer { background:#1b2028; color:#eef2f7; }" // 页面背景。
        "#recordingViewport { background:#05070a; border:1px solid #303a47; " // 播放框前半段。
        "border-radius:8px; color:#8291a3; }"                         // 播放框圆角和文字。
        "#recordingSidePanel { background:#151a21; border:1px solid #303a47; " // 列表面板。
        "border-radius:8px; }"                                       // 列表面板圆角。
        "#recordingTitle { color:white; font-size:15px; font-weight:600; }" // 列表标题。
        "#recordingStatus { color:#8fa0b3; font-size:11px; }"       // 状态文字。
        "QListWidget { background:#10141a; color:#e5eaf0; border:1px solid #303a47; " // 列表。
        "border-radius:5px; outline:none; }"                         // 列表圆角。
        "QListWidget::item { min-height:38px; padding:4px 8px; "    // 普通列表项。
        "border-bottom:1px solid #242d38; }"                         // 列表项分隔线。
        "QListWidget::item:selected { background:#176fa6; color:white; }" // 选中项。
        "QPushButton { min-height:34px; padding:2px 12px; color:#17202a; " // 按钮前半段。
        "background:#e8edf2; border:none; border-radius:6px; }"      // 按钮背景。
        "QPushButton:hover { background:#ffffff; }"                  // 悬停按钮。
        "QPushButton:checked { background:#1685c5; color:white; }"   // 选中按钮。
        "QLabel { color:#eef2f7; }"                                  // 普通标签。
        "QSlider::groove:horizontal { height:5px; background:#303a47; " // 滑轨前半段。
        "border-radius:2px; }"                                       // 滑轨圆角。
        "QSlider::sub-page:horizontal { background:#22a5e5; border-radius:2px; }" // 已经过区段。
        "QSlider::handle:horizontal { width:14px; margin:-5px 0; "   // 滑块手柄前半段。
        "background:white; border-radius:7px; }"));                  // 滑块手柄背景。
}

/** 创建播放区、录像列表、控制栏并连接所有用户操作信号。 */
void VideoPlayer::initializeLayout()
{
    videoOutput = new QVideoWidget(this);                         // 创建标准媒体视频输出控件。
    videoOutput->setObjectName(QStringLiteral("recordingViewport")); // 使用统一播放框样式。
    mjpegOutput = new QLabel(QStringLiteral("暂无录像画面"), this); // 创建旧 MJPEG 显示标签。
    mjpegOutput->setObjectName(QStringLiteral("recordingViewport")); // 同样使用播放框样式。
    mjpegOutput->setAlignment(Qt::AlignCenter);                   // 图片和提示文字居中。
    mjpegOutput->setMinimumSize(320, 240);                        // 保证最小可视区域。
    mjpegOutput->setSizePolicy(QSizePolicy::Expanding,            // 水平方向尽量扩展。
                               QSizePolicy::Expanding);           // 垂直方向尽量扩展。

    videoStack = new QStackedWidget(this);                        // 创建两种渲染控件容器。
    videoStack->addWidget(videoOutput);                           // 第一个页面是 QMediaPlayer 输出。
    videoStack->addWidget(mjpegOutput);                           // 第二个页面是旧 MJPEG QLabel。
    videoStack->setCurrentWidget(mjpegOutput);                    // 未播放时先显示提示标签。

    videoList = new QListWidget(this);                            // 创建事件录像列表。
    videoList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff); // 禁止横向滚动条。

    playlistPanel = new QWidget(this);                            // 创建右侧列表面板容器。
    playlistPanel->setObjectName(QStringLiteral("recordingSidePanel")); // 应用侧栏 QSS。
    playlistPanel->setMinimumWidth(220);                          // 列表面板最窄 220 像素。
    playlistPanel->setMaximumWidth(280);                          // 列表面板最宽 280 像素。

    QLabel *listTitle =                                          // 局部变量：录像列表标题标签。
        new QLabel(QStringLiteral("录像文件"), playlistPanel);    // 父对象为列表面板。
    listTitle->setObjectName(QStringLiteral("recordingTitle"));   // 应用标题样式。
    recordingStatusLabel =                                      // 创建扫描/播放状态标签。
        new QLabel(QStringLiteral("正在扫描事件录像..."),         // 初始状态文字。
                   playlistPanel);                               // 父对象为列表面板。
    recordingStatusLabel->setObjectName(QStringLiteral("recordingStatus")); // 应用状态样式。
    recordingStatusLabel->setWordWrap(true);                     // 长状态文字允许换行。
    refreshButton =                                              // 创建手动刷新按钮。
        new QPushButton(QStringLiteral("刷新录像"), playlistPanel); // 父对象为列表面板。
    QVBoxLayout *playlistLayout =                                // 局部变量：列表面板垂直布局。
        new QVBoxLayout(playlistPanel);                           // 安装到 playlistPanel。
    playlistLayout->setContentsMargins(10, 10, 10, 10);          // 设置四周 10 像素边距。
    playlistLayout->setSpacing(8);                               // 相邻控件间距为 8。
    playlistLayout->addWidget(listTitle);                        // 顶部放标题。
    playlistLayout->addWidget(recordingStatusLabel);             // 标题下放状态。
    playlistLayout->addWidget(videoList, 1);                     // 列表占据剩余空间。
    playlistLayout->addWidget(refreshButton);                    // 底部放刷新按钮。

    positionSlider = new QSlider(Qt::Horizontal, this);          // 创建水平播放进度条。
    positionSlider->setMaximumHeight(15);                        // 限制进度条高度。
    positionSlider->setObjectName(QStringLiteral("durationSlider")); // 设置调试/样式对象名。

    volumeSlider = new QSlider(Qt::Horizontal, this);            // 创建水平音量条。
    volumeSlider->setRange(0, 100);                              // 音量合法范围为 0～100。
    volumeSlider->setMaximumWidth(80);                           // 音量条最大宽度 80。
    volumeSlider->setObjectName(QStringLiteral("volumeSlider")); // 设置对象名。
    volumeSlider->setValue(50);                                  // 默认音量 50%。

    playPauseButton = new QPushButton(QStringLiteral("播放/暂停"), this); // 创建播放暂停按钮。
    nextButton = new QPushButton(QStringLiteral("下一段"), this);         // 创建下一段按钮。
    volumeDownButton = new QPushButton(QStringLiteral("音量 -"), this);   // 创建减音量按钮。
    volumeUpButton = new QPushButton(QStringLiteral("音量 +"), this);     // 创建加音量按钮。
    playlistToggleButton = new QPushButton(QStringLiteral("录像列表"), this); // 创建列表开关按钮。
    playPauseButton->setCheckable(true);                         // 允许按钮保存播放/暂停选中状态。
    playlistToggleButton->setCheckable(true);                    // 允许按钮保存列表显示状态。

    QPalette timePalette;                                       // 局部调色板用于时间文字。
    timePalette.setColor(QPalette::WindowText, Qt::white);       // 时间文字设为白色。
    QFont timeFont;                                              // 局部字体用于时间标签。
    timeFont.setPixelSize(18);                                   // 时间字体大小为 18 像素。

    currentTimeLabel = new QLabel(QStringLiteral("00:00"), this); // 创建当前时间标签。
    totalTimeLabel = new QLabel(QStringLiteral("/00:00"), this);  // 创建总时长标签。
    currentTimeLabel->setFont(timeFont);                         // 应用 18 像素字体。
    totalTimeLabel->setFont(timeFont);                           // 总时长使用相同字体。
    currentTimeLabel->setPalette(timePalette);                   // 当前时间设为白色。
    totalTimeLabel->setPalette(timePalette);                     // 总时长设为白色。

    QWidget *progressContainer = new QWidget(this);              // 局部容器承载进度条。
    QHBoxLayout *progressLayout =                                // 局部水平布局管理进度条。
        new QHBoxLayout(progressContainer);                      // 安装到进度容器。
    progressLayout->addWidget(positionSlider);                   // 进度条填入容器。
    progressLayout->setContentsMargins(0, 0, 0, 0);             // 去除容器额外边距。

    QWidget *controlBar = new QWidget(this);                     // 局部容器承载所有控制按钮。
    controlBar->setMinimumHeight(48);                            // 控制栏至少高 48 像素。
    QHBoxLayout *controlLayout =                                 // 局部水平布局管理控制栏。
        new QHBoxLayout(controlBar);                             // 安装到 controlBar。
    controlLayout->addWidget(playPauseButton);                   // 添加播放/暂停。
    controlLayout->addWidget(nextButton);                        // 添加下一段。
    controlLayout->addWidget(volumeDownButton);                  // 添加音量减。
    controlLayout->addWidget(volumeSlider);                      // 添加音量滑块。
    controlLayout->addWidget(volumeUpButton);                    // 添加音量加。
    controlLayout->addWidget(currentTimeLabel);                  // 添加当前时间。
    controlLayout->addWidget(totalTimeLabel);                    // 添加总时长。
    controlLayout->addStretch();                                 // 弹性空间把列表按钮推到右侧。
    controlLayout->addWidget(playlistToggleButton);              // 添加录像列表开关。
    controlLayout->setContentsMargins(0, 0, 0, 0);              // 去除控制栏内部边距。

    QWidget *playerPanel = new QWidget(this);                    // 局部容器组合视频、进度和控制栏。
    QVBoxLayout *playerLayout =                                  // 局部垂直布局管理左侧播放器。
        new QVBoxLayout(playerPanel);                            // 安装到 playerPanel。
    playerLayout->setContentsMargins(0, 0, 0, 0);               // 去除播放器容器边距。
    playerLayout->setSpacing(8);                                 // 三部分间距为 8。
    playerLayout->addWidget(videoStack, 1);                      // 播放画面占据剩余空间。
    playerLayout->addWidget(progressContainer);                  // 画面下方放进度条。
    playerLayout->addWidget(controlBar);                         // 最下方放控制栏。

    QHBoxLayout *contentLayout = new QHBoxLayout(this);          // 页面根水平布局。
    contentLayout->setContentsMargins(12, 12, 12, 12);          // 页面四周保留 12 像素。
    contentLayout->setSpacing(12);                               // 播放区和列表间距 12。
    contentLayout->addWidget(playerPanel, 1);                    // 左侧播放器可伸缩。
    contentLayout->addWidget(playlistPanel);                     // 右侧列表保持限制宽度。

    connect(playPauseButton, &QPushButton::clicked,              // 播放按钮点击信号。
            this, &VideoPlayer::togglePlayback);                 // 切换播放暂停。
    connect(nextButton, &QPushButton::clicked,                   // 下一段按钮点击信号。
            this, &VideoPlayer::playNextVideo);                  // 播放下一段。
    connect(volumeDownButton, &QPushButton::clicked,             // 音量减按钮点击信号。
            this, &VideoPlayer::decreaseVolume);                 // 音量减少 5。
    connect(volumeUpButton, &QPushButton::clicked,               // 音量加按钮点击信号。
            this, &VideoPlayer::increaseVolume);                 // 音量增加 5。
    connect(playlistToggleButton, &QPushButton::clicked,         // 列表开关按钮点击信号。
            this, &VideoPlayer::togglePlaylistPanel);            // 显示或隐藏面板。
    connect(videoList, &QListWidget::itemClicked,                // 用户点击录像列表项。
            this, &VideoPlayer::playSelectedVideo);              // 播放对应录像。
    connect(positionSlider, &QSlider::sliderReleased,            // 用户释放进度滑块。
            this, &VideoPlayer::seekToSliderPosition);           // 跳到指定时间或帧。
    connect(volumeSlider, &QSlider::sliderReleased,              // 用户释放音量滑块。
            this, &VideoPlayer::applySliderVolume);              // 应用选定音量。
    connect(refreshButton, &QPushButton::clicked,                // 刷新按钮点击信号。
            this, &VideoPlayer::refreshRecordings);              // 立即扫描录像目录。
}

/** 创建 Qt 媒体播放器，并连接状态、时长和位置更新信号。 */
void VideoPlayer::initializeMediaPlayer()
{
    mediaPlayer = new QMediaPlayer(this);                        // 创建播放器并交给页面管理。
    playlist = new QMediaPlaylist(this);                         // 创建播放列表并交给页面管理。
    mediaPlayer->setVideoOutput(videoOutput);                    // 将解码画面输出到 QVideoWidget。
    mediaPlayer->setVolume(volumeSlider->value());               // 应用 UI 默认音量 50。

    connect(mediaPlayer, &QMediaPlayer::stateChanged,            // 播放器状态变化信号。
            this, &VideoPlayer::handlePlayerStateChanged);       // 同步播放按钮。
    connect(mediaPlayer, &QMediaPlayer::durationChanged,         // 总时长变化信号。
            this, &VideoPlayer::handleDurationChanged);          // 更新进度范围和总时间。
    connect(mediaPlayer, &QMediaPlayer::positionChanged,         // 当前播放位置变化信号。
            this, &VideoPlayer::handlePositionChanged);          // 更新进度值和当前时间。
}

/**
 * 页面尺寸变化事件。
 * @param event 包含新的页面尺寸，由 QWidget 事件系统传入。
 */
void VideoPlayer::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);                                 // 先保留 QWidget 默认缩放处理。
    if (rawMjpegMode && !mjpegFrames.isEmpty())                  // 只有旧 MJPEG 有手动缩放的像素图。
        updateMjpegFrame();                                      // 按新的标签尺寸重新生成 QPixmap。
}

/** 根据当前模式切换播放和暂停。 */
void VideoPlayer::togglePlayback()
{
    if (rawMjpegMode)                                            // 旧 MJPEG 由定时器控制播放。
    {
        if (mjpegFrames.isEmpty())                               // 没有成功解析的帧时不能播放。
            return;                                              // 保持停止状态。
        if (mjpegPlaybackTimer->isActive())                      // 定时器正在运行代表正在播放。
            mjpegPlaybackTimer->stop();                          // 停止定时器即暂停。
        else                                                     // 定时器未运行代表暂停。
            mjpegPlaybackTimer->start();                         // 按 66ms 周期恢复播放。
        playPauseButton->setChecked(                             // 同步按钮选中状态。
            mjpegPlaybackTimer->isActive());                     // 活跃代表“正在播放”。
        return;                                                  // MJPEG 路径处理完成。
    }
    if (mediaPlayer->state() == QMediaPlayer::PlayingState)      // 标准媒体当前正在播放。
        mediaPlayer->pause();                                    // 调用播放器暂停。
    else                                                         // 停止或暂停状态。
        mediaPlayer->play();                                     // 调用播放器开始/继续播放。
}

/** 以循环方式播放 recordingPaths 中的下一段录像。 */
void VideoPlayer::playNextVideo()
{
    if (recordingPaths.isEmpty())                                // 没有录像时不能计算下一项。
        return;                                                  // 直接结束。
    const int nextIndex = currentRecordingIndex < 0              // 尚未选中任何录像？
                              ? 0                                // 从最新的第 0 项开始。
                              : (currentRecordingIndex + 1) %    // 否则当前下标加一。
                                    recordingPaths.size();       // 取模使最后一项后回到第 0 项。
    playRecordingAtRow(nextIndex);                               // 统一走按行播放逻辑。
}

/** 把音量滑块增加 5 并应用到播放器。 */
void VideoPlayer::increaseVolume()
{
    volumeSlider->setValue(volumeSlider->value() + 5);           // QSlider 自动限制最大值 100。
    applySliderVolume();                                         // 将新值写入 QMediaPlayer。
}

/** 把音量滑块减少 5 并应用到播放器。 */
void VideoPlayer::decreaseVolume()
{
    volumeSlider->setValue(volumeSlider->value() - 5);           // QSlider 自动限制最小值 0。
    applySliderVolume();                                         // 将新值写入 QMediaPlayer。
}

/** 根据列表开关按钮状态显示或隐藏右侧面板。 */
void VideoPlayer::togglePlaylistPanel()
{
    playlistPanel->setVisible(                                   // 设置右侧面板可见状态。
        !playlistToggleButton->isChecked());                     // 按钮选中时隐藏，未选中时显示。
}

/**
 * 同步标准媒体播放按钮。
 * @param state QMediaPlayer 当前状态。
 */
void VideoPlayer::handlePlayerStateChanged(QMediaPlayer::State state)
{
    if (!rawMjpegMode)                                           // MJPEG 状态由定时器单独维护。
        playPauseButton->setChecked(                             // 更新播放按钮选中状态。
            state == QMediaPlayer::PlayingState);                // 只有 PlayingState 设为选中。
}

/**
 * 播放用户点击的录像。
 * @param item 被点击的列表项。
 */
void VideoPlayer::playSelectedVideo(QListWidgetItem *item)
{
    if (!item)                                                   // 防御空列表项指针。
        return;                                                  // 没有目标时不播放。

    playRecordingAtRow(videoList->row(item));                    // 将列表项换算成下标后播放。
}

/**
 * 同步 QMediaPlaylist 下标到 UI 列表。
 * @param index 播放列表当前下标。
 */
void VideoPlayer::handlePlaylistIndexChanged(int index)
{
    if (index >= 0)                                              // -1 表示没有有效媒体项。
        videoList->setCurrentRow(index);                         // 选中对应 UI 行。
}

/**
 * 更新标准媒体总时长。
 * @param durationMilliseconds 总时长，单位毫秒。
 */
void VideoPlayer::handleDurationChanged(qint64 durationMilliseconds)
{
    if (rawMjpegMode)                                            // MJPEG 总时长按固定 15 FPS 自行计算。
        return;                                                  // 忽略 QMediaPlayer 信号。
    positionSlider->setRange(                                    // 进度滑块单位采用秒。
        0, static_cast<int>(durationMilliseconds / 1000));       // 毫秒除以 1000 得总秒数。
    totalTimeLabel->setText(                                     // 更新“/MM:SS”总时间文字。
        QStringLiteral("/") + formatDuration(durationMilliseconds)); // 格式化毫秒。
}

/**
 * 更新标准媒体当前播放位置。
 * @param positionMilliseconds 当前位置，单位毫秒。
 */
void VideoPlayer::handlePositionChanged(qint64 positionMilliseconds)
{
    if (rawMjpegMode)                                            // MJPEG 位置由帧索引维护。
        return;                                                  // 忽略 QMediaPlayer 信号。
    if (!positionSlider->isSliderDown())                         // 用户拖动时不能自动抢回滑块位置。
        positionSlider->setValue(                                // 非拖动状态同步播放器位置。
            static_cast<int>(positionMilliseconds / 1000));      // 毫秒转换为滑块使用的秒。
    currentTimeLabel->setText(                                   // 更新时间标签。
        formatDuration(positionMilliseconds));                   // 格式化为 MM:SS。
}

/** 用户释放进度滑块后跳转到指定媒体位置或 MJPEG 帧。 */
void VideoPlayer::seekToSliderPosition()
{
    if (rawMjpegMode)                                            // MJPEG 滑块值直接代表帧下标。
    {
        mjpegFrameIndex = qBound(                                // 把目标帧严格限制到合法范围。
            0,                                                   // 最小帧下标是 0。
            positionSlider->value(),                             // 用户选择的目标下标。
            qMax(0, mjpegFrames.size() - 1));                    // 最大值为最后一帧，空列表时为 0。
        updateMjpegFrame();                                      // 立即显示用户选择的帧。
        return;                                                  // 不调用 QMediaPlayer。
    }
    mediaPlayer->setPosition(                                    // 标准媒体跳转接口单位为毫秒。
        static_cast<qint64>(positionSlider->value()) * 1000);    // 滑块秒数乘 1000。
}

/** 把音量滑块值写入 QMediaPlayer。 */
void VideoPlayer::applySliderVolume()
{
    mediaPlayer->setVolume(volumeSlider->value());               // 合法范围已由滑块限制为 0～100。
}

/** 手动或定时重新扫描录像，并允许选择最新录像。 */
void VideoPlayer::refreshRecordings()
{
    scanVideoFiles(true);                                        // true 表示必要时自动播放最新录像。
}

/**
 * 扫描录像目录并刷新列表。
 * @param selectLatest 没有当前项时是否选择最新录像。
 */
void VideoPlayer::scanVideoFiles(bool selectLatest)
{
    const QString directoryPath =                               // 保存事件录像目录完整路径。
        QDir(QCoreApplication::applicationDirPath())             // 从可执行程序目录开始。
            .filePath(QStringLiteral("data/recordings"));        // 拼接 data/recordings。
    QDir().mkpath(directoryPath);                                // 目录不存在时递归创建。
    const QDir videoDirectory(directoryPath);                    // 创建用于扫描的目录对象。

    const QStringList filters = {                                // 支持的录像扩展名筛选器。
        QStringLiteral("*.mp4"), QStringLiteral("*.mkv"),        // 标准 MP4 和 MKV 容器。
        QStringLiteral("*.wmv"), QStringLiteral("*.avi"),        // 兼容 WMV 和 AVI 容器。
        QStringLiteral("*.h264"),                                // 当前事件录像使用的 H.264 裸码流。
        QStringLiteral("*.mjpeg"), QStringLiteral("*.mjpg")};    // 兼容历史 MJPEG 裸流。
    const QFileInfoList files = videoDirectory.entryInfoList(    // 扫描得到录像文件信息列表。
        filters,                                                 // 只接受上述扩展名。
        QDir::Files,                                             // 只列普通文件，不列目录。
        QDir::Time);                                             // 按修改时间从新到旧排序。

    QStringList discoveredPaths;                                 // 保存本次扫描得到的绝对路径。
    for (const QFileInfo &fileInfo : files)                      // 逐个处理扫描结果。
        discoveredPaths.append(fileInfo.absoluteFilePath());     // 将文件完整路径加入新列表。

    const QString currentPath =                                  // 保存刷新前当前录像路径。
        currentRecordingIndex >= 0 &&                            // 当前下标必须不小于 0。
                currentRecordingIndex < recordingPaths.size()    // 当前下标也必须没有越界。
            ? recordingPaths.at(currentRecordingIndex)           // 合法时取得当前绝对路径。
            : QString();                                         // 无当前项时使用空字符串。
    if (discoveredPaths != recordingPaths)                       // 只有目录内容改变才重建 UI 列表。
    {
        recordingPaths = discoveredPaths;                        // 用最新扫描结果替换成员列表。
        videoList->clear();                                      // 删除旧 UI 列表项。
        for (const QString &path : recordingPaths)               // 为每个录像路径创建一项。
        {
            const QFileInfo info(path);                          // 获取当前录像的名称、时间和大小。
            videoList->addItem(                                  // 向列表末尾加入格式化文字。
                QStringLiteral("%1\n%2 · %3 KB")                  // 第一行文件名，第二行时间和大小。
                    .arg(info.fileName(),                        // %1 替换为纯文件名。
                         info.lastModified().toString(           // %2 替换为最后修改时间。
                             QStringLiteral("MM-dd HH:mm:ss")))   // 时间显示格式。
                    .arg(qMax<qint64>(1, info.size() / 1024)));  // %3 为至少 1KB 的整数大小。
        }
        currentRecordingIndex =                                  // 尝试在新列表中恢复当前录像。
            recordingPaths.indexOf(currentPath);                 // 找不到时自动得到 -1。
        if (currentRecordingIndex >= 0)                           // 旧当前录像仍然存在。
            videoList->setCurrentRow(currentRecordingIndex);     // 恢复 UI 选中行。
    }

    recordingStatusLabel->setText(                               // 根据扫描结果更新状态说明。
        recordingPaths.isEmpty()                                 // 当前是否一段录像都没有。
            ? QStringLiteral("暂无事件录像。触发画面变化事件后将自动出现。") // 空目录提示。
            : QStringLiteral("已发现 %1 段录像，最新录像排在最前。") // 非空目录提示模板。
                  .arg(recordingPaths.size()));                  // %1 替换为录像数量。

    QSettings settings;                                          // 使用主程序组织名和应用名打开配置。
    const QString latestPath =                                   // 保存边缘监控页记录的最新完成录像。
        QFileInfo(settings.value(                                // 从 QSettings 读取配置值。
                      QStringLiteral("edge/latestRecordingPath")) // 配置键。
                      .toString())                               // QVariant 转成路径字符串。
            .absoluteFilePath();                                 // 统一转换成绝对路径便于比较。
    const int latestIndex = recordingPaths.indexOf(latestPath);  // 查找最新录像在扫描列表中的位置。
    const bool newCompletedRecording =                           // 判断是否发现新的已完成录像。
        latestIndex >= 0 &&                                      // 最新路径必须真实存在于列表。
        latestPath != loadedLatestRecordingPath;                 // 且不能是上次已经自动加载的同一路径。
    if (newCompletedRecording ||                                 // 优先自动播放新完成录像。
        (selectLatest && !recordingPaths.isEmpty() &&             // 或调用者要求选择最新且列表非空。
         currentRecordingIndex < 0))                             // 同时当前没有有效选中项。
    {
        const int row = newCompletedRecording ? latestIndex : 0; // 新录像用其下标，否则选第 0 项。
        loadedLatestRecordingPath = recordingPaths.at(row);      // 记录已自动加载路径，防止重复。
        playRecordingAtRow(row);                                 // 播放目标录像。
    }
}

/**
 * 播放 recordingPaths 中指定行。
 * @param row 目标录像下标。
 */
void VideoPlayer::playRecordingAtRow(int row)
{
    if (row < 0 || row >= recordingPaths.size())                 // 下标必须位于列表有效范围。
        return;                                                  // 越界时不改变当前播放状态。

    stopMjpegPlayback();                                         // 停止并清除可能存在的旧 MJPEG。
    mediaPlayer->stop();                                         // 停止可能存在的标准媒体播放。
    currentRecordingIndex = row;                                 // 保存新的当前录像下标。
    videoList->setCurrentRow(row);                               // 同步 UI 选中行。
    const QString filePath = recordingPaths.at(row);             // 取得目标录像绝对路径。
    const QString suffix =                                       // 保存不区分大小写的文件后缀。
        QFileInfo(filePath).suffix().toLower();                  // 例如 h264、mjpeg 或 mp4。
    if (suffix == QLatin1String("mjpeg") ||                      // 旧 .mjpeg 后缀？
        suffix == QLatin1String("mjpg"))                         // 或旧 .mjpg 后缀？
    {
        if (!loadMjpegRecording(filePath))                       // 读取并解析连续 JPEG 帧。
        {
            recordingStatusLabel->setText(                       // 在页面显示读取失败。
                QStringLiteral("录像读取失败：%1")               // 错误文字模板。
                    .arg(QFileInfo(filePath).fileName()));       // %1 替换为纯文件名。
            return;                                              // 不进入 MJPEG 播放模式。
        }
        rawMjpegMode = true;                                     // 后续控制走自定义 MJPEG 路径。
        videoStack->setCurrentWidget(mjpegOutput);               // 切换到 QLabel 渲染页。
        mjpegPlaybackTimer->start();                             // 以 66ms 周期开始逐帧播放。
        playPauseButton->setChecked(true);                       // 同步按钮为播放状态。
    }
    else                                                        // 其他格式交给 Qt Multimedia。
    {
        rawMjpegMode = false;                                    // 后续控制走 QMediaPlayer。
        videoStack->setCurrentWidget(videoOutput);               // 切换到 QVideoWidget。
        mediaPlayer->setMedia(                                   // 设置新的播放器输入媒体。
            QMediaContent(QUrl::fromLocalFile(filePath)));       // 本地路径转成 file URL。
        mediaPlayer->play();                                     // 立即开始播放。
    }
    recordingStatusLabel->setText(                               // 显示当前播放文件。
        QStringLiteral("正在播放：%1")                           // 状态文字模板。
            .arg(QFileInfo(filePath).fileName()));               // 只显示文件名，不显示长绝对路径。
}

/**
 * 读取旧 MJPEG 文件并按 JPEG SOI/EOI 标志拆帧。
 * @param filePath 旧录像完整路径。
 * @return 成功提取至少一帧返回 true。
 */
bool VideoPlayer::loadMjpegRecording(const QString &filePath)
{
    QFile file(filePath);                                        // 创建指向目标录像的局部文件对象。
    const qint64 maximumRecordingBytes =                          // 限制一次性载入内存的最大文件大小。
        128LL * 1024 * 1024;                                     // 上限为 128 MiB。
    if (!file.open(QIODevice::ReadOnly) ||                        // 文件必须能以只读方式打开。
        file.size() <= 0 ||                                      // 文件必须包含数据。
        file.size() > maximumRecordingBytes)                     // 文件不能超过内存保护上限。
    {
        qWarning() << "MJPEG录像无法打开或文件过大：" << filePath; // 输出失败路径。
        return false;                                            // 不改变为 MJPEG 播放状态。
    }
    const QByteArray data = file.readAll();                       // 将允许大小内的完整旧录像读入内存。
    const QByteArray startMarker = QByteArray::fromHex("ffd8");   // JPEG SOI 起始字节 FF D8。
    const QByteArray endMarker = QByteArray::fromHex("ffd9");     // JPEG EOI 结束字节 FF D9。
    mjpegFrames.clear();                                         // 清空上一段 MJPEG 帧缓存。
    int searchPosition = 0;                                      // 从文件字节下标 0 开始搜索。
    while (searchPosition < data.size())                         // 尚未搜索到文件末尾时继续。
    {
        const int start = data.indexOf(startMarker, searchPosition); // 查找下一帧 SOI。
        if (start < 0)                                           // -1 表示再也没有起始标记。
            break;                                               // 拆帧结束。
        const int end = data.indexOf(endMarker, start + 2);      // 从 SOI 后寻找对应 EOI。
        if (end < 0)                                             // 缺少结束标记代表末帧不完整。
            break;                                               // 不保存残缺 JPEG。
        mjpegFrames.append(                                      // 把一帧完整 JPEG 字节加入缓存。
            data.mid(start, end - start + 2));                   // 长度包含结尾两个 EOI 字节。
        searchPosition = end + 2;                                // 下一轮从当前 EOI 之后继续。
    }
    if (mjpegFrames.isEmpty())                                   // 文件中没有任何完整 JPEG。
    {
        qWarning() << "MJPEG录像中没有有效JPEG帧：" << filePath; // 输出解析失败路径。
        return false;                                            // 告诉调用者无法播放。
    }

    mjpegFrameIndex = 0;                                         // 播放位置复位到第一帧。
    positionSlider->setRange(0, mjpegFrames.size() - 1);         // MJPEG 进度条范围改为帧下标。
    totalTimeLabel->setText(                                     // 按固定 15 FPS 估算总时长。
        QStringLiteral("/") +                                    // 保持总时间前导斜杠格式。
        formatDuration(qint64(mjpegFrames.size()) * 1000 / 15)); // 帧数÷15 转毫秒。
    updateMjpegFrame();                                          // 立即显示第一帧。
    qInfo() << "事件录像已载入：" << filePath                   // 输出加载成功文件。
            << "帧数=" << mjpegFrames.size();                   // 输出解析出的帧数。
    return true;                                                 // 通知调用者可以进入 MJPEG 模式。
}

/** 停止旧 MJPEG 播放并清空相关状态。 */
void VideoPlayer::stopMjpegPlayback()
{
    mjpegPlaybackTimer->stop();                                  // 停止继续触发下一帧。
    mjpegFrames.clear();                                         // 释放全部 JPEG 帧字节缓存。
    mjpegFrameIndex = 0;                                         // 帧位置复位。
    rawMjpegMode = false;                                        // 退出自定义 MJPEG 模式。
}

/** 按定时器周期显示下一张旧 MJPEG 帧。 */
void VideoPlayer::showNextMjpegFrame()
{
    if (!rawMjpegMode || mjpegFrames.isEmpty())                  // 必须处于 MJPEG 模式且有帧。
    {
        mjpegPlaybackTimer->stop();                              // 状态无效时停止空转定时器。
        return;                                                  // 不访问帧数组。
    }
    if (mjpegFrameIndex >= mjpegFrames.size())                   // 已经越过最后一帧。
        mjpegFrameIndex = 0;                                     // 回到第一帧循环播放。
    updateMjpegFrame();                                          // 解码并显示当前索引帧。
    ++mjpegFrameIndex;                                           // 为下次定时器准备下一帧下标。
}

/** 解码当前旧 MJPEG 帧并同步画面、进度和时间。 */
void VideoPlayer::updateMjpegFrame()
{
    if (mjpegFrames.isEmpty())                                   // 空缓存没有可解码内容。
        return;                                                  // 直接结束。
    const int frameIndex = qBound(                               // 得到严格合法的实际帧下标。
        0,                                                       // 最小下标。
        mjpegFrameIndex,                                         // 当前期望下标。
        mjpegFrames.size() - 1);                                 // 最大下标。
    const QImage image = QImage::fromData(                       // 使用 Qt 图片插件解码 JPEG 字节。
        mjpegFrames.at(frameIndex), "JPEG");                     // 输入指定帧并明确格式。
    if (image.isNull())                                          // 损坏 JPEG 会解码为空图。
        return;                                                  // 跳过本帧显示。
    mjpegOutput->setPixmap(                                      // 把解码图设置到 MJPEG 标签。
        QPixmap::fromImage(image).scaled(                        // QImage 转成 QPixmap 并缩放。
            mjpegOutput->size(),                                 // 目标尺寸使用当前标签尺寸。
            Qt::KeepAspectRatio,                                 // 保持原始宽高比，避免拉伸变形。
            Qt::FastTransformation));                            // 使用快速缩放降低 CPU 消耗。
    if (!positionSlider->isSliderDown())                         // 用户拖动时不要覆盖其位置。
        positionSlider->setValue(frameIndex);                    // 同步进度滑块到当前帧。
    currentTimeLabel->setText(                                   // 按固定 15 FPS 更新当前时间。
        formatDuration(qint64(frameIndex) * 1000 / 15));         // 帧下标换算成毫秒。
}

/**
 * 格式化媒体时间。
 * @param milliseconds 输入毫秒数，负数会按 0 处理。
 * @return MM:SS 格式文本。
 */
QString VideoPlayer::formatDuration(qint64 milliseconds)
{
    const qint64 totalSeconds =                                  // 保存截断后的非负总秒数。
        qMax<qint64>(0, milliseconds / 1000);                    // 毫秒除以 1000 并限制最小为 0。
    const qint64 minutes = totalSeconds / 60;                    // 整除 60 得到分钟部分。
    const qint64 seconds = totalSeconds % 60;                    // 对 60 取余得到秒部分。
    return QStringLiteral("%1:%2")                               // 构造两个占位符的时间模板。
        .arg(minutes, 2, 10, QLatin1Char('0'))                   // 分钟宽度 2，不足前补 0。
        .arg(seconds, 2, 10, QLatin1Char('0'));                  // 秒宽度 2，不足前补 0。
}
