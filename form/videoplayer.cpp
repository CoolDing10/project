/*
 * ============================================================================
 * videoplayer.cpp — 事件录像播放器页面实现
 * ============================================================================
 *
 * 本文件实现 VideoPlayer 类，提供事件录像的发现、列表、播放功能。
 *
 * 架构概览：
 *   布局 ── 水平分割：左侧播放区 + 右侧录像列表
 *   播放区 ── QStackedWidget 在两种渲染器间切换：
 *             ├── videoOutput (QVideoWidget)：用于 mp4/mkv 等标准容器格式
 *             └── mjpegOutput (QLabel)：用于 .mjpeg 原始 JPEG 流格式
 *   控制栏 ── 播放/暂停、下一段、音量加减、进度条、时间显示
 *
 * 双模式播放：
 *   1. 标准容器格式（QMediaPlayer 模式）
 *      - 使用 Qt Multimedia 框架播放
 *      - 支持硬件加速解码（取决于平台）
 *   2. MJPEG 原始流格式（rawMjpegMode）
 *      - 由 TcpFrameSender 录制的 .mjpeg 文件
 *      - 文件内容是连续的 JPEG 帧（FF D8 ... FF D9）
 *      - 使用 QTimer 逐帧解码并显示到 QLabel
 *
 * 自动发现：
 *   - recordingScanTimer 每 2 秒扫描 data/recordings/ 目录
 *   - 通过 QSettings 读取 latestRecordingPath，自动切换到最新录像
 */
#include "videoplayer.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMediaPlaylist>
#include <QMediaContent>
#include <QPalette>
#include <QPushButton>
#include <QResizeEvent>
#include <QSettings>
#include <QSlider>
#include <QStackedWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QVideoWidget>

/*
 * ===== 构造函数 =====
 * 初始化所有成员为 nullptr/默认值，构建 UI 布局，创建媒体播放器，
 * 启动录像扫描定时器。
 */
VideoPlayer::VideoPlayer(QWidget *parent)
    : QWidget(parent),
      mediaPlayer(nullptr),
      playlist(nullptr),
      videoOutput(nullptr),
      mjpegOutput(nullptr),
      videoStack(nullptr),
      videoList(nullptr),
      positionSlider(nullptr),
      volumeSlider(nullptr),
      playPauseButton(nullptr),
      nextButton(nullptr),
      volumeDownButton(nullptr),
      volumeUpButton(nullptr),
      playlistToggleButton(nullptr),
      refreshButton(nullptr),
      playlistPanel(nullptr),
      controlsOverlay(nullptr),
      currentTimeLabel(nullptr),
      totalTimeLabel(nullptr),
      recordingStatusLabel(nullptr),
      recordingScanTimer(new QTimer(this)),
      mjpegPlaybackTimer(new QTimer(this)),
      currentRecordingIndex(-1),
      mjpegFrameIndex(0),
      rawMjpegMode(false)
{
    applyStyleSheet();
    initializeLayout();
    initializeMediaPlayer();
    scanVideoFiles(true);
    recordingScanTimer->setInterval(2000);
    connect(recordingScanTimer, &QTimer::timeout,
            this, &VideoPlayer::refreshRecordings);
    recordingScanTimer->start();
    mjpegPlaybackTimer->setInterval(66);
    connect(mjpegPlaybackTimer, &QTimer::timeout,
            this, &VideoPlayer::showNextMjpegFrame);
}

/*
 * 设置暗色主题 QSS 样式表。
 * 使用 objectName 选择器限定各区域样式，避免污染子控件。
 */
void VideoPlayer::applyStyleSheet()
{
    setObjectName(QStringLiteral("recordingPlayer"));
    setStyleSheet(QStringLiteral(
        "#recordingPlayer { background:#1b2028; color:#eef2f7; }"
        "#recordingViewport { background:#05070a; border:1px solid #303a47; "
        "border-radius:8px; color:#8291a3; }"
        "#recordingSidePanel { background:#151a21; border:1px solid #303a47; "
        "border-radius:8px; }"
        "#recordingTitle { color:white; font-size:15px; font-weight:600; }"
        "#recordingStatus { color:#8fa0b3; font-size:11px; }"
        "QListWidget { background:#10141a; color:#e5eaf0; border:1px solid #303a47; "
        "border-radius:5px; outline:none; }"
        "QListWidget::item { min-height:38px; padding:4px 8px; "
        "border-bottom:1px solid #242d38; }"
        "QListWidget::item:selected { background:#176fa6; color:white; }"
        "QPushButton { min-height:34px; padding:2px 12px; color:#17202a; "
        "background:#e8edf2; border:none; border-radius:6px; }"
        "QPushButton:hover { background:#ffffff; }"
        "QPushButton:checked { background:#1685c5; color:white; }"
        "QLabel { color:#eef2f7; }"
        "QSlider::groove:horizontal { height:5px; background:#303a47; "
        "border-radius:2px; }"
        "QSlider::sub-page:horizontal { background:#22a5e5; border-radius:2px; }"
        "QSlider::handle:horizontal { width:14px; margin:-5px 0; "
        "background:white; border-radius:7px; }"));
}

/*
 * 构建完整的播放器 UI 布局：
 *
 *   ┌───────────────────────────────────────────────┬──────────────┐
 *   │  videoStack (QStackedWidget)                  │ playlistPanel│
 *   │  ├─ videoOutput (QVideoWidget)  标准格式       │ ├─ 标题       │
 *   │  └─ mjpegOutput (QLabel)        MJPEG 格式     │ ├─ 状态       │
 *   │                                               │ ├─ 录像列表   │
 *   │  progressContainer (进度条)                    │ └─ 刷新按钮   │
 *   │  controlBar (播放按钮 + 音量 + 时间)            │              │
 *   └───────────────────────────────────────────────┴──────────────┘
 */
void VideoPlayer::initializeLayout()
{
    videoOutput = new QVideoWidget(this);
    videoOutput->setObjectName(QStringLiteral("recordingViewport"));
    mjpegOutput = new QLabel(QStringLiteral("暂无录像画面"), this);
    mjpegOutput->setObjectName(QStringLiteral("recordingViewport"));
    mjpegOutput->setAlignment(Qt::AlignCenter);
    mjpegOutput->setMinimumSize(320, 240);
    mjpegOutput->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    videoStack = new QStackedWidget(this);
    videoStack->addWidget(videoOutput);
    videoStack->addWidget(mjpegOutput);
    videoStack->setCurrentWidget(mjpegOutput);

    videoList = new QListWidget(this);
    videoList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    playlistPanel = new QWidget(this);
    playlistPanel->setObjectName(QStringLiteral("recordingSidePanel"));
    playlistPanel->setMinimumWidth(220);
    playlistPanel->setMaximumWidth(280);

    QLabel *listTitle = new QLabel(QStringLiteral("录像文件"), playlistPanel);
    listTitle->setObjectName(QStringLiteral("recordingTitle"));
    recordingStatusLabel = new QLabel(QStringLiteral("正在扫描事件录像..."),
                                      playlistPanel);
    recordingStatusLabel->setObjectName(QStringLiteral("recordingStatus"));
    recordingStatusLabel->setWordWrap(true);
    refreshButton = new QPushButton(QStringLiteral("刷新录像"), playlistPanel);
    QVBoxLayout *playlistLayout = new QVBoxLayout(playlistPanel);
    playlistLayout->setContentsMargins(10, 10, 10, 10);
    playlistLayout->setSpacing(8);
    playlistLayout->addWidget(listTitle);
    playlistLayout->addWidget(recordingStatusLabel);
    playlistLayout->addWidget(videoList, 1);
    playlistLayout->addWidget(refreshButton);

    positionSlider = new QSlider(Qt::Horizontal, this);
    positionSlider->setMaximumHeight(15);
    positionSlider->setObjectName(QStringLiteral("durationSlider"));

    volumeSlider = new QSlider(Qt::Horizontal, this);
    volumeSlider->setRange(0, 100);
    volumeSlider->setMaximumWidth(80);
    volumeSlider->setObjectName(QStringLiteral("volumeSlider"));
    volumeSlider->setValue(50);

    playPauseButton = new QPushButton(QStringLiteral("播放/暂停"), this);
    nextButton = new QPushButton(QStringLiteral("下一段"), this);
    volumeDownButton = new QPushButton(QStringLiteral("音量 -"), this);
    volumeUpButton = new QPushButton(QStringLiteral("音量 +"), this);
    playlistToggleButton = new QPushButton(QStringLiteral("录像列表"), this);
    playPauseButton->setCheckable(true);
    playlistToggleButton->setCheckable(true);

    QPalette timePalette;
    timePalette.setColor(QPalette::WindowText, Qt::white);
    QFont timeFont;
    timeFont.setPixelSize(18);

    currentTimeLabel = new QLabel(QStringLiteral("00:00"), this);
    totalTimeLabel = new QLabel(QStringLiteral("/00:00"), this);
    currentTimeLabel->setFont(timeFont);
    totalTimeLabel->setFont(timeFont);
    currentTimeLabel->setPalette(timePalette);
    totalTimeLabel->setPalette(timePalette);

    QWidget *progressContainer = new QWidget(this);
    QHBoxLayout *progressLayout = new QHBoxLayout(progressContainer);
    progressLayout->addWidget(positionSlider);
    progressLayout->setContentsMargins(0, 0, 0, 0);

    QWidget *controlBar = new QWidget(this);
    controlBar->setMinimumHeight(48);
    QHBoxLayout *controlLayout = new QHBoxLayout(controlBar);
    controlLayout->addWidget(playPauseButton);
    controlLayout->addWidget(nextButton);
    controlLayout->addWidget(volumeDownButton);
    controlLayout->addWidget(volumeSlider);
    controlLayout->addWidget(volumeUpButton);
    controlLayout->addWidget(currentTimeLabel);
    controlLayout->addWidget(totalTimeLabel);
    controlLayout->addStretch();
    controlLayout->addWidget(playlistToggleButton);
    controlLayout->setContentsMargins(0, 0, 0, 0);

    QWidget *playerPanel = new QWidget(this);
    QVBoxLayout *playerLayout = new QVBoxLayout(playerPanel);
    playerLayout->setContentsMargins(0, 0, 0, 0);
    playerLayout->setSpacing(8);
    playerLayout->addWidget(videoStack, 1);
    playerLayout->addWidget(progressContainer);
    playerLayout->addWidget(controlBar);

    QHBoxLayout *contentLayout = new QHBoxLayout(this);
    contentLayout->setContentsMargins(12, 12, 12, 12);
    contentLayout->setSpacing(12);
    contentLayout->addWidget(playerPanel, 1);
    contentLayout->addWidget(playlistPanel);

    connect(playPauseButton, &QPushButton::clicked,
            this, &VideoPlayer::togglePlayback);
    connect(nextButton, &QPushButton::clicked,
            this, &VideoPlayer::playNextVideo);
    connect(volumeDownButton, &QPushButton::clicked,
            this, &VideoPlayer::decreaseVolume);
    connect(volumeUpButton, &QPushButton::clicked,
            this, &VideoPlayer::increaseVolume);
    connect(playlistToggleButton, &QPushButton::clicked,
            this, &VideoPlayer::togglePlaylistPanel);
    connect(videoList, &QListWidget::itemClicked,
            this, &VideoPlayer::playSelectedVideo);
    connect(positionSlider, &QSlider::sliderReleased,
            this, &VideoPlayer::seekToSliderPosition);
    connect(volumeSlider, &QSlider::sliderReleased,
            this, &VideoPlayer::applySliderVolume);
    connect(refreshButton, &QPushButton::clicked,
            this, &VideoPlayer::refreshRecordings);
}

/*
 * 创建 Qt Multimedia 框架的播放器和播放列表。
 * QMediaPlayer 负责解码和播放，QVideoWidget 负责渲染。
 */
void VideoPlayer::initializeMediaPlayer()
{
    mediaPlayer = new QMediaPlayer(this);
    playlist = new QMediaPlaylist(this);
    mediaPlayer->setVideoOutput(videoOutput);
    mediaPlayer->setVolume(volumeSlider->value());

    connect(mediaPlayer, &QMediaPlayer::stateChanged,
            this, &VideoPlayer::handlePlayerStateChanged);
    connect(mediaPlayer, &QMediaPlayer::durationChanged,
            this, &VideoPlayer::handleDurationChanged);
    connect(mediaPlayer, &QMediaPlayer::positionChanged,
            this, &VideoPlayer::handlePositionChanged);
}

/*
 * 窗口大小变化时，MJPEG 模式下重新缩放当前帧以适应新尺寸。
 */
void VideoPlayer::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (rawMjpegMode && !mjpegFrames.isEmpty())
        updateMjpegFrame();
}

/*
 * 播放/暂停切换。
 * 标准容器格式：直接调用 QMediaPlayer 的 play/pause
 * MJPEG 模式：启停 mjpegPlaybackTimer 定时器
 */
void VideoPlayer::togglePlayback()
{
    if (rawMjpegMode)
    {
        if (mjpegFrames.isEmpty())
            return;
        if (mjpegPlaybackTimer->isActive())
            mjpegPlaybackTimer->stop();
        else
            mjpegPlaybackTimer->start();
        playPauseButton->setChecked(mjpegPlaybackTimer->isActive());
        return;
    }
    if (mediaPlayer->state() == QMediaPlayer::PlayingState)
        mediaPlayer->pause();
    else
        mediaPlayer->play();
}

/* 循环播放下一段录像 */
void VideoPlayer::playNextVideo()
{
    if (recordingPaths.isEmpty())
        return;
    const int nextIndex = currentRecordingIndex < 0
                              ? 0
                              : (currentRecordingIndex + 1) % recordingPaths.size();
    playRecordingAtRow(nextIndex);
}

void VideoPlayer::increaseVolume()
{
    volumeSlider->setValue(volumeSlider->value() + 5);
    applySliderVolume();
}

void VideoPlayer::decreaseVolume()
{
    volumeSlider->setValue(volumeSlider->value() - 5);
    applySliderVolume();
}

void VideoPlayer::togglePlaylistPanel()
{
    playlistPanel->setVisible(!playlistToggleButton->isChecked());
}

void VideoPlayer::handlePlayerStateChanged(QMediaPlayer::State state)
{
    if (!rawMjpegMode)
        playPauseButton->setChecked(state == QMediaPlayer::PlayingState);
}

void VideoPlayer::playSelectedVideo(QListWidgetItem *item)
{
    if (!item)
        return;

    playRecordingAtRow(videoList->row(item));
}

void VideoPlayer::handlePlaylistIndexChanged(int index)
{
    if (index >= 0)
        videoList->setCurrentRow(index);
}

void VideoPlayer::handleDurationChanged(qint64 durationMilliseconds)
{
    if (rawMjpegMode)
        return;
    positionSlider->setRange(0, static_cast<int>(durationMilliseconds / 1000));
    totalTimeLabel->setText(QStringLiteral("/") +
                            formatDuration(durationMilliseconds));
}

void VideoPlayer::handlePositionChanged(qint64 positionMilliseconds)
{
    if (rawMjpegMode)
        return;
    if (!positionSlider->isSliderDown())
        positionSlider->setValue(static_cast<int>(positionMilliseconds / 1000));
    currentTimeLabel->setText(formatDuration(positionMilliseconds));
}

void VideoPlayer::seekToSliderPosition()
{
    if (rawMjpegMode)
    {
        mjpegFrameIndex = qBound(0, positionSlider->value(),
                                 qMax(0, mjpegFrames.size() - 1));
        updateMjpegFrame();
        return;
    }
    mediaPlayer->setPosition(static_cast<qint64>(positionSlider->value()) * 1000);
}

void VideoPlayer::applySliderVolume()
{
    mediaPlayer->setVolume(volumeSlider->value());
}

void VideoPlayer::refreshRecordings()
{
    scanVideoFiles(true);
}

/*
 * 扫描 data/recordings/ 目录，发现所有支持的录像文件。
 *
 * 支持的格式：
 *   标准容器：mp4, mkv, wmv, avi
 *   原始 MJPEG：.mjpeg, .mjpg
 *
 * 自动选择最新录像：
 *   当 selectLatest 为 true 时，自动播放最新文件。
 *   通过 QSettings 中的 latestRecordingPath 检测新完成的录像。
 *
 * 防重复机制：
 *   loadedLatestRecordingPath 记录上次自动加载的路径，
 *   避免同一文件被反复自动加载。
 */
void VideoPlayer::scanVideoFiles(bool selectLatest)
{
    const QString directoryPath = QDir(QCoreApplication::applicationDirPath())
                                      .filePath(QStringLiteral("data/recordings"));
    QDir().mkpath(directoryPath);
    const QDir videoDirectory(directoryPath);

    const QStringList filters = {
        QStringLiteral("*.mp4"), QStringLiteral("*.mkv"),
        QStringLiteral("*.wmv"), QStringLiteral("*.avi"),
        QStringLiteral("*.mjpeg"), QStringLiteral("*.mjpg")};
    const QFileInfoList files = videoDirectory.entryInfoList(
        filters, QDir::Files, QDir::Time);

    QStringList discoveredPaths;
    for (const QFileInfo &fileInfo : files)
        discoveredPaths.append(fileInfo.absoluteFilePath());

    const QString currentPath = currentRecordingIndex >= 0 &&
                                        currentRecordingIndex < recordingPaths.size()
                                    ? recordingPaths.at(currentRecordingIndex)
                                    : QString();
    if (discoveredPaths != recordingPaths)
    {
        recordingPaths = discoveredPaths;
        videoList->clear();
        for (const QString &path : recordingPaths)
        {
            const QFileInfo info(path);
            videoList->addItem(QStringLiteral("%1\n%2 · %3 KB")
                                   .arg(info.fileName(),
                                        info.lastModified().toString(QStringLiteral("MM-dd HH:mm:ss")))
                                   .arg(qMax<qint64>(1, info.size() / 1024)));
        }
        currentRecordingIndex = recordingPaths.indexOf(currentPath);
        if (currentRecordingIndex >= 0)
            videoList->setCurrentRow(currentRecordingIndex);
    }

    recordingStatusLabel->setText(recordingPaths.isEmpty()
                                      ? QStringLiteral("暂无事件录像。触发画面变化事件后将自动出现。")
                                      : QStringLiteral("已发现 %1 段录像，最新录像排在最前。")
                                            .arg(recordingPaths.size()));

    QSettings settings;
    const QString latestPath = QFileInfo(settings.value(
                                                     QStringLiteral("edge/latestRecordingPath"))
                                             .toString())
                                   .absoluteFilePath();
    const int latestIndex = recordingPaths.indexOf(latestPath);
    const bool newCompletedRecording = latestIndex >= 0 &&
                                       latestPath != loadedLatestRecordingPath;
    if (newCompletedRecording ||
        (selectLatest && !recordingPaths.isEmpty() &&
         currentRecordingIndex < 0))
    {
        const int row = newCompletedRecording ? latestIndex : 0;
        loadedLatestRecordingPath = recordingPaths.at(row);
        playRecordingAtRow(row);
    }
}

/*
 * 播放指定行的录像文件。
 *
 * 路由逻辑：
 *   如果文件后缀是 .mjpeg 或 .mjpg → 进入 MJPEG 模式（自定义解析播放）
 *   其他格式 → 使用 QMediaPlayer 播放
 *
 * MJPEG 模式下：
 *   videoStack 切换到 mjpegOutput（QLabel），mjpegPlaybackTimer 启动。
 * 标准模式下：
 *   videoStack 切换到 videoOutput（QVideoWidget），QMediaPlayer 接管。
 */
void VideoPlayer::playRecordingAtRow(int row)
{
    if (row < 0 || row >= recordingPaths.size())
        return;

    stopMjpegPlayback();
    mediaPlayer->stop();
    currentRecordingIndex = row;
    videoList->setCurrentRow(row);
    const QString filePath = recordingPaths.at(row);
    const QString suffix = QFileInfo(filePath).suffix().toLower();
    if (suffix == QLatin1String("mjpeg") || suffix == QLatin1String("mjpg"))
    {
        if (!loadMjpegRecording(filePath))
        {
            recordingStatusLabel->setText(QStringLiteral("录像读取失败：%1")
                                              .arg(QFileInfo(filePath).fileName()));
            return;
        }
        rawMjpegMode = true;
        videoStack->setCurrentWidget(mjpegOutput);
        mjpegPlaybackTimer->start();
        playPauseButton->setChecked(true);
    }
    else
    {
        rawMjpegMode = false;
        videoStack->setCurrentWidget(videoOutput);
        mediaPlayer->setMedia(QMediaContent(QUrl::fromLocalFile(filePath)));
        mediaPlayer->play();
    }
    recordingStatusLabel->setText(QStringLiteral("正在播放：%1")
                                      .arg(QFileInfo(filePath).fileName()));
}

/*
 * 加载 MJPEG 录像文件并提取所有 JPEG 帧。
 *
 * MJPEG 文件格式：
 *   由 TcpFrameSender 录制的 .mjpeg 文件本质上是连续的 JPEG 图像流。
 *   每帧以 FF D8（SOI 标记）开头，FF D9（EOI 标记）结尾。
 *
 * 解析流程：
 *   1. 读取整个文件到内存（最大 128MB）
 *   2. 从文件头开始搜索 FF D8
 *   3. 找到 FF D8 后，搜索下一个 FF D9
 *   4. 将 FF D8 ... FF D9 之间的数据作为一个完整 JPEG 帧存入列表
 *   5. 从 FF D9 之后继续搜索下一帧
 *
 * 性能优化：
 *   一次性解析所有帧存入内存（mjpegFrames），播放时只需 QImage::fromData()
 *   解码当前帧，避免在播放过程中反复解析文件。
 */
bool VideoPlayer::loadMjpegRecording(const QString &filePath)
{
    QFile file(filePath);
    const qint64 maximumRecordingBytes = 128LL * 1024 * 1024;
    if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 ||
        file.size() > maximumRecordingBytes)
    {
        qWarning() << "MJPEG录像无法打开或文件过大：" << filePath;
        return false;
    }
    const QByteArray data = file.readAll();
    const QByteArray startMarker = QByteArray::fromHex("ffd8");
    const QByteArray endMarker = QByteArray::fromHex("ffd9");
    mjpegFrames.clear();
    int searchPosition = 0;
    while (searchPosition < data.size())
    {
        const int start = data.indexOf(startMarker, searchPosition);
        if (start < 0)
            break;
        const int end = data.indexOf(endMarker, start + 2);
        if (end < 0)
            break;
        mjpegFrames.append(data.mid(start, end - start + 2));
        searchPosition = end + 2;
    }
    if (mjpegFrames.isEmpty())
    {
        qWarning() << "MJPEG录像中没有有效JPEG帧：" << filePath;
        return false;
    }

    mjpegFrameIndex = 0;
    positionSlider->setRange(0, mjpegFrames.size() - 1);
    totalTimeLabel->setText(QStringLiteral("/") +
                            formatDuration(qint64(mjpegFrames.size()) * 1000 / 15));
    updateMjpegFrame();
    qInfo() << "事件录像已载入：" << filePath
            << "帧数=" << mjpegFrames.size();
    return true;
}

void VideoPlayer::stopMjpegPlayback()
{
    mjpegPlaybackTimer->stop();
    mjpegFrames.clear();
    mjpegFrameIndex = 0;
    rawMjpegMode = false;
}

void VideoPlayer::showNextMjpegFrame()
{
    if (!rawMjpegMode || mjpegFrames.isEmpty())
    {
        mjpegPlaybackTimer->stop();
        return;
    }
    if (mjpegFrameIndex >= mjpegFrames.size())
        mjpegFrameIndex = 0;
    updateMjpegFrame();
    ++mjpegFrameIndex;
}

void VideoPlayer::updateMjpegFrame()
{
    if (mjpegFrames.isEmpty())
        return;
    const int frameIndex = qBound(0, mjpegFrameIndex,
                                  mjpegFrames.size() - 1);
    const QImage image = QImage::fromData(mjpegFrames.at(frameIndex), "JPEG");
    if (image.isNull())
        return;
    mjpegOutput->setPixmap(QPixmap::fromImage(image).scaled(
        mjpegOutput->size(), Qt::KeepAspectRatio, Qt::FastTransformation));
    if (!positionSlider->isSliderDown())
        positionSlider->setValue(frameIndex);
    currentTimeLabel->setText(formatDuration(qint64(frameIndex) * 1000 / 15));
}

QString VideoPlayer::formatDuration(qint64 milliseconds)
{
    const qint64 totalSeconds = qMax<qint64>(0, milliseconds / 1000);
    const qint64 minutes = totalSeconds / 60;
    const qint64 seconds = totalSeconds % 60;
    return QStringLiteral("%1:%2")
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(seconds, 2, 10, QLatin1Char('0'));
}
