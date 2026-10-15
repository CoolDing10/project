// 防止头文件被重复包含的头文件保护宏
#ifndef TCP_FRAME_SENDER_H
#define TCP_FRAME_SENDER_H

// 引入 C++ 原子操作库，用于多线程安全的计数器
#include <atomic>
// 引入 Qt 的 IP 地址封装类
#include <QHostAddress>
// 引入 Qt 的图像类，用于接收和处理帧图像
#include <QImage>
// 引入 Qt 对象基类，提供信号槽等核心功能
#include <QObject>

// 前向声明：Qt 文件操作类，用于录像文件的写入
class QFile;
// 前向声明：Qt TCP 套接字类，用于建立 TCP 网络连接
class QTcpSocket;
// 前向声明：Qt 定时器类，用于 TCP 重连和录像定时停止
class QTimer;
// 前向声明：Qt UDP 套接字类，用于 UDP 数据报发送
class QUdpSocket;
// 前向声明：FFmpeg 编码器上下文，管理 H.264 编码参数和状态
struct AVCodecContext;
// 前向声明：FFmpeg 视频帧，存储原始 YUV 帧数据
struct AVFrame;
// 前向声明：FFmpeg 编码数据包，存储编码后的 H.264 数据
struct AVPacket;
// 前向声明：FFmpeg 像素格式转换上下文，用于 RGB → YUV420P 转换
struct SwsContext;

// 帧输出统计数据结构体，用于向外暴露编码和发送的统计数据
struct FrameOutputStatistics
{
    // 接收到的帧总数（从摄像头或其他源送入的原始帧数）
    quint64 receivedFrames = 0;
    // 成功编码的帧总数（经过 H.264 编码器成功编码的帧数）
    quint64 encodedFrames = 0;
    // 成功发送的帧总数（通过网络成功发出的编码帧数）
    quint64 sentFrames = 0;
    // 丢弃的帧总数（编码失败或发送失败导致跳过的帧数）
    quint64 droppedFrames = 0;
    // 已发送的总字节数（通过网络发出的所有数据字节累计）
    quint64 sentBytes = 0;
    // 编码总耗时（微秒），所有帧编码耗时的累计值
    quint64 totalEncodeMicroseconds = 0;
    // 最近一次编码的耗时（微秒），用于实时监控编码性能
    quint64 lastEncodeMicroseconds = 0;
};

// 帧发送器类：名称为兼容原有 UI 保留；支持 H.264/TCP 和 H.264/RTP/UDP 两种链路。
// 负责从 QImage 到 H.264 编码再到网络发送的完整流水线
class TcpFrameSender : public QObject
{
    // Qt 元对象系统宏，使该类支持信号与槽机制
    Q_OBJECT
public:
    // 构造函数
    // @param parent 父 QObject 指针，用于 Qt 对象树内存管理，默认为 nullptr
    explicit TcpFrameSender(QObject *parent = nullptr);
    // 析构函数，负责释放编码器资源和取消网络连接
    ~TcpFrameSender() override;
    // 获取当前的帧统计数据快照
    // @return FrameOutputStatistics 包含所有计数字段最新值的结构体
    FrameOutputStatistics statistics() const;

public slots:
    // 初始化槽函数：创建 UDP/TCP 套接字、重连定时器等资源（仅首次调用有效）
    void initialize();
    // 配置推流参数槽函数
    // @param host 目标服务器 IP 地址或主机名
    // @param port 目标服务器端口号
    // @param enabled 是否启用推流
    // @param transport 传输协议，"tcp" 使用 TCP，"udp" 使用 RTP/UDP
    void configure(const QString &host, quint16 port, bool enabled,
                   const QString &transport);
    // 发送一帧图像槽函数：核心处理流水线入口
    // @param image 待发送的原始 QImage 图像（通常来自 V4L2 摄像头采集）
    void sendFrame(const QImage &image);
    // 开始录像槽函数
    // @param filePath 录像文件的完整保存路径
    // @param durationMs 录像时长（毫秒），<= 0 表示手动停止
    void startRecording(const QString &filePath, int durationMs);
    // 停止录像槽函数
    void stopRecording();
    // 停止所有操作槽函数：关闭推流、停止录像、释放编码器、断开连接
    void stop();

signals:
    // 帧处理完成信号：通知调用方当前帧已处理完毕，可送入下一帧
    void frameHandled();
    // 状态变更信号：用于 UI 显示当前连接/编码状态信息
    // @param status 状态描述字符串
    void statusChanged(const QString &status);
    // 录像状态变更信号
    // @param active true 表示录像开始，false 表示录像停止
    // @param filePath 当前录像文件路径
    void recordingStateChanged(bool active, const QString &filePath);
    // 录像错误信号
    // @param message 错误描述信息
    void recordingError(const QString &message);

private:
    // 尝试建立 TCP 连接（仅在未连接状态下执行）
    void tryTcpConnect();
    // 通过 TCP 协议发送一帧 H.264 数据
    // @param accessUnit H.264 访问单元（编码后的帧数据）
    // @param width 编码帧宽度
    // @param height 编码帧高度
    // @param timestampMs 时间戳（毫秒，用于接收端同步）
    // @return true 发送成功，false 发送失败
    bool sendTcpFrame(const QByteArray &accessUnit, int width, int height,
                      qint64 timestampMs);
    // 初始化 H.264 硬件/软件编码器
    // @param width 编码目标宽度
    // @param height 编码目标高度
    // @param error 输出参数，失败时填入错误描述
    // @return true 初始化成功，false 失败
    bool initializeEncoder(int width, int height, QString *error);
    // 释放编码器及其关联的所有 FFmpeg 资源
    void releaseEncoder();
    // 将 QImage 编码为 H.264 访问单元
    // @param image 输入原始图像
    // @param accessUnit 输出参数，编码后的 H.264 数据
    // @param keyFrame 输出参数，指示当前帧是否为关键帧（IDR 帧）
    // @param error 输出参数，失败时填入错误描述
    // @return true 编码成功，false 失败
    bool encodeFrame(const QImage &image, QByteArray *accessUnit,
                     bool *keyFrame, QString *error);
    // 通过 RTP/UDP 协议发送 H.264 访问单元（分包发送）
    // @param accessUnit H.264 编码数据
    // @param keyFrame 是否为关键帧
    // @param timestampUs 时间戳（微秒）
    // @return true 发送成功，false 发送失败
    bool sendAccessUnit(const QByteArray &accessUnit, bool keyFrame,
                        quint64 timestampUs);
    // 将一帧图像以 JPEG 格式写入录像文件
    // @param image 待写入的原始图像
    // @return true 写入成功，false 写入失败
    bool writeRecordingFrame(const QImage &image);

    // --- 网络相关成员 ---
    // UDP 套接字，用于 RTP/UDP 推流模式
    QUdpSocket *udpSocket;
    // TCP 套接字，用于 TCP 推流模式
    QTcpSocket *tcpSocket;
    // TCP 重连定时器，断线后每隔 1 秒自动重连
    QTimer *tcpReconnectTimer;
    // 录像停止定时器，达到指定时长后自动停止录像
    QTimer *recordingStopTimer;
    // 录像文件对象，用于 MJPEG 格式录像的写入
    QFile *recordingFile;

    // --- 服务器连接参数 ---
    // 目标服务器 IP 地址（解析后）
    QHostAddress serverAddress;
    // 目标服务器主机名/IP 字符串（原始输入）
    QString serverHost;
    // 目标服务器端口号
    quint16 serverPort;

    // --- 流和帧序列号 ---
    // 流 ID：随机生成，用于 RTP 接收端区分不同的推流会话
    quint32 streamId;
    // 帧序列号：每发送一帧递增，用于接收端检测丢帧
    quint32 frameSequence;
    // 包序列号：每发送一个 RTP 分包递增，用于 UDP 接收端重组
    quint32 packetSequence;

    // --- 编码器相关 ---
    // 编码器 PTS（显示时间戳）：每编码一帧递增，用于 H.264 编码器内部时间管理
    qint64 encoderPts;

    // --- 状态标志 ---
    // 推流是否启用
    bool sendingEnabled;
    // 是否使用 TCP 传输模式（false 表示 UDP/RTP 模式）
    bool tcpTransport;
    // 是否处于手动录像模式（true 表示由用户手动停止）
    bool manualRecordingActive;
    // 当前活跃的录像文件路径
    QString activeRecordingPath;

    // --- FFmpeg 编码器上下文指针 ---
    // H.264 编码器上下文，存储编码参数和编码器状态
    AVCodecContext *codecContext;
    // 编码器输入帧，存储待编码的 YUV420P 原始帧数据
    AVFrame *codecFrame;
    // 编码器输出包，存储编码后的 H.264 压缩数据
    AVPacket *codecPacket;
    // 像素格式转换上下文，用于 RGB → YUV420P 的色彩空间和分辨率转换
    SwsContext *scaleContext;

    // --- 线程安全的原子计数器 ---
    // 接收帧计数器（原子操作，保证多线程读写安全）
    std::atomic<quint64> receivedFrameCount;
    // 编码帧计数器
    std::atomic<quint64> encodedFrameCount;
    // 发送帧计数器
    std::atomic<quint64> sentFrameCount;
    // 丢弃帧计数器
    std::atomic<quint64> droppedFrameCount;
    // 发送字节计数器
    std::atomic<quint64> sentByteCount;
    // 编码总耗时（微秒）
    std::atomic<quint64> totalEncodeMicroseconds;
    // 最近一次编码耗时（微秒）
    std::atomic<quint64> lastEncodeMicroseconds;
};

// 头文件保护宏结束
#endif