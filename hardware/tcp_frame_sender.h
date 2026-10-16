// 防止头文件被重复包含的头文件保护宏
#ifndef TCP_FRAME_SENDER_H // 仅在尚未包含本头文件时编译下面声明。
#define TCP_FRAME_SENDER_H // 定义保护宏，防止类与信号槽重复声明。

// 引入 Qt 的 IP 地址封装类
#include <QHostAddress> // 保存并校验 UDP 模式的目标 IP 地址。
// 引入 Qt 的图像类，用于接收和处理帧图像
#include <QImage> // sendFrame() 接收的原始摄像头帧类型。
// 引入 Qt 对象基类，提供信号槽等核心功能
#include <QObject> // TcpFrameSender 的基类，提供对象树、信号槽和线程归属。

// 前向声明：Qt 文件操作类，用于录像文件的写入
class QFile; // 只使用指针，因此用前向声明避免在头文件包含 QFile。
// 前向声明：Qt TCP 套接字类，用于建立 TCP 网络连接
class QTcpSocket; // 保存 TCP 长连接并发送自定义帧协议。
// 前向声明：Qt 定时器类，用于 TCP 重连和录像定时停止
class QTimer; // 用于 TCP 重连和录像自动停止。
// 前向声明：Qt UDP 套接字类，用于 UDP 数据报发送
class QUdpSocket; // 发送 RTP 风格的 UDP 分片数据报。
// 前向声明：FFmpeg 编码器上下文，管理 H.264 编码参数和状态
struct AVCodecContext; // 前向声明，具体 FFmpeg 定义只在 .cpp 中引入。
// 前向声明：FFmpeg 视频帧，存储原始 YUV 帧数据
struct AVFrame; // 前向声明编码器输入帧结构。
// 前向声明：FFmpeg 编码数据包，存储编码后的 H.264 数据
struct AVPacket; // 前向声明编码器输出包结构。
// 前向声明：FFmpeg 像素格式转换上下文，用于 RGB → YUV420P 转换
struct SwsContext; // 前向声明 libswscale 转换上下文。

// 帧发送器类：名称为兼容原有 UI 保留；支持 H.264/TCP 和 H.264/RTP/UDP 两种链路。
// 负责从 QImage 到 H.264 编码再到网络发送的完整流水线
class TcpFrameSender : public QObject // 在网络工作线程中完成编码、推流和录像。
{
    // Qt 元对象系统宏，使该类支持信号与槽机制
    Q_OBJECT // 启用 Qt 元对象系统，使槽可以接收跨线程队列调用。
public: // 对象创建和销毁接口。
    // 构造函数
    // @param parent 父 QObject 指针，用于 Qt 对象树内存管理，默认为 nullptr
    explicit TcpFrameSender(QObject *parent = nullptr); // 初始化成员状态；parent 负责可选的 Qt 对象树生命周期。
    // 析构函数，负责释放编码器资源和取消网络连接
    ~TcpFrameSender() override; // 释放 FFmpeg 资源；Qt 子对象由 QObject 自动释放。
public slots: // 由主线程或采集线程通过信号调用的工作线程入口。
    // 初始化槽函数：创建 UDP/TCP 套接字、重连定时器等资源（仅首次调用有效）
    void initialize(); // 在网络线程中一次性创建 socket、定时器和 QFile。
    // 配置推流参数槽函数
    // @param host 目标服务器 IP 地址或主机名
    // @param port 目标服务器端口号
    // @param enabled 是否启用推流
    // @param transport 传输协议，"tcp" 使用 TCP，"udp" 使用 RTP/UDP
    void configure(const QString &host, quint16 port, bool enabled, // host/port 是远端；enabled 是推流开关。
                   const QString &transport); // transport 选择 "tcp" 或 "udp"。
    // 发送一帧图像槽函数：核心处理流水线入口
    // @param image 待发送的原始 QImage 图像（通常来自 V4L2 摄像头采集）
    void sendFrame(const QImage &image); // image 是采集线程投递的独立原始图像帧。
    // 开始录像槽函数
    // @param filePath 录像文件的完整保存路径
    // @param durationMs 自动事件录像时长（毫秒），必须大于 0
    void startRecording(const QString &filePath, int durationMs); // filePath 为输出路径，durationMs 为自动停止毫秒数。
    // 停止录像槽函数
    void stopRecording(); // 刷新并关闭当前录像文件，发送结束信号。
    // 停止所有操作槽函数：关闭推流、停止录像、释放编码器、断开连接
    void stop(); // 停止推流、重连、录像和编码器，关闭两个 socket。

signals: // 跨线程反馈推流、录像状态和错误给主线程 UI。
    // 状态变更信号：用于 UI 显示当前连接/编码状态信息
    // @param status 状态描述字符串
    void statusChanged(const QString &status); // status 是直接显示给 UI 的连接或编码状态文本。
    // 录像状态变更信号
    // @param active true 表示录像开始，false 表示录像停止
    // @param filePath 当前录像文件路径
    void recordingStateChanged(bool active, const QString &filePath); // active 表示启停，filePath 是对应录像路径。
    // 录像错误信号
    // @param message 错误描述信息
    void recordingError(const QString &message); // message 是录像打开、编码或写入错误说明。

private: // 编码、发送、写盘辅助函数以及持久状态。
    // 尝试建立 TCP 连接（仅在未连接状态下执行）
    void tryTcpConnect(); // 仅在未连接且仍启用 TCP 推流时调用 connectToHost()。
    // 通过 TCP 协议发送一帧 H.264 数据
    // @param accessUnit H.264 访问单元（编码后的帧数据）
    // @param width 编码帧宽度
    // @param height 编码帧高度
    // @param timestampMs 时间戳（毫秒，用于接收端同步）
    // @return true 发送成功，false 发送失败
    bool sendTcpFrame(const QByteArray &accessUnit, int width, int height, // accessUnit 为编码帧，width/height 为编码尺寸。
                      qint64 timestampMs); // timestampMs 为写入协议头的 Unix 毫秒时间戳。
    // 初始化 H.264 硬件/软件编码器
    // @param width 编码目标宽度
    // @param height 编码目标高度
    // @param error 输出参数，失败时填入错误描述
    // @return true 初始化成功，false 失败
    bool initializeEncoder(int width, int height, QString *error); // 创建指定尺寸编码器；error 接收失败原因。
    // 释放编码器及其关联的所有 FFmpeg 资源
    void releaseEncoder(); // 释放 swscale、AVPacket、AVFrame 和 AVCodecContext。
    // 将 QImage 编码为 H.264 访问单元
    // @param image 输入原始图像
    // @param accessUnit 输出参数，编码后的 H.264 数据
    // @param keyFrame 输出参数，指示当前帧是否为关键帧（IDR 帧）
    // @param error 输出参数，失败时填入错误描述
    // @return true 编码成功，false 失败
    bool encodeFrame(const QImage &image, QByteArray *accessUnit, // image 为输入；accessUnit 接收 H.264 字节。
                     bool *keyFrame, QString *error); // keyFrame 接收关键帧标记；error 接收错误文本。
    // 通过 RTP/UDP 协议发送 H.264 访问单元（分包发送）
    // @param accessUnit H.264 编码数据
    // @param keyFrame 是否为关键帧
    // @param timestampUs 时间戳（微秒）
    // @return true 发送成功，false 发送失败
    bool sendAccessUnit(const QByteArray &accessUnit, bool keyFrame, // accessUnit 是一帧，keyFrame 是关键帧标志。
                        quint64 timestampUs); // timestampUs 是所有 UDP 分片共用的微秒时间戳。
    // 将一帧已经编码好的 H.264 访问单元写入录像文件
    // @param accessUnit 一帧 H.264 Annex B 数据
    // @return true 写入成功，false 写入失败
    bool writeRecordingFrame(const QByteArray &accessUnit); // 将一帧编码结果原样追加到活动录像文件。

    // --- 网络相关成员 ---
    // UDP 套接字，用于 RTP/UDP 推流模式
    QUdpSocket *udpSocket; // 非拥有语义说明：实际由 QObject 父对象 this 自动释放。
    // TCP 套接字，用于 TCP 推流模式
    QTcpSocket *tcpSocket; // 维护 TCP 连接和 Qt 异步发送缓冲区。
    // TCP 重连定时器，断线后每隔 1 秒自动重连
    QTimer *tcpReconnectTimer; // 每秒触发 tryTcpConnect() 的周期定时器。
    // 录像停止定时器，达到指定时长后自动停止录像
    QTimer *recordingStopTimer; // 单次定时器，到达 durationMs 后停止录像。
    // 录像文件对象，用于 H.264 Annex B 裸码流录像的写入
    QFile *recordingFile; // 写入当前 H.264 Annex B 裸码流文件。

    // --- 服务器连接参数 ---
    // 目标服务器 IP 地址（解析后）
    QHostAddress serverAddress; // UDP writeDatagram() 使用的已解析目标 IP。
    // 目标服务器主机名/IP 字符串（原始输入）
    QString serverHost; // TCP connectToHost() 使用的主机名或 IP 文本。
    // 目标服务器端口号
    quint16 serverPort; // TCP 和 UDP 共用的 1～65535 目标端口。

    // --- 流和帧序列号 ---
    // 流 ID：随机生成，用于 RTP 接收端区分不同的推流会话
    quint32 streamId; // 每次 UDP 推流配置随机生成的 32 位会话 ID。
    // 帧序列号：每发送一帧递增，用于接收端检测丢帧
    quint32 frameSequence; // 每成功发送一帧后递增的帧级序号。
    // 包序列号：每发送一个 RTP 分包递增，用于 UDP 接收端重组
    quint32 packetSequence; // 每发送一个 UDP 分片后递增的包级序号。

    // --- 编码器相关 ---
    // 编码器 PTS（显示时间戳）：每编码一帧递增，用于 H.264 编码器内部时间管理
    qint64 encoderPts; // 按 1/30 秒 time_base 逐帧递增的 FFmpeg PTS。

    // --- 状态标志 ---
    // 推流是否启用
    bool sendingEnabled; // true 表示目标参数有效且用户要求实时推流。
    // 是否使用 TCP 传输模式（false 表示 UDP/RTP 模式）
    bool tcpTransport; // true 选择自定义 TCP 帧协议，false 选择 RTP/UDP 分片。
    // 录像开始后强制下一编码帧成为 IDR，保证录像可独立解码
    bool forceNextKeyFrame; // true 要求下一编码帧成为录像起始 I/IDR 帧。
    // 当前活跃的录像文件路径
    QString activeRecordingPath; // 当前打开的录像路径，无录像时为空。

    // --- FFmpeg 编码器上下文指针 ---
    // H.264 编码器上下文，存储编码参数和编码器状态
    AVCodecContext *codecContext; // 保存 H.264 编码参数、参考状态和内部资源。
    // 编码器输入帧，存储待编码的 YUV420P 原始帧数据
    AVFrame *codecFrame; // 保存每次 sws_scale 生成的 YUV420P 输入帧。
    // 编码器输出包，存储编码后的 H.264 压缩数据
    AVPacket *codecPacket; // 复用来接收编码器输出的压缩包。
    // 像素格式转换上下文，用于 RGB → YUV420P 的色彩空间和分辨率转换
    SwsContext *scaleContext; // 缓存 RGB 到 YUV420P 的缩放与色彩转换配置。
};

// 头文件保护宏结束
#endif // TCP_FRAME_SENDER_H：结束头文件保护。
