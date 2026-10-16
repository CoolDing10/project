// 包含本类的头文件声明
#include "tcp_frame_sender.h"        // 引入 TcpFrameSender 类声明。
#include "video_pipeline_config.h"   // 引入编码帧率与 GOP 共用的标准帧率常量。
// 引入 RTP 打包协议定义（分包头结构体、分包工具函数）
#include "rtp_packet.h" // 引入自定义 RTP 分片头及 makeDatagram() 打包函数。

// Qt 数据流类，用于二进制序列化 TCP 帧头
#include <QDataStream> // 按固定大端顺序把 TCP 帧头字段写入 QByteArray。
// Qt 日期时间类，用于获取当前时间的毫秒级时间戳
#include <QDateTime> // 生成每帧的 Unix 毫秒时间戳。
// Qt 目录操作类，用于创建录像文件所需的父目录
#include <QDir> // 创建录像文件所在目录。
// Qt 文件操作类，用于录像文件的打开、写入和关闭
#include <QFile> // 以二进制方式写入 H.264 Annex B 录像数据。
// Qt 文件信息类，用于提取文件路径中的目录部分
#include <QFileInfo> // 从录像文件完整路径中取得父目录。
// Qt 随机数生成器类，用于生成唯一的流 ID
#include <QRandomGenerator> // 为每次 UDP 推流会话生成随机 streamId。
// Qt 尺寸类，用于图像的宽高缩放计算
#include <QSize> // 保存按最大分辨率等比例缩放后的编码宽高。
// Qt TCP 套接字类，用于建立和维护 TCP 网络连接
#include <QTcpSocket> // 建立可靠字节流连接并发送自定义 H.264 帧包。
// Qt 定时器类，用于 TCP 自动重连和录像定时停止
#include <QTimer> // 实现 TCP 自动重连和事件录像自动停止。
// Qt UDP 套接字类，用于发送 UDP 数据报
#include <QUdpSocket> // 发送一个个独立的 RTP 风格 UDP 数据报。
// Qt 全局定义（如 Q_BYTE_ORDER 字节序宏）
#include <QtGlobal> // 提供 quint 类型、qMin/qMax 和平台字节序宏。

// FFmpeg C 库头文件，使用 extern "C" 包裹以避免 C++ 名称修饰问题
extern "C" // FFmpeg 是 C API，禁止 C++ 对函数名进行名称修饰。
{
// FFmpeg 编解码核心 API：avcodec_find_encoder、avcodec_open2 等
#include <libavcodec/avcodec.h> // 声明编码器查找、上下文、帧、包以及编解码调用。
// FFmpeg 版本号宏定义
#include <libavcodec/version.h> // 提供版本宏，用于选择新旧编码 API。
// FFmpeg 字典操作 API：av_dict_set、av_dict_free（用于设置编码器选项）
#include <libavutil/dict.h> // 提供 AVDictionary 及编码器字符串选项接口。
// FFmpeg 错误码定义和错误描述函数：AVERROR、av_strerror
#include <libavutil/error.h> // 提供 AVERROR、AVERROR_EOF 和 av_strerror()。
// FFmpeg 像素格式转换 API：sws_getCachedContext、sws_scale、sws_freeContext
#include <libswscale/swscale.h> // 提供 RGB565/RGB24 到 YUV420P 的缩放与格式转换。
}

// 匿名命名空间：这些常量和工具函数仅在当前 .cpp 文件内可见
namespace // 匿名命名空间使编码参数和辅助函数仅对本实现文件可见。
{
    // i.MX6ULL只有一个Cortex-A7核心，为降低libx264软件编码负担，
    // 摄像头仍以640×480采集，本地预览保持原尺寸，推流编码限制为320×240。
    // 最大编码宽度：320像素
    constexpr int MaximumEncodedWidth = 320; // 编译期最大编码宽度，限制单核ARM编码负担。
    // 最大编码高度：240像素
    constexpr int MaximumEncodedHeight = 240; // 编译期最大编码高度。
    // 目标比特率：350 kbps，在画质和带宽之间取平衡
    constexpr int TargetBitRate = 350000;     // H.264 平均目标码率，单位 bit/s。
    // TCP 帧魔术字："IMXV"，用于接收端校验数据完整性（类似文件格式的文件签名）
    constexpr quint32 TcpFrameMagic = 0x494d5856; // "IMXV"
    // 版本 2 表示载荷为一帧 H.264 Access Unit；版本 1 曾用于 JPEG。
    // TCP 帧协议版本号
    constexpr quint16 TcpFrameVersion = 2;    // 接收端据此按 H.264 版本解析载荷。
    // TCP 帧头大小：28 字节（魔术字 4 + 版本 2 + 头大小 2 + 载荷大小 4 + 帧序号 4 + 时间戳 8 + 宽度 2 + 高度 2）
    constexpr quint16 TcpFrameHeaderSize = 28; // 自定义 TCP 帧头固定字节数。
    // TCP 发送缓冲区最大待发送字节数：512 KB，超过此值则丢弃当前帧以避免内存堆积
    constexpr qint64 MaximumPendingTcpBytes = 512 * 1024; // socket 待写数据超过 512KiB 时丢弃新帧。

    // 将 FFmpeg 错误码转换为可读的中文/英文错误字符串
    // @param code FFmpeg API 返回的负值错误码
    // @return 人类可读的错误描述字符串
    QString ffmpegError(int code) // code 是 FFmpeg 返回的负错误码，函数返回其可读说明。
    {
        // FFmpeg 错误描述缓冲区（AV_ERROR_MAX_STRING_SIZE 通常为 64 字节）
        char text[AV_ERROR_MAX_STRING_SIZE] = {0}; // text 是 FFmpeg 要写入的固定长度 C 字符缓冲区。
        // 调用 FFmpeg API 将错误码转为字符串描述
        av_strerror(code, text, sizeof(text)); // 将 code 翻译到 text，长度包含结尾零字符空间。
        // 将 C 字符串转为 QString 返回
        return QString::fromLocal8Bit(text); // 把 FFmpeg C 字符串转为 Qt 日志和信号使用的 QString。
    }

    // 注册旧版 FFmpeg 内置编解码器（兼容 i.MX6ULL SDK 的 libavcodec 57）
    void registerLegacyCodecs() // 只为旧版 libavcodec 执行一次全局编解码器注册。
    {
// 仅当 libavcodec 主版本号 < 58 时执行（58 及以上版本自动注册）
#if LIBAVCODEC_VERSION_MAJOR < 58 // 新版已自动注册，只有旧版本需要显式调用。
        // libavcodec 57（i.MX6ULL SDK）不会自动注册内置编码器。
        // 未调用此函数时，avcodec_find_encoder(AV_CODEC_ID_H264) 返回空指针，
        // 即使 ffmpeg -encoders 能看到 libx264。
        // 静态变量，确保注册只执行一次
        static bool registered = false; // 函数静态变量跨调用保存是否已经完成注册。
        if (!registered)                // 仅第一次初始化编码器时进入。
        {
            // 注册所有内置编解码器
            avcodec_register_all(); // 把 FFmpeg 内置编码器登记到全局注册表。
            registered = true;      // 防止后续重复执行全局注册。
        }
#endif // 结束旧版 FFmpeg 兼容分支。
    }

    // 根据最大编码分辨率约束计算实际编码尺寸。
    // @param image 输入的摄像头原始图像。
    // @return 不超过320×240、保持原宽高比且宽高均为偶数的编码尺寸。
    QSize encodedSizeFor(const QImage &image) // image是原始帧，返回FFmpeg实际使用的目标尺寸。
    {
        // 先取得采集图像的原始尺寸。
        QSize size = image.size(); // size是可修改的原始宽高副本。
        // 任一维度超过编码上限时，在320×240边界内等比例缩小。
        if (size.width() > MaximumEncodedWidth || // 原图宽度超过320，或者
            size.height() > MaximumEncodedHeight) // 原图高度超过240时执行缩放。
            size.scale(MaximumEncodedWidth, MaximumEncodedHeight, // 把尺寸限制在320×240以内，
                       Qt::KeepAspectRatio); // 保持宽高比，避免画面被拉伸变形。
        // YUV420P色度平面要求宽度和高度均为偶数。
        size.setWidth(qMax(2, size.width() & ~1)); // 向下取偶数，并确保宽度至少为2。
        size.setHeight(qMax(2, size.height() & ~1)); // 向下取偶数，并确保高度至少为2。
        return size; // 返回编码器和像素转换器共同使用的最终尺寸。
    }

}

// 构造函数：初始化所有成员变量为默认值
// @param parent 父 QObject，用于 Qt 对象树管理
TcpFrameSender::TcpFrameSender(QObject *parent) // parent 是 Qt 父对象；对象随后通常会被移入网络线程。
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
TcpFrameSender::~TcpFrameSender() // 析构时释放不受 Qt 对象树管理的 FFmpeg C 资源。
{
    // 释放所有 FFmpeg 编码器资源（编码器上下文、帧、包、像素转换器）
    releaseEncoder(); // 统一释放 scaleContext、codecPacket、codecFrame 和 codecContext。
}

// 初始化网络套接字和定时器资源（仅首次调用有效，后续调用直接返回）
void TcpFrameSender::initialize() // 在对象所属网络线程中创建 socket、定时器和录像文件对象。
{
    // 如果 UDP 套接字已创建，说明已初始化过，直接返回避免重复创建
    if (udpSocket) // udpSocket 非空作为已经完成初始化的标志。
        return;    // 保证初始化过程幂等，避免重复连接信号或创建子对象。
    // 创建 UDP 套接字，父对象为 this（由 Qt 对象树管理生命周期）
    udpSocket = new QUdpSocket(this); // 创建由 TcpFrameSender 自动释放的 UDP socket。
    // 设置 UDP 发送缓冲区大小为 256 KB
    udpSocket->setSocketOption(QAbstractSocket::SendBufferSizeSocketOption, 256 * 1024); // 请求 256KiB 内核发送缓冲区。
    // 创建 TCP 套接字
    tcpSocket = new QTcpSocket(this); // 创建与本对象具有相同线程归属的 TCP socket。
    // 启用 TCP_NODELAY 选项：禁用 Nagle 算法，减少延迟
    tcpSocket->setSocketOption(QAbstractSocket::LowDelayOption, 1); // 值 1 开启 TCP_NODELAY，避免小包聚合延迟。
    // 启用 TCP KeepAlive：定期发送保活探测包，检测断线
    tcpSocket->setSocketOption(QAbstractSocket::KeepAliveOption, 1); // 值 1 开启 TCP KeepAlive 探测。
    // 创建 TCP 重连定时器
    tcpReconnectTimer = new QTimer(this); // 创建网络线程内运行的周期重连定时器。
    // 设置重连间隔为 1000 毫秒（1 秒）
    tcpReconnectTimer->setInterval(1000); // 每 1000ms 检查并尝试一次 TCP 连接。
    // 定时器超时 → 调用 tryTcpConnect() 尝试重连
    connect(tcpReconnectTimer, &QTimer::timeout, this, // 定时器超时信号和发送器位于同一线程。
            &TcpFrameSender::tryTcpConnect);           // 每次超时执行带状态检查的连接尝试。
    // TCP 连接成功时的处理 lambda
    connect(tcpSocket, &QTcpSocket::connected, this, [this] // TCP 异步握手成功后执行此 Lambda；捕获 this 访问成员。
            {
        // 每次 TCP 重连都从新的 IDR + SPS/PPS 开始，避免接收端缺少参考帧。
        // 释放旧编码器，确保重连后产生新的关键帧和序列参数集
        releaseEncoder(); // 丢弃旧会话编码状态，使接收端从完整新码流开始。
        // 重置编码器 PTS
        encoderPts = 0; // 新会话第一帧从 PTS 0 开始。
        // 重置帧序列号
        frameSequence = 0; // 新 TCP 会话的应用层帧序号从 0 开始。
        // 发送连接成功状态信息
        emit statusChanged(QStringLiteral("H.264/TCP 已连接 %1:%2") // 构造给 UI 的连接成功文本，
                               .arg(serverHost).arg(serverPort)); });  // 依次填入主机名和端口并结束 Lambda。
    // TCP 断开连接时的处理 lambda
    connect(tcpSocket, &QTcpSocket::disconnected, this, [this] // 远端关闭或网络中断后执行此 Lambda。
            {
        // 仅在推流启用且为 TCP 模式时才显示断开状态
        if (sendingEnabled && tcpTransport) // 只有用户仍要求 TCP 推流时才报告并继续重连。
            emit statusChanged(QStringLiteral("H.264/TCP 已断开，正在重连 %1:%2") // 构造断线状态，
                                   .arg(serverHost).arg(serverPort)); });            // 填入目标并结束 Lambda。
    // TCP 连接错误时的处理 lambda
    connect(tcpSocket, // 监听 TCP socket 错误信号。
            QOverload<QAbstractSocket::SocketError>::of(&QTcpSocket::error), // 指定带 SocketError 参数的重载。
            this, [this](QAbstractSocket::SocketError) // 当前 Lambda 不使用枚举值，直接读取 socket 的详细错误文本。
            {
        // 仅在推流启用且为 TCP 模式时才显示错误状态
        if (sendingEnabled && tcpTransport) // 只在当前仍是活动 TCP 推流配置时更新 UI。
            emit statusChanged(QStringLiteral("H.264/TCP 连接失败：%1（自动重连）") // 错误提示模板，
                                   .arg(tcpSocket->errorString())); });              // 填入 Qt socket 错误文本。
    // 创建录像停止定时器
    recordingStopTimer = new QTimer(this); // 创建控制事件录像时长的定时器。
    // 设为单次触发：超时后只执行一次，不会重复
    recordingStopTimer->setSingleShot(true); // 到期一次后自动停止，不周期重复调用 stopRecording()。
    // 定时器超时 → 调用 stopRecording() 自动停止录像
    connect(recordingStopTimer, &QTimer::timeout, this, &TcpFrameSender::stopRecording); // 到时关闭文件并反馈录像结束。
    // 创建录像文件对象
    recordingFile = new QFile(this); // 创建可反复设置文件名并打开的录像文件对象。
}

// 配置推流目标和参数
// @param host 目标主机名或 IP 地址字符串
// @param port 目标端口号
// @param enabled 是否启用推流
// @param transport "tcp" 或 "udp" 传输协议选择
void TcpFrameSender::configure(const QString &host, quint16 port, bool enabled, // host/port 是目标；enabled 是推流总开关。
                               const QString &transport) // transport 为不区分大小写的 "tcp" 或其他 UDP 值。
{
    // 如果尚未初始化，先执行初始化
    if (!udpSocket) // 尚未创建基础资源时，
        initialize(); // 先在当前网络线程完成一次初始化。
    // 判断是否使用 TCP 传输模式（不区分大小写比较）
    const bool useTcp = transport.compare(QStringLiteral("tcp"), // useTcp 保存协议字符串是否表示 TCP，
                                          Qt::CaseInsensitive) == 0; // 比较时忽略大小写，返回 0 表示相等。
    // IP 地址对象
    QHostAddress address; // address 接收 UDP 模式解析出的数值 IP；TCP 可直接使用主机名。
    // 去除主机名/地址字符串两端的空白字符
    const QString trimmedHost = host.trimmed(); // 去除用户输入首尾空白，作为实际连接目标。
    // 验证地址有效性：TCP 模式只需非空，UDP 模式必须是有效 IP 地址
    const bool validAddress = useTcp ? !trimmedHost.isEmpty() // TCP 允许域名，只要求字符串非空；
                                     : address.setAddress(trimmedHost); // UDP writeDatagram 需要可解析的数值 IP。
    // 综合判断推流是否启用：用户启用 且 地址有效 且 端口非零
    sendingEnabled = enabled && validAddress && port != 0; // 三个条件同时满足才进入实际网络发送路径。
    // 记录传输模式标志
    tcpTransport = useTcp; // 保存后续 sendFrame() 选择 TCP 或 UDP 的协议状态。
    // 保存解析后的 IP 地址
    serverAddress = address; // 保存 UDP 模式使用的解析后地址。
    // 保存原始主机名/IP 字符串
    serverHost = trimmedHost; // 保存 TCP connectToHost() 使用的主机名或 IP 文本。
    // 保存目标端口号
    serverPort = port; // 保存两种传输方式共同使用的目标端口。
    // 中止当前 TCP 连接（如果存在）
    tcpSocket->abort(); // 立即放弃旧目标连接及其未发送数据，准备应用新配置。
    // 停止 TCP 重连定时器
    tcpReconnectTimer->stop(); // 防止配置切换期间继续尝试连接旧地址。
    // 如果用户禁用了推流
    if (!enabled) // 用户明确关闭推流时，不把地址无效当作错误。
    {
        emit statusChanged(QStringLiteral("实时推流已关闭")); // 通知 UI 当前稳定状态。
        return;                                             // 不生成会话，也不启动连接。
    }
    // 如果地址或端口无效导致推流未能启用
    if (!sendingEnabled) // 用户要求开启，但地址或端口校验失败。
    {
        emit statusChanged(useTcp                                // 根据所选协议给出针对性错误，
                               ? QStringLiteral("TCP 目标地址和端口无效") // TCP 需要非空主机和非零端口，
                               : QStringLiteral("UDP 目标必须是有效的 IP 地址和端口")); // UDP 还必须是数值 IP。
        return;                                                 // 配置无效，不启动发送。
    }
    // 重新生成流 ID：每次配置变更都产生新的流 ID，接收端可据此识别新会话
    streamId = QRandomGenerator::global()->generate(); // 为本次配置生成新的 32 位 UDP 流会话标识。
    // 重置帧序列号和包序列号
    frameSequence = packetSequence = 0; // 新会话的帧序号和 UDP 包序号同时从 0 开始。
    // 重置编码器 PTS
    encoderPts = 0; // 新会话重新建立连续的编码时间轴。
    // 释放旧编码器（新的配置可能改变编码参数）
    releaseEncoder(); // 下一帧会按当前图像尺寸重新创建编码器并输出 SPS/PPS、IDR。
    // 根据传输模式启动连接
    if (tcpTransport) // TCP 是有连接协议，必须先建立连接。
    {
        // TCP 模式：启动重连定时器并立即尝试连接
        tcpReconnectTimer->start(); // 开启每秒一次的断线重连检查。
        tryTcpConnect();            // 不等待第一次定时器，立即发起首次连接。
    }
    else // UDP 无需握手或维护连接状态。
        // UDP 模式：UDP 是无连接协议，直接发送状态信息
        emit statusChanged(QStringLiteral("H.264/RTP/UDP 正在发送到 %1:%2") // 构造当前发送目标状态，
                               .arg(serverAddress.toString())                 // %1 填入规范化 IP，
                               .arg(serverPort));                             // %2 填入 UDP 端口。
}

// 尝试建立 TCP 连接（仅当推流启用、TCP 模式、且当前未连接时执行）
void TcpFrameSender::tryTcpConnect() // 在条件允许时异步发起一次 TCP 连接。
{
    // 前置条件检查：推流未启用、非 TCP 模式、无 socket 或已连接则直接返回
    if (!sendingEnabled || !tcpTransport || !tcpSocket ||                 // 推流关闭、非 TCP 或 socket 未创建时不连接；并且
        tcpSocket->state() != QAbstractSocket::UnconnectedState)          // 连接中或已连接时也不能重复 connectToHost()。
        return;                                                           // 保持当前状态，等待后续定时器或 socket 信号。
    // 发送正在连接的状态通知
    emit statusChanged(QStringLiteral("H.264/TCP 正在连接 %1:%2") // 告知 UI 正在进行异步握手，
                           .arg(serverHost)                         // 填入目标主机，
                           .arg(serverPort));                       // 填入目标端口。
    // 异步连接到目标主机和端口（非阻塞，连接结果通过信号通知）
    tcpSocket->connectToHost(serverHost, serverPort); // 立即返回；成功、失败结果由 connected/error 信号通知。
}

// 通过 TCP 发送一帧 H.264 编码数据（自定义 TCP 帧协议）
// @param accessUnit H.264 访问单元（一帧完整的编码数据）
// @param width 编码帧宽度
// @param height 编码帧高度
// @param timestampMs 时间戳（毫秒）
// @return true 发送成功，false 条件不满足或发送失败
bool TcpFrameSender::sendTcpFrame(const QByteArray &accessUnit, int width, int height, // accessUnit 是完整编码帧，width/height 是编码尺寸。
                                  qint64 timestampMs) // timestampMs 是采集处理时的 Unix 毫秒时间戳。
{
    // 多种发送前置条件检查，任一不满足则返回 false：
    // socket 不存在、未连接、发送缓冲区堆积超限、帧数据为空、宽高无效或超范围
    if (!tcpSocket || tcpSocket->state() != QAbstractSocket::ConnectedState || // socket 必须存在且已连接；
        tcpSocket->bytesToWrite() > MaximumPendingTcpBytes || accessUnit.isEmpty() || // 待发送队列不能过量，帧也不能为空；
        width <= 0 || height <= 0 || width > 65535 || height > 65535)          // 宽高必须能装入 16 位无符号头字段。
        return false;                                                          // 任一条件不满足就丢弃本帧。
    // 构造 TCP 帧的字节数组
    QByteArray packet; // packet 保存即将一次性写入 socket 的“28 字节帧头 + H.264 数据”。
    // 预分配内存：帧头 28 字节 + H.264 载荷大小
    packet.reserve(TcpFrameHeaderSize + accessUnit.size()); // 一次预留完整容量，减少 append 导致的重新分配。
    // 创建数据流对象，用于按二进制格式写入帧头字段
    QDataStream stream(&packet, QIODevice::WriteOnly); // stream 将基本整数类型依次序列化到 packet。
    // 设置为大端字节序（网络字节序标准）
    stream.setByteOrder(QDataStream::BigEndian); // 固定使用网络字节序，消除两端 CPU 字节序差异。
    // 写入帧头：魔术字(4) + 版本(2) + 头大小(2) + 载荷大小(4) + 帧序号(4) + 时间戳(8) + 宽(2) + 高(2)
    stream << TcpFrameMagic << TcpFrameVersion << TcpFrameHeaderSize // 写入协议识别字、版本和头长度，
           << quint32(accessUnit.size()) << frameSequence << timestampMs // 再写载荷长度、帧序号和时间戳，
           << quint16(width) << quint16(height);                         // 最后写 16 位编码宽高。
    // 将 H.264 编码数据追加到帧头之后
    packet.append(accessUnit); // 将 H.264 Annex B 访问单元紧接在帧头之后。
    // 通过 TCP 套接字写入整个帧数据
    const qint64 written = tcpSocket->write(packet); // written 是成功复制到 Qt 发送缓冲区的字节数。
    // 检查写入字节数是否与预期一致（TCP 写操作可能部分写入）
    if (written != packet.size()) // 负值或短写都表示整帧没有完整排入发送缓冲区。
        return false;             // 不增加序号，让上层报告发送失败。
    // 帧序列号递增
    ++frameSequence; // 仅完整接收一帧后才递增应用层帧序号。
    return true;     // 告诉调用者本帧已经成功提交给 TCP socket。
}

// 初始化 H.264 编码器及其关联的 FFmpeg 资源
// @param width 编码目标宽度（会被对齐到偶数）
// @param height 编码目标高度（会被对齐到偶数）
// @param error 输出参数：失败时接收错误描述字符串
// @return true 初始化成功，false 失败（error 中包含详细原因）
bool TcpFrameSender::initializeEncoder(int width, int height, QString *error) // width/height 是目标尺寸；error 接收失败原因。
{
    // 先释放可能存在的旧编码器，确保干净初始化
    releaseEncoder(); // 避免旧尺寸的上下文、帧和转换器残留。
    // 注册旧版 FFmpeg 编解码器（libavcodec 57 兼容性处理）
    registerLegacyCodecs(); // 确保旧版 FFmpeg 的 H.264 编码器可被查找到。
    // 尺寸有效性检查：宽高至少为 2
    if (width < 2 || height < 2) // YUV420P 至少需要 2×2 像素。
    {
        if (error) // 调用者提供了错误输出指针时，
            *error = QStringLiteral("无效的输入尺寸：%1x%2").arg(width).arg(height); // 写入包含实际宽高的说明。
        return false; // 参数不合法，不分配任何编码资源。
    }
    // 查找 H.264 编码器（优先使用 libx264 硬件/软件编码器）
    auto codec = avcodec_find_encoder(AV_CODEC_ID_H264); // codec 指向 FFmpeg 注册表中的 H.264 编码器描述。
    if (!codec)                                          // 空指针表示当前 FFmpeg 没有可用 H.264 编码器。
    {
        if (error) // 存在错误输出参数时，
            *error = QStringLiteral("FFmpeg 中未找到 H.264 编码器"); // 给出缺少编码器的直接原因。
        return false; // 无编码器无法继续初始化。
    }
    // 分配编码器上下文
    codecContext = avcodec_alloc_context3(codec); // 分配并绑定所选编码器的配置与运行状态上下文。
    // 分配编码器输入帧（用于存储转换后的 YUV420P 数据）
    codecFrame = av_frame_alloc(); // 分配只含元数据的 AVFrame，像素缓冲区稍后单独分配。
    // 分配编码器输出包（用于存储编码后的 H.264 数据）
    codecPacket = av_packet_alloc(); // 分配复用的压缩输出包对象。
    // 检查内存分配是否全部成功
    if (!codecContext || !codecFrame || !codecPacket) // 任一 FFmpeg 分配失败都不能组成完整流水线。
    {
        if (error) // 调用者需要错误说明时，
            *error = QStringLiteral("FFmpeg 编码器内存分配失败"); // 报告资源分配阶段失败。
        releaseEncoder(); // 释放可能已经成功分配的其他对象。
        return false;     // 初始化失败。
    }
    // 设置编码器类型为视频编码
    codecContext->codec_type = AVMEDIA_TYPE_VIDEO; // 明确上下文处理视频而非音频或字幕。
    // 设置编码器 ID 为 H.264
    codecContext->codec_id = AV_CODEC_ID_H264; // 明确输出编码格式为 H.264/AVC。
    // 设置编码宽度（向下取偶，兼容 YUV420P 格式要求）
    codecContext->width = width & ~1; // 清除最低位，将宽度向下对齐为偶数。
    // 设置编码高度（向下取偶）
    codecContext->height = height & ~1; // 清除最低位，将高度向下对齐为偶数。
    // 设置像素格式为 YUV420P（H.264 标准格式，色度 4:2:0 子采样）
    codecContext->pix_fmt = AV_PIX_FMT_YUV420P; // 编码器输入使用一个 Y 平面和两个 1/4 色度平面。
    // 设置时间基：1/30 秒每 tick
    codecContext->time_base = AVRational{ // PTS 每增加 1 所代表的时间单位，
        1, VideoPipelineConfig::StandardFramesPerSecond}; // 设置为 1/30 秒。
    // 设置帧率：30 FPS
    codecContext->framerate = AVRational{ // 向编码器声明输入帧率，
        VideoPipelineConfig::StandardFramesPerSecond, 1}; // 设置为 30/1 FPS。
    // 设置目标比特率：350 kbps
    codecContext->bit_rate = TargetBitRate; // 目标平均输出码率为 350000 bit/s。
    // 设置 GOP 大小：每 30 帧一个 IDR 关键帧
    codecContext->gop_size = VideoPipelineConfig::StandardFramesPerSecond; // 30 FPS 下约每秒安排一次关键帧。
    // 禁用 B 帧：设置最大 B 帧数为 0（低延迟推流不需要 B 帧）
    codecContext->max_b_frames = 0; // 禁止需要未来帧参考的 B 帧，减少缓存和端到端延迟。
    // 设置编码线程数为 1（i.MX6ULL 单核 Cortex-A7，多线程无益且可能导致帧乱序）
    codecContext->thread_count = 1; // 单核处理器只启用一个编码线程。
    // 启用低延迟标志
    codecContext->flags |= AV_CODEC_FLAG_LOW_DELAY; // 告知编码器优先采用低延迟工作方式。

    // 创建编码器选项字典
    AVDictionary *options = nullptr; // options 是传给具体 H.264 编码器的键值参数集合。
    // preset = "ultrafast"：使用最快编码预设，牺牲压缩率换取编码速度
    av_dict_set(&options, "preset", "ultrafast", 0); // 使用最低计算量的 x264 预设以适应 i.MX6ULL。
    // tune = "zerolatency"：零延迟调优，适用于实时推流场景
    av_dict_set(&options, "tune", "zerolatency", 0); // 关闭会增加缓存的编码策略。
    // profile = "baseline"：H.264 Baseline Profile（最基础配置，兼容性最好）
    av_dict_set(&options, "profile", "baseline", 0); // 使用不含 B 帧等高级工具的高兼容 Profile。
    // repeat_headers = "1"：在每个 IDR 帧前重复 SPS/PPS 头信息
    av_dict_set(&options, "repeat_headers", "1", 0); // 要求关键帧重复携带解码所需 SPS/PPS。
    // x264-params：重复 SPS/PPS、禁用场景切换，并给每帧添加访问单元分隔符。
    // AUD 让保存后的 Annex B 裸码流可以可靠地区分每一帧。
    av_dict_set(&options, "x264-params",                       // 传递 libx264 专用组合参数，
                "repeat-headers=1:scenecut=0:aud=1", 0);      // 重复头、禁场景切换关键帧并加入 AUD。
    // 打开编码器，应用上述所有参数
    const int result = avcodec_open2(codecContext, codec, &options); // result 为 0 表示按当前参数成功打开编码器。
    // 释放选项字典（avcodec_open2 会复制需要的值，原字典不再需要）
    av_dict_free(&options); // 释放字典本身及字符串，函数同时把指针置空。
    // 检查编码器是否成功打开
    if (result < 0) // FFmpeg 负返回值表示编码器参数或运行环境不支持。
    {
        if (error) // 调用者提供错误输出指针时，
            *error = QStringLiteral("H.264 编码器初始化失败：%1") // 构造错误前缀，
                         .arg(ffmpegError(result));                      // 填入 FFmpeg 对 result 的解释。
        releaseEncoder(); // 关闭并释放此次失败的上下文及帧、包。
        return false;     // 告知调用者初始化没有成功。
    }
    // 设置编码帧的像素格式
    codecFrame->format = codecContext->pix_fmt; // AVFrame 像素格式必须与编码上下文一致。
    // 设置编码帧的宽度
    codecFrame->width = codecContext->width; // AVFrame 宽度采用已经偶数对齐的编码宽度。
    // 设置编码帧的高度
    codecFrame->height = codecContext->height; // AVFrame 高度采用已经偶数对齐的编码高度。
    // 为编码帧分配数据缓冲区（32 字节对齐，提高内存访问效率）
    const int bufferResult = av_frame_get_buffer(codecFrame, 32); // 分配三个 YUV 平面的存储并按 32 字节对齐。
    if (bufferResult < 0)                                        // 负值表示分配失败。
    {
        if (error) // 调用者需要错误文本时，
            *error = QStringLiteral("H.264 帧缓冲区分配失败：%1") // 构造错误前缀，
                         .arg(ffmpegError(bufferResult));                // 填入具体 FFmpeg 内存错误。
        releaseEncoder(); // 回收已打开的编码器和部分帧资源。
        return false;     // 初始化失败。
    }
    // 重置编码器 PTS 为 0
    encoderPts = 0; // 新编码器第一帧的时间戳从 0 开始。
    return true;    // 上下文、帧、包和帧缓冲区均已就绪。
}

// 释放所有 FFmpeg 编码器资源（编码器上下文、帧、包、像素格式转换器）
void TcpFrameSender::releaseEncoder() // 释放全部 FFmpeg 资源，允许空指针并可重复调用。
{
    // 如果像素格式转换器存在，释放它
    if (scaleContext) // 像素转换上下文存在时，
        sws_freeContext(scaleContext); // 释放 libswscale 内部分配的表和缓存。
    // 将指针置空，防止悬空指针
    scaleContext = nullptr; // sws_freeContext 不负责置空，手动避免悬空指针。
    // 释放编码输出包（av_packet_free 同时释放包和将指针置空）
    av_packet_free(&codecPacket); // 释放输出包及其引用的数据，并把成员置空。
    // 释放编码输入帧
    av_frame_free(&codecFrame); // 释放输入帧及 av_frame_get_buffer 分配的 YUV 平面。
    // 释放编码器上下文
    avcodec_free_context(&codecContext); // 关闭编码器并释放上下文，同时把成员置空。
}

// 将 QImage 编码为 H.264 访问单元（Access Unit）
// @param image 输入原始 QImage 图像（支持 RGB16/RGB888 等格式）
// @param accessUnit 输出参数：编码后的 H.264 数据字节数组
// @param keyFrame 输出参数：指示当前帧是否为 IDR 关键帧
// @param error 输出参数：失败时接收错误描述
// @return true 编码成功，false 编码失败
bool TcpFrameSender::encodeFrame(const QImage &image, QByteArray *accessUnit, // image 是输入帧，accessUnit 接收编码字节。
                                 bool *keyFrame, QString *error) // keyFrame 接收关键帧标志，error 接收失败原因。
{
    // 根据320×240上限计算编码尺寸；当前640×480采集帧会缩小为320×240。
    const QSize encodedSize = encodedSizeFor(image); // encodedSize同时提供给H.264编码器和像素格式转换器。
    // 如果编码器未初始化或尺寸不匹配，则重新初始化编码器
    if (!codecContext || codecContext->width != encodedSize.width() || // 尚无编码器或宽度改变，或者
        codecContext->height != encodedSize.height())                  // 高度改变时必须重建 FFmpeg 状态。
        if (!initializeEncoder(encodedSize.width(), encodedSize.height(), error)) // 按当前帧目标尺寸初始化。
            return false;                                              // 初始化失败，错误已写入 error。

    // V4L2 采集帧本来就是 RGB565。直接让 swscale 读取这块内存，避免每帧先
    // 分配 RGB888 图像并执行一次 RGB565 -> RGB888 的全帧转换。
    // 转换后的图像（仅在输入格式非 RGB16/RGB888 时使用）
    QImage convertedImage; // 仅当输入不是 RGB565/RGB888 时保存临时 RGB888 转换结果。
    // 指向实际要转换的源图像指针
    const QImage *sourceImage = &image; // 默认直接从原始 QImage 的像素内存读取。
    // 源像素格式（FFmpeg AVPixelFormat 枚举值）
    AVPixelFormat sourceFormat = AV_PIX_FMT_NONE; // 稍后根据 QImage 格式设置 FFmpeg 源像素格式。
    // 判断输入图像的像素格式
    if (image.format() == QImage::Format_RGB16) // 摄像头常见的 RGB565 快速路径，无需 Qt 中间转换。
    {
        // RGB565 格式：根据系统字节序选择对应的 FFmpeg 像素格式
#if Q_BYTE_ORDER == Q_LITTLE_ENDIAN // 根据 CPU 内存中字节排列选择准确的 FFmpeg 格式。
        sourceFormat = AV_PIX_FMT_RGB565LE; // 小端序 RGB565
#else // 大端 CPU 使用 RGB565BE。
        sourceFormat = AV_PIX_FMT_RGB565BE; // 大端序 RGB565
#endif // 结束平台字节序选择。
    }
    else if (image.format() == QImage::Format_RGB888) // 已经是每像素连续 R、G、B 三字节时直接使用。
        // RGB888 格式：直接映射到 FFmpeg RGB24
        sourceFormat = AV_PIX_FMT_RGB24; // Qt RGB888 与 FFmpeg RGB24 的内存布局对应。
    else // 其他 QImage 格式先由 Qt 转成统一 RGB888。
    {
        // 不支持直接转换的格式：先转换为 RGB888 再处理
        convertedImage = image.convertToFormat(QImage::Format_RGB888); // 分配并生成兼容的临时图像。
        sourceImage = &convertedImage;                                 // 后续 swscale 从临时图像读取。
        sourceFormat = AV_PIX_FMT_RGB24;                               // 告知 swscale 当前内存为 RGB24。
    }

    // 检查源图像有效性及编码帧是否可写
    if (sourceImage->isNull() || av_frame_make_writable(codecFrame) < 0) // 图像转换失败或复用帧缓冲区无法取得写权限。
    {
        if (error) // 调用者提供错误输出位置时，
            *error = QStringLiteral("图像转换或编码帧缓冲区不可写"); // 写入本阶段统一错误说明。
        return false; // 不调用 sws_scale，避免访问无效内存。
    }
    // 获取或创建像素格式转换上下文（RGB → YUV420P）
    // sws_getCachedContext 会复用已有 context 如果参数未变，避免重复创建
    scaleContext = sws_getCachedContext( // 复用参数相同时的转换器，参数变化时自动重建。
        scaleContext,               // 现有的转换上下文（可为 nullptr）
        sourceImage->width(),       // 源图像宽度
        sourceImage->height(),      // 源图像高度
        sourceFormat,               // 源像素格式
        codecContext->width,        // 目标宽度（编码器宽度）
        codecContext->height,       // 目标高度（编码器高度）
        AV_PIX_FMT_YUV420P,         // 目标像素格式：YUV420P
        SWS_FAST_BILINEAR,          // 缩放算法：快速双线性插值
        nullptr, nullptr, nullptr); // 不使用额外的滤波器参数
    if (!scaleContext) // 空指针表示转换器参数不支持或内存分配失败。
    {
        if (error) // 调用者提供错误输出位置时，
            *error = QStringLiteral("FFmpeg 像素格式转换器初始化失败"); // 写入 swscale 初始化错误。
        return false; // 无转换器不能生成编码器需要的 YUV420P。
    }
    // 设置源数据指针数组：RGB 图像只有平面 0（打包格式），其余平面为空
    const uint8_t *sourceData[4] = {sourceImage->constBits(), nullptr, nullptr, nullptr}; // 打包 RGB 只有第 0 平面指向像素首地址。
    // 设置源行跨度数组：每一行的字节数
    int sourceLines[4] = {sourceImage->bytesPerLine(), 0, 0, 0}; // 第 0 项是 RGB 行跨度，其余未使用平面填 0。
    // 执行像素格式转换：RGB → YUV420P，同时完成缩放
    sws_scale(scaleContext, sourceData, sourceLines, 0, sourceImage->height(), // 从源图第 0 行转换全部高度，
              codecFrame->data, codecFrame->linesize);                       // 输出到 AVFrame 的 Y、U、V 平面及各自行跨度。
    // 设置编码帧的 PTS（显示时间戳）
    codecFrame->pts = encoderPts++; // 为本帧赋当前连续序号，然后为下一帧自增。
    // 事件录像可能在推流中途开始。此时强制产生一个新的 IDR；配合
    // repeat_headers，录像文件会从可独立解码的 SPS/PPS + IDR 开始。
    codecFrame->pict_type = forceNextKeyFrame // 录像刚开始时是否要求编码器立即生成帧内帧，
                                ? AV_PICTURE_TYPE_I // 是：显式请求 I 帧，
                                : AV_PICTURE_TYPE_NONE; // 否：让编码器按 GOP 自行选择帧类型。
    // 清空输出缓冲区
    accessUnit->clear(); // 清除调用者缓冲区中的旧帧数据，准备接收本帧全部包。
    // 初始化关键帧标志为 false
    *keyFrame = false; // 默认本帧不是关键帧，收到带 KEY 标志的包时再设为 true。

// 新版 FFmpeg API（send/receive 模式，libavcodec >= 57.37.100）
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(57, 37, 100) // 版本足够新时使用分离的 send/receive API。
    // 将原始帧送入编码器
    const int sent = avcodec_send_frame(codecContext, codecFrame); // sent 表示编码器是否接受本次 YUV 输入帧。
    if (sent < 0)                                                  // 负值表示输入提交失败。
    {
        if (error) // 需要错误说明时，
            *error = QStringLiteral("avcodec_send_frame: %1") // 标明失败 API，
                         .arg(ffmpegError(sent));               // 并填入 FFmpeg 错误文本。
        return false; // 输入帧未被接受，本次编码失败。
    }

    // 循环从编码器取出编码后的数据包（一帧可能产生多个 NAL 单元）
    while (true) // 持续取包，直到编码器告知本次暂无更多输出。
    {
        // 从编码器接收编码后的数据包
        const int result = avcodec_receive_packet(codecContext, codecPacket); // result 表示是否成功取得一个压缩包。
        // EAGAIN：编码器需要更多输入帧；EOF：编码器已刷新完毕
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) // 需要更多输入或编码器已结束时，
            break;                                             // 停止本帧的取包循环。
        if (result < 0)                                        // 其他负值是真正的编码错误。
        {
            if (error) // 调用者提供错误输出位置时，
                *error = QStringLiteral("avcodec_receive_packet: %1").arg(ffmpegError(result)); // 写入取包错误。
            return false; // 本帧输出不完整，不能发送或录像。
        }
        // 将编码数据追加到输出缓冲区
        accessUnit->append(reinterpret_cast<const char *>(codecPacket->data), codecPacket->size); // 按顺序拼接包内 H.264 字节。
        // 检查是否为关键帧（IDR 帧），关键帧标志可取或（可能多个 NAL 单元中任一个是关键帧）
        *keyFrame = *keyFrame || ((codecPacket->flags & AV_PKT_FLAG_KEY) != 0); // 任一输出包带 KEY 标志即认定访问单元为关键帧。
        // 释放当前编码包的引用（减少内部引用计数，避免内存泄漏）
        av_packet_unref(codecPacket); // 释放当前包引用，使同一个 AVPacket 可接收下一个输出。
    }
#else // 旧版 libavcodec 使用一次调用同时送帧和取包的 API。
    // FFmpeg 3.0 及更早版本（i.MX6ULL SDK 为 libavcodec 57.24.102）
    // 尚未提供 send/receive API，一帧输入通过旧接口取得至多一个编码包。
    // 先释放编码包中可能存在的旧数据引用
    av_packet_unref(codecPacket); // 清除复用包中上一次编码留下的数据引用。
    // 是否获取到编码包标志（输出参数）
    int gotPacket = 0; // 旧 API 通过此输出整数告知本次是否产生压缩包。
    // 旧式编码 API：avcodec_encode_video2，一帧输入一次调用
    const int result = avcodec_encode_video2(codecContext, codecPacket, // 提交 YUV 帧并复用 codecPacket 接收输出，
                                             codecFrame, &gotPacket);   // gotPacket 接收是否有有效输出。
    if (result < 0)                                                     // 负值表示旧编码 API 失败。
    {
        if (error) // 调用者需要错误文本时，
            *error = QStringLiteral("avcodec_encode_video2: %1") // 标明旧 API 名称，
                         .arg(ffmpegError(result));                 // 并填入 FFmpeg 错误说明。
        return false; // 本帧编码失败。
    }
    // 如果编码器产生了输出数据
    if (gotPacket) // 编码器本次确实输出了压缩包。
    {
        // 将编码数据追加到输出缓冲区
        accessUnit->append( // 将旧 API 产生的唯一数据包复制到输出字节数组，
            reinterpret_cast<const char *>(codecPacket->data), // 把 uint8_t* 解释为 QByteArray 接受的 char*，
            codecPacket->size);                                // 复制包中有效字节数。
        // 判断是否为关键帧
        *keyFrame = (codecPacket->flags & AV_PKT_FLAG_KEY) != 0; // 根据包标志设置本帧关键帧结果。
    }
    // 释放编码包引用
    av_packet_unref(codecPacket); // 无论是否产包都清理复用包引用。
#endif // 结束新旧 FFmpeg 编码 API 条件分支。
    // 编码后检查输出缓冲区是否为空（理论上不应发生）
    if (accessUnit->isEmpty()) // 低延迟配置预期每个输入帧都有输出，空结果视为失败。
    {
        if (error) // 提供了错误输出参数时，
            *error = QStringLiteral("编码器未产生 H.264 输出数据"); // 写入空输出说明。
        return false; // 空访问单元不能发送或写盘。
    }
    // 只有编码器确实输出关键帧后才清除请求，避免录像从 P 帧开始。
    if (forceNextKeyFrame && *keyFrame) // 强制请求已经由实际关键帧满足时，
        forceNextKeyFrame = false;      // 清除请求，后续恢复正常 GOP 帧型选择。
    return true;                        // accessUnit 和 keyFrame 输出均有效。
}

// 通过 RTP/UDP 协议发送 H.264 访问单元（将大数据分包发送）
// @param accessUnit H.264 编码数据（访问单元）
// @param keyFrame 指示当前帧是否为关键帧
// @param timestampUs 时间戳（微秒）
// @return true 发送成功，false 条件不满足或发送失败
bool TcpFrameSender::sendAccessUnit(const QByteArray &accessUnit, bool keyFrame, // accessUnit 是一帧 H.264，keyFrame 标识是否关键帧。
                                    quint64 timestampUs) // timestampUs 是写入所有分片头的微秒时间戳。
{
    // 前置条件检查：UDP socket 必须存在、推流已启用、当前为 UDP 模式、数据非空
    if (!udpSocket || !sendingEnabled || tcpTransport || accessUnit.isEmpty()) // socket、开关、协议和载荷任一不满足即不能发 UDP。
        return false;                                                          // 不修改任何序号。
    // 计算需要的 RTP 分包数（向上取整）
    const int count = (accessUnit.size() + VideoRtp::MaximumPayloadSize - 1) / // 加“除数-1”实现整数向上取整，
                      VideoRtp::MaximumPayloadSize;                            // 得到本帧需要的 UDP 分片总数。
    // 分包数有效性检查：0 或超过最大分包限制则失败
    if (count <= 0 || count > VideoRtp::MaximumFragmentsPerFrame) // 分片数必须非零且不超过协议头可表达的安全上限。
        return false;                                                // 帧过大时直接丢弃，防止接收端无法重组。
    // 逐分包发送
    for (int index = 0; index < count; ++index) // index 是当前分片在本帧内的 0 起始序号。
    {
        // 当前分包的起始偏移
        const int offset = index * VideoRtp::MaximumPayloadSize; // offset 是当前分片在访问单元中的起始字节位置。
        // 当前分包的载荷大小（最后一个分包可能小于最大载荷）
        const int size = qMin(VideoRtp::MaximumPayloadSize, accessUnit.size() - offset); // 最后一片只取剩余有效字节。
        // 构造 RTP 分包头部
        VideoRtp::Header header; // header 保存接收端重组和统计所需的固定字段。
        // 设置标志位：最后一个分包设置 MarkerFlag；关键帧设置 KeyFrameFlag
        header.flags = (index + 1 == count ? VideoRtp::MarkerFlag : 0) | // 最后一片设置帧结束标志，
                       (keyFrame ? VideoRtp::KeyFrameFlag : 0);           // 关键帧的所有分片设置关键帧标志。
        // 设置流 ID（用于接收端区分不同的推流会话）
        header.streamId = streamId; // 同一推流配置期间的所有包使用相同会话 ID。
        // 设置帧序列号（接收端可检测丢帧）
        header.frameSequence = frameSequence; // 同一帧的全部分片共享相同帧序号。
        // 设置包序列号并递增
        header.packetSequence = packetSequence++; // 每个 UDP 数据报使用全局连续包序号，然后自增。
        // 当前分包在帧中的索引
        header.fragmentIndex = quint16(index); // 写入当前分片编号，接收端据此排列载荷。
        // 帧被拆分成的总分包数
        header.fragmentCount = quint16(count); // 写入本帧总分片数，接收端据此判断是否收齐。
        // 发送时间戳（微秒）
        header.sendTimestampUs = timestampUs; // 所有分片共享原始帧发送时间戳。
        // 调用 RTP 工具函数构造完整的数据报（头 + 载荷）
        const QByteArray datagram = VideoRtp::makeDatagram( // datagram 是序列化后的协议头与当前载荷副本，
            header, accessUnit.constData() + offset, size);  // 输入载荷从 offset 开始，共 size 字节。
        // 通过 UDP 套接字发送数据报到目标地址
        const qint64 written = udpSocket->writeDatagram(datagram, serverAddress, serverPort); // written 是本数据报提交字节数。
        // 检查是否完整写入
        if (written != datagram.size()) // UDP 必须一次完整写入一个数据报，短写或负值均为失败。
            return false;               // 停止发送本帧余下分片。
    }
    // 帧序列号递增（每帧发送完成后递增一次）
    ++frameSequence; // 全部分片成功提交后再进入下一帧序号。
    return true;     // 告知调用者整帧 UDP 分片发送成功。
}

// 将已经编码好的 H.264 Annex B 访问单元写入录像文件
// @param accessUnit 一帧 H.264 编码数据
// @return true 写入成功或无录像文件（跳过），false 写入失败
bool TcpFrameSender::writeRecordingFrame(const QByteArray &accessUnit) // accessUnit 是要原样追加到裸 H.264 文件的一帧 Annex B 数据。
{
    // 如果录像文件未打开，无需写入，返回 true（不阻断推流流程）
    if (!recordingFile || !recordingFile->isOpen()) // 没有活动录像时，本函数作为无操作成功。
        return true;                                // 不让“未录像”影响实时推流。
    if (accessUnit.isEmpty() ||                     // 空编码帧不能写入；或者
        recordingFile->write(accessUnit) != accessUnit.size()) // 实际写入字节数不等于整帧长度时视为失败。
    {
        // 写入失败：发送错误信号
        emit recordingError(QStringLiteral("H.264 录像写入失败：%1") // 构造录像错误通知，
                                .arg(recordingFile->errorString()));    // 填入 QFile 提供的文件系统原因。
        // 写入失败时自动停止录像
        stopRecording(); // 关闭当前文件，防止后续帧继续写入损坏状态。
        return false;    // 告诉 sendFrame() 本次写盘失败。
    }
    return true; // 当前帧已经完整写入录像文件。
}

// 核心帧处理函数：接收图像 → 编码 → 发送的完整流水线入口
// @param image 来自摄像头采集的原始 QImage 帧
void TcpFrameSender::sendFrame(const QImage &image) // image 是采集线程通过队列连接送来的独立 QImage 帧。
{
    // 检查是否正在进行录像（文件已打开写入）
    const bool recording = recordingFile && recordingFile->isOpen(); // recording 快照表示本帧到达时是否有活动录像文件。
    // 如果图像为空，或推流和录像都不需要处理，则丢弃此帧
    if (image.isNull() || (!sendingEnabled && !recording)) // 空图像或网络、录像都不需要本帧时直接跳过编码。
    {
        return; // 避免无意义地占用单核 CPU。
    }
    // 获取当前时间的毫秒级时间戳（用于接收端的帧同步）
    const qint64 timestampMs = QDateTime::currentMSecsSinceEpoch(); // 保存当前 Unix 毫秒时间，供 TCP/RTP 头携带。
    // 编码后的 H.264 数据缓冲区
    QByteArray encodedFrame; // 接收 encodeFrame() 产生的一帧 H.264 Annex B 字节。
    // 关键帧标志（输出参数）
    bool keyFrame = false; // 接收编码输出是否为 IDR/关键帧。
    // 编码错误描述（输出参数）
    QString error; // 接收 FFmpeg 初始化、转换或编码失败的可读原因。
    // 调用编码函数：将 QImage 编码为 H.264 访问单元
    const bool encoded = encodeFrame(image, &encodedFrame, &keyFrame, &error); // 一次编码同时供网络和录像复用。
    // 编码失败处理
    if (!encoded) // 编码失败时没有合法载荷可以继续处理。
    {
        // 发送编码失败状态信息
        emit statusChanged(QStringLiteral("H.264 编码失败：%1").arg(error)); // 向 UI 报告编码链路错误。
        if (recording) // 活动录像也依赖同一个编码结果，因此必须同步结束。
        {
            emit recordingError(QStringLiteral("H.264 录像编码失败：%1").arg(error)); // 发出录像专用错误信号。
            stopRecording(); // 关闭当前文件并发出录像停止状态。
        }
        return; // 本帧处理到此结束。
    }
    // 同一个 H.264 编码结果直接写入录像，避免录像和推流重复编码。
    if (recording) // 本帧到达时录像文件处于打开状态，
        writeRecordingFrame(encodedFrame); // 直接复用已编码访问单元写盘，不重复调用编码器。
    // 仅录像时写盘完成即可，不进入网络发送路径。
    if (!sendingEnabled) // 当前只有录像需求而没有实时网络推流时，
        return;          // 写盘后结束，避免访问网络 socket。
    // 根据传输模式选择发送方式：
    // TCP 模式 → sendTcpFrame    UDP 模式 → sendAccessUnit (RTP/UDP)
    const bool sent = tcpTransport // sent 保存所选协议是否完整接受本帧，
                          ? sendTcpFrame(encodedFrame, codecContext->width, // TCP 使用完整访问单元和编码宽度，
                                         codecContext->height, timestampMs) // 同时携带编码高度及毫秒时间戳；
                          : sendAccessUnit(encodedFrame, keyFrame,           // UDP 使用访问单元和关键帧标志，
                                           quint64(timestampMs) * 1000); // 毫秒转微秒
    // 发送失败处理
    if (!sent) // 网络函数拒绝或无法提交本帧时更新状态，但不停止后续帧处理。
    {
        // 根据传输模式发送不同的错误状态信息
        if (tcpTransport) // TCP 模式根据当前连接状态区分拥塞与未连接。
            // TCP 模式：区分是"已连接但拥塞"还是"未连接"
            emit statusChanged(tcpSocket && tcpSocket->state() == QAbstractSocket::ConnectedState // socket 已连接却发送失败时，
                                   ? QStringLiteral("H.264/TCP 发送拥塞或失败：%1") // 使用拥塞/发送错误模板，
                                         .arg(tcpSocket->errorString())             // 填入 socket 具体错误；
                                   : QStringLiteral("H.264/TCP 尚未连接，当前帧已丢弃")); // 否则说明仍在重连。
        else // UDP 模式没有连接状态，只报告 writeDatagram 的 socket 错误。
            // UDP 模式：显示 UDP 错误信息
            emit statusChanged(QStringLiteral("RTP/UDP 发送失败：%1") // 构造 UDP 错误文本，
                                   .arg(udpSocket ? udpSocket->errorString() // socket 存在时读取其错误，
                                                  : QStringLiteral("socket 未初始化"))); // 否则指出未初始化。
    }
}

// 开始录像：将视频帧编码成 H.264 Annex B 裸码流写入文件
// @param filePath 录像文件的完整保存路径
// @param durationMs 自动事件录像持续时间（毫秒），必须大于 0
void TcpFrameSender::startRecording(const QString &filePath, int durationMs) // filePath 是目标文件，durationMs 是自动停止毫秒数。
{
    if (durationMs <= 0) // 非正持续时间无法形成有效定时录像。
    {
        emit recordingError(QStringLiteral("自动事件录像时长必须大于 0")); // 向主线程报告参数错误。
        return;                                                         // 不初始化或打开文件。
    }
    // 确保网络和文件资源已初始化
    if (!recordingFile) // 发送器尚未执行 initialize() 时，
        initialize();   // 创建 QFile 和录像停止定时器等公共资源。
    // 如果文件已打开（可能已在录像中）
    if (recordingFile->isOpen()) // 已经录像时不更换文件，只延长当前事件录像时间。
    {
        recordingStopTimer->start(durationMs); // 重新启动单次定时器，从现在起再录 durationMs。
        return;                                // 保持原 activeRecordingPath 和文件句柄。
    }
    // 创建录像文件所在的父目录（如果不存在）
    QDir().mkpath(QFileInfo(filePath).absolutePath()); // 递归创建目标文件的父目录；已存在时无副作用。
    // 设置录像文件的路径
    recordingFile->setFileName(filePath); // 将调用者提供的完整路径绑定到复用 QFile 对象。
    // 以只写+截断模式打开文件（覆盖已有内容）
    if (!recordingFile->open(QIODevice::WriteOnly | QIODevice::Truncate)) // 以只写方式打开，并覆盖同名旧文件。
    {
        // 文件打开失败：发送错误信号
        emit recordingError(QStringLiteral("无法创建录像文件：%1").arg(recordingFile->errorString())); // 反馈权限、目录或磁盘错误。
        return; // 文件未打开，不进入录像状态。
    }
    // 记录当前活跃的录像文件路径
    activeRecordingPath = filePath; // 保存当前文件路径，结束录像时通过信号原样返回。
    // 即使编码器正在用于网络推流，也让录像从新的关键帧开始。
    forceNextKeyFrame = true; // 请求下一个编码帧为 I/IDR，使新文件能独立开始解码。
    recordingStopTimer->start(durationMs); // 到期后自动调用 stopRecording()。
    // 发送录像开始状态信号
    emit recordingStateChanged(true, filePath); // 通知页面录像已成功开始及实际路径。
}

// 停止录像：关闭录像文件并发送状态变更信号
void TcpFrameSender::stopRecording() // 安全结束当前文件写入，并反馈完成路径。
{
    // 停止录像停止定时器
    if (recordingStopTimer) // 定时器已创建时，
        recordingStopTimer->stop(); // 取消尚未触发的自动停止，避免重复回调。
    // 如果文件未打开，无需执行关闭操作
    if (!recordingFile || !recordingFile->isOpen()) // 无文件对象或文件未打开表示当前没有活动录像。
        return;                                      // 保持幂等，多次停止不会重复发信号。
    // 保存文件路径（close 后 activeRecordingPath 会被清空）
    const QString path = activeRecordingPath; // 在清空成员前保存完成文件路径供信号使用。
    // 强制将缓冲区数据写入磁盘
    recordingFile->flush(); // 请求把 Qt 用户态缓冲数据写向操作系统。
    // 关闭录像文件
    recordingFile->close(); // 关闭文件描述符，完成本次裸 H.264 文件写入。
    // 清空活跃录像路径（表示当前无录像进行中）
    activeRecordingPath.clear(); // 空路径表示发送器当前不处于录像状态。
    // 发送录像停止状态信号
    emit recordingStateChanged(false, path); // 通知页面录像完成，并提供可保存/播放的路径。
}

// 停止所有操作：推流、录像、编码器、网络连接
void TcpFrameSender::stop() // 页面析构时在网络线程同步调用的总资源停止入口。
{
    // 禁用推流标志
    sendingEnabled = false; // 先禁止 sendFrame() 继续进入网络发送分支。
    // 停止 TCP 重连定时器
    if (tcpReconnectTimer) // 定时器已创建时，
        tcpReconnectTimer->stop(); // 禁止停止过程中再次发起 TCP 连接。
    // 停止录像（如果正在进行中）
    stopRecording(); // 刷新并关闭可能仍在写入的录像文件。
    // 释放编码器资源
    releaseEncoder(); // 释放全部 FFmpeg 上下文和缓冲区。
    // 中止 TCP 连接（如果有）
    if (tcpSocket) // TCP socket 已创建时，
        tcpSocket->abort(); // 立即断开连接并丢弃尚未发送的缓冲数据。
    // 关闭 UDP 套接字（如果有）
    if (udpSocket) // UDP socket 已创建时，
        udpSocket->close(); // 关闭底层描述符并停止数据报发送。
}
