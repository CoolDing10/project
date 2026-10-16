// 包含本类的头文件声明
#include "tcp_frame_sender.h"
#include "video_pipeline_config.h"
// 引入 RTP 打包协议定义（分包头结构体、分包工具函数）
#include "rtp_packet.h"

// Qt 数据流类，用于二进制序列化 TCP 帧头
#include <QDataStream>
// Qt 日期时间类，用于获取当前时间的毫秒级时间戳
#include <QDateTime>
// Qt 目录操作类，用于创建录像文件所需的父目录
#include <QDir>
// Qt 文件操作类，用于录像文件的打开、写入和关闭
#include <QFile>
// Qt 文件信息类，用于提取文件路径中的目录部分
#include <QFileInfo>
// Qt 随机数生成器类，用于生成唯一的流 ID
#include <QRandomGenerator>
// Qt 尺寸类，用于图像的宽高缩放计算
#include <QSize>
// Qt TCP 套接字类，用于建立和维护 TCP 网络连接
#include <QTcpSocket>
// Qt 定时器类，用于 TCP 自动重连和录像定时停止
#include <QTimer>
// Qt UDP 套接字类，用于发送 UDP 数据报
#include <QUdpSocket>
// Qt 全局定义（如 Q_BYTE_ORDER 字节序宏）
#include <QtGlobal>

// FFmpeg C 库头文件，使用 extern "C" 包裹以避免 C++ 名称修饰问题
extern "C"
{
// FFmpeg 编解码核心 API：avcodec_find_encoder、avcodec_open2 等
#include <libavcodec/avcodec.h>
// FFmpeg 版本号宏定义
#include <libavcodec/version.h>
// FFmpeg 字典操作 API：av_dict_set、av_dict_free（用于设置编码器选项）
#include <libavutil/dict.h>
// FFmpeg 错误码定义和错误描述函数：AVERROR、av_strerror
#include <libavutil/error.h>
// FFmpeg 像素格式转换 API：sws_getCachedContext、sws_scale、sws_freeContext
#include <libswscale/swscale.h>
}

// 匿名命名空间：这些常量和工具函数仅在当前 .cpp 文件内可见
namespace
{
    // i.MX6ULL 只有一个 Cortex-A7 核，640x480 的 libx264 软件编码只能达到约
    // 3~4 FPS。推流编码限制到 QVGA，摄像头采集和本地预览仍保持 640x480。
    // 最大编码宽度：320 像素
    constexpr int MaximumEncodedWidth = 320;
    // 最大编码高度：240 像素
    constexpr int MaximumEncodedHeight = 240;
    // 目标比特率：350 kbps，在画质和带宽之间取平衡
    constexpr int TargetBitRate = 350000;
    // TCP 帧魔术字："IMXV"，用于接收端校验数据完整性（类似文件格式的文件签名）
    constexpr quint32 TcpFrameMagic = 0x494d5856; // "IMXV"
    // 版本 2 表示载荷为一帧 H.264 Access Unit；版本 1 曾用于 JPEG。
    // TCP 帧协议版本号
    constexpr quint16 TcpFrameVersion = 2;
    // TCP 帧头大小：28 字节（魔术字 4 + 版本 2 + 头大小 2 + 载荷大小 4 + 帧序号 4 + 时间戳 8 + 宽度 2 + 高度 2）
    constexpr quint16 TcpFrameHeaderSize = 28;
    // TCP 发送缓冲区最大待发送字节数：512 KB，超过此值则丢弃当前帧以避免内存堆积
    constexpr qint64 MaximumPendingTcpBytes = 512 * 1024;

    // 将 FFmpeg 错误码转换为可读的中文/英文错误字符串
    // @param code FFmpeg API 返回的负值错误码
    // @return 人类可读的错误描述字符串
    QString ffmpegError(int code)
    {
        // FFmpeg 错误描述缓冲区（AV_ERROR_MAX_STRING_SIZE 通常为 64 字节）
        char text[AV_ERROR_MAX_STRING_SIZE] = {0};
        // 调用 FFmpeg API 将错误码转为字符串描述
        av_strerror(code, text, sizeof(text));
        // 将 C 字符串转为 QString 返回
        return QString::fromLocal8Bit(text);
    }

    // 注册旧版 FFmpeg 内置编解码器（兼容 i.MX6ULL SDK 的 libavcodec 57）
    void registerLegacyCodecs()
    {
// 仅当 libavcodec 主版本号 < 58 时执行（58 及以上版本自动注册）
#if LIBAVCODEC_VERSION_MAJOR < 58
        // libavcodec 57（i.MX6ULL SDK）不会自动注册内置编码器。
        // 未调用此函数时，avcodec_find_encoder(AV_CODEC_ID_H264) 返回空指针，
        // 即使 ffmpeg -encoders 能看到 libx264。
        // 静态变量，确保注册只执行一次
        static bool registered = false;
        if (!registered)
        {
            // 注册所有内置编解码器
            avcodec_register_all();
            registered = true;
        }
#endif
    }

    // 根据最大编码分辨率约束计算实际的编码尺寸
    // @param image 输入原始图像
    // @return 缩放后的编码尺寸（不超过 MaximumEncodedWidth x MaximumEncodedHeight，保持宽高比）
    QSize encodedSizeFor(const QImage &image)
    {
        // 获取原始图像尺寸
        QSize size = image.size();
        // 如果任一维度超过最大编码分辨率限制，则等比例缩放
        if (size.width() > MaximumEncodedWidth ||
            size.height() > MaximumEncodedHeight)
            size.scale(MaximumEncodedWidth, MaximumEncodedHeight,
                       Qt::KeepAspectRatio);
        // YUV420P 的宽高必须为偶数（色度子采样的要求）。
        // 将宽度向下取到最近的偶数
        size.setWidth(qMax(2, size.width() & ~1));
        // 将高度向下取到最近的偶数
        size.setHeight(qMax(2, size.height() & ~1));
        return size;
    }
}

// 构造函数：初始化所有成员变量为默认值
// @param parent 父 QObject，用于 Qt 对象树管理
TcpFrameSender::TcpFrameSender(QObject *parent)
    : QObject(parent),                                  // 调用基类构造函数
      udpSocket(nullptr),                               // UDP 套接字初始为 nullptr（惰性初始化）
      tcpSocket(nullptr),                               // TCP 套接字初始为 nullptr
      tcpReconnectTimer(nullptr),                       // TCP 重连定时器初始为 nullptr
      recordingStopTimer(nullptr),                      // 录像停止定时器初始为 nullptr
      recordingFile(nullptr),                           // 录像文件对象初始为 nullptr
      serverPort(8888),                                 // 默认服务器端口号 8888
      streamId(QRandomGenerator::global()->generate()), // 生成随机流 ID
      frameSequence(0),                                 // 帧序列号从 0 开始
      packetSequence(0),                                // 包序列号从 0 开始
      encoderPts(0),                                    // 编码器 PTS 从 0 开始
      sendingEnabled(false),                            // 默认不启用推流
      tcpTransport(false),                              // 默认使用 UDP 传输模式
      forceNextKeyFrame(false),                         // 默认不强制关键帧
      codecContext(nullptr),                            // 编码器上下文初始为 nullptr（惰性初始化）
      codecFrame(nullptr),                              // 编码器输入帧初始为 nullptr
      codecPacket(nullptr),                             // 编码器输出包初始为 nullptr
      scaleContext(nullptr)                             // 像素转换上下文初始为 nullptr
{
}

// 析构函数：释放编码器资源（网络套接字由 Qt 对象树自动管理）
TcpFrameSender::~TcpFrameSender()
{
    // 释放所有 FFmpeg 编码器资源（编码器上下文、帧、包、像素转换器）
    releaseEncoder();
}

// 初始化网络套接字和定时器资源（仅首次调用有效，后续调用直接返回）
void TcpFrameSender::initialize()
{
    // 如果 UDP 套接字已创建，说明已初始化过，直接返回避免重复创建
    if (udpSocket)
        return;
    // 创建 UDP 套接字，父对象为 this（由 Qt 对象树管理生命周期）
    udpSocket = new QUdpSocket(this);
    // 设置 UDP 发送缓冲区大小为 256 KB
    udpSocket->setSocketOption(QAbstractSocket::SendBufferSizeSocketOption, 256 * 1024);
    // 创建 TCP 套接字
    tcpSocket = new QTcpSocket(this);
    // 启用 TCP_NODELAY 选项：禁用 Nagle 算法，减少延迟
    tcpSocket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    // 启用 TCP KeepAlive：定期发送保活探测包，检测断线
    tcpSocket->setSocketOption(QAbstractSocket::KeepAliveOption, 1);
    // 创建 TCP 重连定时器
    tcpReconnectTimer = new QTimer(this);
    // 设置重连间隔为 1000 毫秒（1 秒）
    tcpReconnectTimer->setInterval(1000);
    // 定时器超时 → 调用 tryTcpConnect() 尝试重连
    connect(tcpReconnectTimer, &QTimer::timeout, this,
            &TcpFrameSender::tryTcpConnect);
    // TCP 连接成功时的处理 lambda
    connect(tcpSocket, &QTcpSocket::connected, this, [this]
            {
        // 每次 TCP 重连都从新的 IDR + SPS/PPS 开始，避免接收端缺少参考帧。
        // 释放旧编码器，确保重连后产生新的关键帧和序列参数集
        releaseEncoder();
        // 重置编码器 PTS
        encoderPts = 0;
        // 重置帧序列号
        frameSequence = 0;
        // 发送连接成功状态信息
        emit statusChanged(QStringLiteral("H.264/TCP 已连接 %1:%2")
                               .arg(serverHost).arg(serverPort)); });
    // TCP 断开连接时的处理 lambda
    connect(tcpSocket, &QTcpSocket::disconnected, this, [this]
            {
        // 仅在推流启用且为 TCP 模式时才显示断开状态
        if (sendingEnabled && tcpTransport)
            emit statusChanged(QStringLiteral("H.264/TCP 已断开，正在重连 %1:%2")
                                   .arg(serverHost).arg(serverPort)); });
    // TCP 连接错误时的处理 lambda
    connect(tcpSocket,
            QOverload<QAbstractSocket::SocketError>::of(&QTcpSocket::error),
            this, [this](QAbstractSocket::SocketError)
            {
        // 仅在推流启用且为 TCP 模式时才显示错误状态
        if (sendingEnabled && tcpTransport)
            emit statusChanged(QStringLiteral("H.264/TCP 连接失败：%1（自动重连）")
                                   .arg(tcpSocket->errorString())); });
    // 创建录像停止定时器
    recordingStopTimer = new QTimer(this);
    // 设为单次触发：超时后只执行一次，不会重复
    recordingStopTimer->setSingleShot(true);
    // 定时器超时 → 调用 stopRecording() 自动停止录像
    connect(recordingStopTimer, &QTimer::timeout, this, &TcpFrameSender::stopRecording);
    // 创建录像文件对象
    recordingFile = new QFile(this);
}

// 配置推流目标和参数
// @param host 目标主机名或 IP 地址字符串
// @param port 目标端口号
// @param enabled 是否启用推流
// @param transport "tcp" 或 "udp" 传输协议选择
void TcpFrameSender::configure(const QString &host, quint16 port, bool enabled,
                               const QString &transport)
{
    // 如果尚未初始化，先执行初始化
    if (!udpSocket)
        initialize();
    // 判断是否使用 TCP 传输模式（不区分大小写比较）
    const bool useTcp = transport.compare(QStringLiteral("tcp"),
                                          Qt::CaseInsensitive) == 0;
    // IP 地址对象
    QHostAddress address;
    // 去除主机名/地址字符串两端的空白字符
    const QString trimmedHost = host.trimmed();
    // 验证地址有效性：TCP 模式只需非空，UDP 模式必须是有效 IP 地址
    const bool validAddress = useTcp ? !trimmedHost.isEmpty()
                                     : address.setAddress(trimmedHost);
    // 综合判断推流是否启用：用户启用 且 地址有效 且 端口非零
    sendingEnabled = enabled && validAddress && port != 0;
    // 记录传输模式标志
    tcpTransport = useTcp;
    // 保存解析后的 IP 地址
    serverAddress = address;
    // 保存原始主机名/IP 字符串
    serverHost = trimmedHost;
    // 保存目标端口号
    serverPort = port;
    // 中止当前 TCP 连接（如果存在）
    tcpSocket->abort();
    // 停止 TCP 重连定时器
    tcpReconnectTimer->stop();
    // 如果用户禁用了推流
    if (!enabled)
    {
        emit statusChanged(QStringLiteral("实时推流已关闭"));
        return;
    }
    // 如果地址或端口无效导致推流未能启用
    if (!sendingEnabled)
    {
        emit statusChanged(useTcp
                               ? QStringLiteral("TCP 目标地址和端口无效")
                               : QStringLiteral("UDP 目标必须是有效的 IP 地址和端口"));
        return;
    }
    // 重新生成流 ID：每次配置变更都产生新的流 ID，接收端可据此识别新会话
    streamId = QRandomGenerator::global()->generate();
    // 重置帧序列号和包序列号
    frameSequence = packetSequence = 0;
    // 重置编码器 PTS
    encoderPts = 0;
    // 释放旧编码器（新的配置可能改变编码参数）
    releaseEncoder();
    // 根据传输模式启动连接
    if (tcpTransport)
    {
        // TCP 模式：启动重连定时器并立即尝试连接
        tcpReconnectTimer->start();
        tryTcpConnect();
    }
    else
        // UDP 模式：UDP 是无连接协议，直接发送状态信息
        emit statusChanged(QStringLiteral("H.264/RTP/UDP 正在发送到 %1:%2")
                               .arg(serverAddress.toString())
                               .arg(serverPort));
}

// 尝试建立 TCP 连接（仅当推流启用、TCP 模式、且当前未连接时执行）
void TcpFrameSender::tryTcpConnect()
{
    // 前置条件检查：推流未启用、非 TCP 模式、无 socket 或已连接则直接返回
    if (!sendingEnabled || !tcpTransport || !tcpSocket ||
        tcpSocket->state() != QAbstractSocket::UnconnectedState)
        return;
    // 发送正在连接的状态通知
    emit statusChanged(QStringLiteral("H.264/TCP 正在连接 %1:%2")
                           .arg(serverHost)
                           .arg(serverPort));
    // 异步连接到目标主机和端口（非阻塞，连接结果通过信号通知）
    tcpSocket->connectToHost(serverHost, serverPort);
}

// 通过 TCP 发送一帧 H.264 编码数据（自定义 TCP 帧协议）
// @param accessUnit H.264 访问单元（一帧完整的编码数据）
// @param width 编码帧宽度
// @param height 编码帧高度
// @param timestampMs 时间戳（毫秒）
// @return true 发送成功，false 条件不满足或发送失败
bool TcpFrameSender::sendTcpFrame(const QByteArray &accessUnit, int width, int height,
                                  qint64 timestampMs)
{
    // 多种发送前置条件检查，任一不满足则返回 false：
    // socket 不存在、未连接、发送缓冲区堆积超限、帧数据为空、宽高无效或超范围
    if (!tcpSocket || tcpSocket->state() != QAbstractSocket::ConnectedState ||
        tcpSocket->bytesToWrite() > MaximumPendingTcpBytes || accessUnit.isEmpty() ||
        width <= 0 || height <= 0 || width > 65535 || height > 65535)
        return false;
    // 构造 TCP 帧的字节数组
    QByteArray packet;
    // 预分配内存：帧头 28 字节 + H.264 载荷大小
    packet.reserve(TcpFrameHeaderSize + accessUnit.size());
    // 创建数据流对象，用于按二进制格式写入帧头字段
    QDataStream stream(&packet, QIODevice::WriteOnly);
    // 设置为大端字节序（网络字节序标准）
    stream.setByteOrder(QDataStream::BigEndian);
    // 写入帧头：魔术字(4) + 版本(2) + 头大小(2) + 载荷大小(4) + 帧序号(4) + 时间戳(8) + 宽(2) + 高(2)
    stream << TcpFrameMagic << TcpFrameVersion << TcpFrameHeaderSize
           << quint32(accessUnit.size()) << frameSequence << timestampMs
           << quint16(width) << quint16(height);
    // 将 H.264 编码数据追加到帧头之后
    packet.append(accessUnit);
    // 通过 TCP 套接字写入整个帧数据
    const qint64 written = tcpSocket->write(packet);
    // 检查写入字节数是否与预期一致（TCP 写操作可能部分写入）
    if (written != packet.size())
        return false;
    // 帧序列号递增
    ++frameSequence;
    return true;
}

// 初始化 H.264 编码器及其关联的 FFmpeg 资源
// @param width 编码目标宽度（会被对齐到偶数）
// @param height 编码目标高度（会被对齐到偶数）
// @param error 输出参数：失败时接收错误描述字符串
// @return true 初始化成功，false 失败（error 中包含详细原因）
bool TcpFrameSender::initializeEncoder(int width, int height, QString *error)
{
    // 先释放可能存在的旧编码器，确保干净初始化
    releaseEncoder();
    // 注册旧版 FFmpeg 编解码器（libavcodec 57 兼容性处理）
    registerLegacyCodecs();
    // 尺寸有效性检查：宽高至少为 2
    if (width < 2 || height < 2)
    {
        if (error)
            *error = QStringLiteral("无效的输入尺寸：%1x%2").arg(width).arg(height);
        return false;
    }
    // 查找 H.264 编码器（优先使用 libx264 硬件/软件编码器）
    auto codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!codec)
    {
        if (error)
            *error = QStringLiteral("FFmpeg 中未找到 H.264 编码器");
        return false;
    }
    // 分配编码器上下文
    codecContext = avcodec_alloc_context3(codec);
    // 分配编码器输入帧（用于存储转换后的 YUV420P 数据）
    codecFrame = av_frame_alloc();
    // 分配编码器输出包（用于存储编码后的 H.264 数据）
    codecPacket = av_packet_alloc();
    // 检查内存分配是否全部成功
    if (!codecContext || !codecFrame || !codecPacket)
    {
        if (error)
            *error = QStringLiteral("FFmpeg 编码器内存分配失败");
        releaseEncoder();
        return false;
    }
    // 设置编码器类型为视频编码
    codecContext->codec_type = AVMEDIA_TYPE_VIDEO;
    // 设置编码器 ID 为 H.264
    codecContext->codec_id = AV_CODEC_ID_H264;
    // 设置编码宽度（向下取偶，兼容 YUV420P 格式要求）
    codecContext->width = width & ~1;
    // 设置编码高度（向下取偶）
    codecContext->height = height & ~1;
    // 设置像素格式为 YUV420P（H.264 标准格式，色度 4:2:0 子采样）
    codecContext->pix_fmt = AV_PIX_FMT_YUV420P;
    // 设置时间基：1/30 秒每 tick
    codecContext->time_base = AVRational{
        1, VideoPipelineConfig::StandardFramesPerSecond};
    // 设置帧率：30 FPS
    codecContext->framerate = AVRational{
        VideoPipelineConfig::StandardFramesPerSecond, 1};
    // 设置目标比特率：350 kbps
    codecContext->bit_rate = TargetBitRate;
    // 设置 GOP 大小：每 30 帧一个 IDR 关键帧
    codecContext->gop_size = VideoPipelineConfig::StandardFramesPerSecond;
    // 禁用 B 帧：设置最大 B 帧数为 0（低延迟推流不需要 B 帧）
    codecContext->max_b_frames = 0;
    // 设置编码线程数为 1（i.MX6ULL 单核 Cortex-A7，多线程无益且可能导致帧乱序）
    codecContext->thread_count = 1;
    // 启用低延迟标志
    codecContext->flags |= AV_CODEC_FLAG_LOW_DELAY;

    // 创建编码器选项字典
    AVDictionary *options = nullptr;
    // preset = "ultrafast"：使用最快编码预设，牺牲压缩率换取编码速度
    av_dict_set(&options, "preset", "ultrafast", 0);
    // tune = "zerolatency"：零延迟调优，适用于实时推流场景
    av_dict_set(&options, "tune", "zerolatency", 0);
    // profile = "baseline"：H.264 Baseline Profile（最基础配置，兼容性最好）
    av_dict_set(&options, "profile", "baseline", 0);
    // repeat_headers = "1"：在每个 IDR 帧前重复 SPS/PPS 头信息
    av_dict_set(&options, "repeat_headers", "1", 0);
    // x264-params：重复 SPS/PPS、禁用场景切换，并给每帧添加访问单元分隔符。
    // AUD 让保存后的 Annex B 裸码流可以可靠地区分每一帧。
    av_dict_set(&options, "x264-params",
                "repeat-headers=1:scenecut=0:aud=1", 0);
    // 打开编码器，应用上述所有参数
    const int result = avcodec_open2(codecContext, codec, &options);
    // 释放选项字典（avcodec_open2 会复制需要的值，原字典不再需要）
    av_dict_free(&options);
    // 检查编码器是否成功打开
    if (result < 0)
    {
        if (error)
            *error = QStringLiteral("H.264 编码器初始化失败：%1")
                         .arg(ffmpegError(result));
        releaseEncoder();
        return false;
    }
    // 设置编码帧的像素格式
    codecFrame->format = codecContext->pix_fmt;
    // 设置编码帧的宽度
    codecFrame->width = codecContext->width;
    // 设置编码帧的高度
    codecFrame->height = codecContext->height;
    // 为编码帧分配数据缓冲区（32 字节对齐，提高内存访问效率）
    const int bufferResult = av_frame_get_buffer(codecFrame, 32);
    if (bufferResult < 0)
    {
        if (error)
            *error = QStringLiteral("H.264 帧缓冲区分配失败：%1")
                         .arg(ffmpegError(bufferResult));
        releaseEncoder();
        return false;
    }
    // 重置编码器 PTS 为 0
    encoderPts = 0;
    return true;
}

// 释放所有 FFmpeg 编码器资源（编码器上下文、帧、包、像素格式转换器）
void TcpFrameSender::releaseEncoder()
{
    // 如果像素格式转换器存在，释放它
    if (scaleContext)
        sws_freeContext(scaleContext);
    // 将指针置空，防止悬空指针
    scaleContext = nullptr;
    // 释放编码输出包（av_packet_free 同时释放包和将指针置空）
    av_packet_free(&codecPacket);
    // 释放编码输入帧
    av_frame_free(&codecFrame);
    // 释放编码器上下文
    avcodec_free_context(&codecContext);
}

// 将 QImage 编码为 H.264 访问单元（Access Unit）
// @param image 输入原始 QImage 图像（支持 RGB16/RGB888 等格式）
// @param accessUnit 输出参数：编码后的 H.264 数据字节数组
// @param keyFrame 输出参数：指示当前帧是否为 IDR 关键帧
// @param error 输出参数：失败时接收错误描述
// @return true 编码成功，false 编码失败
bool TcpFrameSender::encodeFrame(const QImage &image, QByteArray *accessUnit,
                                 bool *keyFrame, QString *error)
{
    // 根据图像尺寸计算实际编码尺寸（受 MaximumEncodedWidth x MaximumEncodedHeight 限制）
    const QSize encodedSize = encodedSizeFor(image);
    // 如果编码器未初始化或尺寸不匹配，则重新初始化编码器
    if (!codecContext || codecContext->width != encodedSize.width() ||
        codecContext->height != encodedSize.height())
        if (!initializeEncoder(encodedSize.width(), encodedSize.height(), error))
            return false;

    // V4L2 采集帧本来就是 RGB565。直接让 swscale 读取这块内存，避免每帧先
    // 分配 RGB888 图像并执行一次 RGB565 -> RGB888 的全帧转换。
    // 转换后的图像（仅在输入格式非 RGB16/RGB888 时使用）
    QImage convertedImage;
    // 指向实际要转换的源图像指针
    const QImage *sourceImage = &image;
    // 源像素格式（FFmpeg AVPixelFormat 枚举值）
    AVPixelFormat sourceFormat = AV_PIX_FMT_NONE;
    // 判断输入图像的像素格式
    if (image.format() == QImage::Format_RGB16)
    {
        // RGB565 格式：根据系统字节序选择对应的 FFmpeg 像素格式
#if Q_BYTE_ORDER == Q_LITTLE_ENDIAN
        sourceFormat = AV_PIX_FMT_RGB565LE; // 小端序 RGB565
#else
        sourceFormat = AV_PIX_FMT_RGB565BE; // 大端序 RGB565
#endif
    }
    else if (image.format() == QImage::Format_RGB888)
        // RGB888 格式：直接映射到 FFmpeg RGB24
        sourceFormat = AV_PIX_FMT_RGB24;
    else
    {
        // 不支持直接转换的格式：先转换为 RGB888 再处理
        convertedImage = image.convertToFormat(QImage::Format_RGB888);
        sourceImage = &convertedImage;
        sourceFormat = AV_PIX_FMT_RGB24;
    }

    // 检查源图像有效性及编码帧是否可写
    if (sourceImage->isNull() || av_frame_make_writable(codecFrame) < 0)
    {
        if (error)
            *error = QStringLiteral("图像转换或编码帧缓冲区不可写");
        return false;
    }
    // 获取或创建像素格式转换上下文（RGB → YUV420P）
    // sws_getCachedContext 会复用已有 context 如果参数未变，避免重复创建
    scaleContext = sws_getCachedContext(
        scaleContext,               // 现有的转换上下文（可为 nullptr）
        sourceImage->width(),       // 源图像宽度
        sourceImage->height(),      // 源图像高度
        sourceFormat,               // 源像素格式
        codecContext->width,        // 目标宽度（编码器宽度）
        codecContext->height,       // 目标高度（编码器高度）
        AV_PIX_FMT_YUV420P,         // 目标像素格式：YUV420P
        SWS_FAST_BILINEAR,          // 缩放算法：快速双线性插值
        nullptr, nullptr, nullptr); // 不使用额外的滤波器参数
    if (!scaleContext)
    {
        if (error)
            *error = QStringLiteral("FFmpeg 像素格式转换器初始化失败");
        return false;
    }
    // 设置源数据指针数组：RGB 图像只有平面 0（打包格式），其余平面为空
    const uint8_t *sourceData[4] = {sourceImage->constBits(), nullptr, nullptr, nullptr};
    // 设置源行跨度数组：每一行的字节数
    int sourceLines[4] = {sourceImage->bytesPerLine(), 0, 0, 0};
    // 执行像素格式转换：RGB → YUV420P，同时完成缩放
    sws_scale(scaleContext, sourceData, sourceLines, 0, sourceImage->height(),
              codecFrame->data, codecFrame->linesize);
    // 设置编码帧的 PTS（显示时间戳）
    codecFrame->pts = encoderPts++;
    // 事件录像可能在推流中途开始。此时强制产生一个新的 IDR；配合
    // repeat_headers，录像文件会从可独立解码的 SPS/PPS + IDR 开始。
    codecFrame->pict_type = forceNextKeyFrame
                                ? AV_PICTURE_TYPE_I
                                : AV_PICTURE_TYPE_NONE;
    // 清空输出缓冲区
    accessUnit->clear();
    // 初始化关键帧标志为 false
    *keyFrame = false;

// 新版 FFmpeg API（send/receive 模式，libavcodec >= 57.37.100）
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(57, 37, 100)
    // 将原始帧送入编码器
    const int sent = avcodec_send_frame(codecContext, codecFrame);
    if (sent < 0)
    {
        if (error)
            *error = QStringLiteral("avcodec_send_frame: %1")
                         .arg(ffmpegError(sent));
        return false;
    }

    // 循环从编码器取出编码后的数据包（一帧可能产生多个 NAL 单元）
    while (true)
    {
        // 从编码器接收编码后的数据包
        const int result = avcodec_receive_packet(codecContext, codecPacket);
        // EAGAIN：编码器需要更多输入帧；EOF：编码器已刷新完毕
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
            break;
        if (result < 0)
        {
            if (error)
                *error = QStringLiteral("avcodec_receive_packet: %1").arg(ffmpegError(result));
            return false;
        }
        // 将编码数据追加到输出缓冲区
        accessUnit->append(reinterpret_cast<const char *>(codecPacket->data), codecPacket->size);
        // 检查是否为关键帧（IDR 帧），关键帧标志可取或（可能多个 NAL 单元中任一个是关键帧）
        *keyFrame = *keyFrame || ((codecPacket->flags & AV_PKT_FLAG_KEY) != 0);
        // 释放当前编码包的引用（减少内部引用计数，避免内存泄漏）
        av_packet_unref(codecPacket);
    }
#else
    // FFmpeg 3.0 及更早版本（i.MX6ULL SDK 为 libavcodec 57.24.102）
    // 尚未提供 send/receive API，一帧输入通过旧接口取得至多一个编码包。
    // 先释放编码包中可能存在的旧数据引用
    av_packet_unref(codecPacket);
    // 是否获取到编码包标志（输出参数）
    int gotPacket = 0;
    // 旧式编码 API：avcodec_encode_video2，一帧输入一次调用
    const int result = avcodec_encode_video2(codecContext, codecPacket,
                                             codecFrame, &gotPacket);
    if (result < 0)
    {
        if (error)
            *error = QStringLiteral("avcodec_encode_video2: %1")
                         .arg(ffmpegError(result));
        return false;
    }
    // 如果编码器产生了输出数据
    if (gotPacket)
    {
        // 将编码数据追加到输出缓冲区
        accessUnit->append(
            reinterpret_cast<const char *>(codecPacket->data),
            codecPacket->size);
        // 判断是否为关键帧
        *keyFrame = (codecPacket->flags & AV_PKT_FLAG_KEY) != 0;
    }
    // 释放编码包引用
    av_packet_unref(codecPacket);
#endif
    // 编码后检查输出缓冲区是否为空（理论上不应发生）
    if (accessUnit->isEmpty())
    {
        if (error)
            *error = QStringLiteral("编码器未产生 H.264 输出数据");
        return false;
    }
    // 只有编码器确实输出关键帧后才清除请求，避免录像从 P 帧开始。
    if (forceNextKeyFrame && *keyFrame)
        forceNextKeyFrame = false;
    return true;
}

// 通过 RTP/UDP 协议发送 H.264 访问单元（将大数据分包发送）
// @param accessUnit H.264 编码数据（访问单元）
// @param keyFrame 指示当前帧是否为关键帧
// @param timestampUs 时间戳（微秒）
// @return true 发送成功，false 条件不满足或发送失败
bool TcpFrameSender::sendAccessUnit(const QByteArray &accessUnit, bool keyFrame,
                                    quint64 timestampUs)
{
    // 前置条件检查：UDP socket 必须存在、推流已启用、当前为 UDP 模式、数据非空
    if (!udpSocket || !sendingEnabled || tcpTransport || accessUnit.isEmpty())
        return false;
    // 计算需要的 RTP 分包数（向上取整）
    const int count = (accessUnit.size() + VideoRtp::MaximumPayloadSize - 1) /
                      VideoRtp::MaximumPayloadSize;
    // 分包数有效性检查：0 或超过最大分包限制则失败
    if (count <= 0 || count > VideoRtp::MaximumFragmentsPerFrame)
        return false;
    // 逐分包发送
    for (int index = 0; index < count; ++index)
    {
        // 当前分包的起始偏移
        const int offset = index * VideoRtp::MaximumPayloadSize;
        // 当前分包的载荷大小（最后一个分包可能小于最大载荷）
        const int size = qMin(VideoRtp::MaximumPayloadSize, accessUnit.size() - offset);
        // 构造 RTP 分包头部
        VideoRtp::Header header;
        // 设置标志位：最后一个分包设置 MarkerFlag；关键帧设置 KeyFrameFlag
        header.flags = (index + 1 == count ? VideoRtp::MarkerFlag : 0) |
                       (keyFrame ? VideoRtp::KeyFrameFlag : 0);
        // 设置流 ID（用于接收端区分不同的推流会话）
        header.streamId = streamId;
        // 设置帧序列号（接收端可检测丢帧）
        header.frameSequence = frameSequence;
        // 设置包序列号并递增
        header.packetSequence = packetSequence++;
        // 当前分包在帧中的索引
        header.fragmentIndex = quint16(index);
        // 帧被拆分成的总分包数
        header.fragmentCount = quint16(count);
        // 发送时间戳（微秒）
        header.sendTimestampUs = timestampUs;
        // 调用 RTP 工具函数构造完整的数据报（头 + 载荷）
        const QByteArray datagram = VideoRtp::makeDatagram(
            header, accessUnit.constData() + offset, size);
        // 通过 UDP 套接字发送数据报到目标地址
        const qint64 written = udpSocket->writeDatagram(datagram, serverAddress, serverPort);
        // 检查是否完整写入
        if (written != datagram.size())
            return false;
    }
    // 帧序列号递增（每帧发送完成后递增一次）
    ++frameSequence;
    return true;
}

// 将已经编码好的 H.264 Annex B 访问单元写入录像文件
// @param accessUnit 一帧 H.264 编码数据
// @return true 写入成功或无录像文件（跳过），false 写入失败
bool TcpFrameSender::writeRecordingFrame(const QByteArray &accessUnit)
{
    // 如果录像文件未打开，无需写入，返回 true（不阻断推流流程）
    if (!recordingFile || !recordingFile->isOpen())
        return true;
    if (accessUnit.isEmpty() ||
        recordingFile->write(accessUnit) != accessUnit.size())
    {
        // 写入失败：发送错误信号
        emit recordingError(QStringLiteral("H.264 录像写入失败：%1")
                                .arg(recordingFile->errorString()));
        // 写入失败时自动停止录像
        stopRecording();
        return false;
    }
    return true;
}

// 核心帧处理函数：接收图像 → 编码 → 发送的完整流水线入口
// @param image 来自摄像头采集的原始 QImage 帧
void TcpFrameSender::sendFrame(const QImage &image)
{
    // 检查是否正在进行录像（文件已打开写入）
    const bool recording = recordingFile && recordingFile->isOpen();
    // 如果图像为空，或推流和录像都不需要处理，则丢弃此帧
    if (image.isNull() || (!sendingEnabled && !recording))
    {
        return;
    }
    // 获取当前时间的毫秒级时间戳（用于接收端的帧同步）
    const qint64 timestampMs = QDateTime::currentMSecsSinceEpoch();
    // 编码后的 H.264 数据缓冲区
    QByteArray encodedFrame;
    // 关键帧标志（输出参数）
    bool keyFrame = false;
    // 编码错误描述（输出参数）
    QString error;
    // 调用编码函数：将 QImage 编码为 H.264 访问单元
    const bool encoded = encodeFrame(image, &encodedFrame, &keyFrame, &error);
    // 编码失败处理
    if (!encoded)
    {
        // 发送编码失败状态信息
        emit statusChanged(QStringLiteral("H.264 编码失败：%1").arg(error));
        if (recording)
        {
            emit recordingError(QStringLiteral("H.264 录像编码失败：%1").arg(error));
            stopRecording();
        }
        return;
    }
    // 同一个 H.264 编码结果直接写入录像，避免录像和推流重复编码。
    if (recording)
        writeRecordingFrame(encodedFrame);
    // 仅录像时写盘完成即可，不进入网络发送路径。
    if (!sendingEnabled)
        return;
    // 根据传输模式选择发送方式：
    // TCP 模式 → sendTcpFrame    UDP 模式 → sendAccessUnit (RTP/UDP)
    const bool sent = tcpTransport
                          ? sendTcpFrame(encodedFrame, codecContext->width,
                                         codecContext->height, timestampMs)
                          : sendAccessUnit(encodedFrame, keyFrame,
                                           quint64(timestampMs) * 1000); // 毫秒转微秒
    // 发送失败处理
    if (!sent)
    {
        // 根据传输模式发送不同的错误状态信息
        if (tcpTransport)
            // TCP 模式：区分是"已连接但拥塞"还是"未连接"
            emit statusChanged(tcpSocket && tcpSocket->state() == QAbstractSocket::ConnectedState
                                   ? QStringLiteral("H.264/TCP 发送拥塞或失败：%1")
                                         .arg(tcpSocket->errorString())
                                   : QStringLiteral("H.264/TCP 尚未连接，当前帧已丢弃"));
        else
            // UDP 模式：显示 UDP 错误信息
            emit statusChanged(QStringLiteral("RTP/UDP 发送失败：%1")
                                   .arg(udpSocket ? udpSocket->errorString()
                                                  : QStringLiteral("socket 未初始化")));
    }
}

// 开始录像：将视频帧编码成 H.264 Annex B 裸码流写入文件
// @param filePath 录像文件的完整保存路径
// @param durationMs 自动事件录像持续时间（毫秒），必须大于 0
void TcpFrameSender::startRecording(const QString &filePath, int durationMs)
{
    if (durationMs <= 0)
    {
        emit recordingError(QStringLiteral("自动事件录像时长必须大于 0"));
        return;
    }
    // 确保网络和文件资源已初始化
    if (!recordingFile)
        initialize();
    // 如果文件已打开（可能已在录像中）
    if (recordingFile->isOpen())
    {
        recordingStopTimer->start(durationMs);
        return;
    }
    // 创建录像文件所在的父目录（如果不存在）
    QDir().mkpath(QFileInfo(filePath).absolutePath());
    // 设置录像文件的路径
    recordingFile->setFileName(filePath);
    // 以只写+截断模式打开文件（覆盖已有内容）
    if (!recordingFile->open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        // 文件打开失败：发送错误信号
        emit recordingError(QStringLiteral("无法创建录像文件：%1").arg(recordingFile->errorString()));
        return;
    }
    // 记录当前活跃的录像文件路径
    activeRecordingPath = filePath;
    // 即使编码器正在用于网络推流，也让录像从新的关键帧开始。
    forceNextKeyFrame = true;
    recordingStopTimer->start(durationMs);
    // 发送录像开始状态信号
    emit recordingStateChanged(true, filePath);
}

// 停止录像：关闭录像文件并发送状态变更信号
void TcpFrameSender::stopRecording()
{
    // 停止录像停止定时器
    if (recordingStopTimer)
        recordingStopTimer->stop();
    // 如果文件未打开，无需执行关闭操作
    if (!recordingFile || !recordingFile->isOpen())
        return;
    // 保存文件路径（close 后 activeRecordingPath 会被清空）
    const QString path = activeRecordingPath;
    // 强制将缓冲区数据写入磁盘
    recordingFile->flush();
    // 关闭录像文件
    recordingFile->close();
    // 清空活跃录像路径（表示当前无录像进行中）
    activeRecordingPath.clear();
    // 发送录像停止状态信号
    emit recordingStateChanged(false, path);
}

// 停止所有操作：推流、录像、编码器、网络连接
void TcpFrameSender::stop()
{
    // 禁用推流标志
    sendingEnabled = false;
    // 停止 TCP 重连定时器
    if (tcpReconnectTimer)
        tcpReconnectTimer->stop();
    // 停止录像（如果正在进行中）
    stopRecording();
    // 释放编码器资源
    releaseEncoder();
    // 中止 TCP 连接（如果有）
    if (tcpSocket)
        tcpSocket->abort();
    // 关闭 UDP 套接字（如果有）
    if (udpSocket)
        udpSocket->close();
}
