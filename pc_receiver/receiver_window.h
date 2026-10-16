// 防止头文件被重复包含的头文件保护宏
#ifndef RECEIVER_WINDOW_H // 如果尚未包含本声明，才编译下面内容。
#define RECEIVER_WINDOW_H // 定义保护宏，避免主窗口重复声明。

// 引入 H.264 解码器类，用于将 H.264 码流解码为 QImage
#include "h264_decoder.h" // 主窗口按值持有 H264Decoder，因此需要完整类型。
// 引入 RTP 帧重组器类，用于将 RTP 分包重组为完整帧
#include "rtp_reassembler.h" // 主窗口按值持有 Reassembler 并使用 CompleteFrame。

// Qt 高精度计时器，用于统计更新周期计时
#include <QElapsedTimer> // 统计周期和单帧解码耗时使用单调时钟。
// Qt 字节数组类，用于缓存 TCP 接收到的原始数据
#include <QByteArray> // 保存 TCP 字节流缓存和 MQTT 消息体。
// Qt IP 地址封装类，记录推流设备的 IP 地址
#include <QHostAddress> // 保存当前视频发送端 IP。
// Qt 图像类，存储解码后的视频帧
#include <QImage> // 保存最后一张已解码图像。
// Qt JSON 对象类，用于构造 MQTT 命令和解析设备状态消息
#include <QJsonObject> // 构造远程命令及附加参数。
// Qt 窗口基类，ReceiverWindow 继承自 QWidget
#include <QWidget> // ReceiverWindow 的 Qt 页面基类。

// 前向声明：事件录像归档浏览器控件
class ArchiveBrowser; // 前置声明事件录像归档浏览器类。
// 前向声明：Qt 标签控件
class QLabel; // 前置声明文本与图像标签控件类。
// 前向声明：Qt 单行文本编辑框
class QLineEdit; // 前置声明单行文本输入控件类。
// 前向声明：Qt 列表控件
class QListWidget; // 前置声明列表控件类。
// 前向声明：Qt 按钮控件
class QPushButton; // 前置声明按钮控件类。
// 前向声明：Qt 窗口大小调整事件
class QResizeEvent; // 前置声明窗口尺寸变化事件类。
// 前向声明：Qt 数值输入框
class QSpinBox; // 前置声明整数输入控件类。
// 前向声明：Qt 定时器
class QTimer; // 前置声明定时器类。
// 前向声明：Qt TCP 服务器（监听推流连接）
class QTcpServer; // 前置声明TCP监听服务器类。
// 前向声明：Qt TCP 套接字（与推流设备通信）
class QTcpSocket; // 前置声明TCP连接套接字类。
// 前向声明：Qt UDP 套接字（接收 RTP/UDP 数据报）
class QUdpSocket; // 前置声明UDP数据报套接字类。
// 前向声明：Qt MQTT 客户端（远程控制与状态订阅）
class QMqttClient; // 前置声明MQTT客户端类。

// PC 端监控窗口类：集成视频接收（TCP/UDP）、MQTT 远程控制、录像管理功能
// final 关键字防止被继承
class ReceiverWindow final : public QWidget // 整合视频接收、解码显示、MQTT 和归档页面。
{
    // Qt 元对象系统宏，使该类支持信号与槽机制
    Q_OBJECT // 启用 Qt 元对象和 private slots。
public: // 主窗口构造接口。
    // 构造函数
    // @param parent 父窗口指针，默认为 nullptr
    explicit ReceiverWindow(QWidget *parent = nullptr); // parent 是可选父窗口，通常为空。

protected: // QWidget 事件重写区域。
    // 窗口大小变化事件处理（重写基类虚函数）
    // @param event 窗口大小调整事件对象
    void resizeEvent(QResizeEvent *event) override; // event 是尺寸变化事件，用于重绘 lastFrame。

private slots: // 由按钮、socket、MQTT 和定时器信号触发的槽函数。
    // 切换 TCP/UDP 监听状态（启动/停止）
    void toggleListening(); // 同时启动或停止同端口 TCP 与 UDP 监听。
    // 读取并处理所有待处理的 UDP 数据报（RTP/UDP 模式）
    void readPendingDatagrams(); // 解析并重组当前所有待收 UDP 数据报。
    // 接受新的 TCP 推流客户端连接
    void acceptTcpConnection(); // 接受最新 TCP 客户端并替换旧视频连接。
    // 读取并解析 TCP 帧数据（自定义 28 字节帧头协议）
    void readTcpFrames(); // 从字节流缓存解析一个或多个完整自定义帧。
    // TCP 客户端断开连接时调用的槽函数
    void tcpClientDisconnected(); // 清理已断开的当前客户端。
    // 定时器触发：清理超时未完成重组的 RTP 帧
    void expireIncompleteFrames(); // 让重组器清理超过等待期限的帧。
    // 切换 MQTT 连接状态（连接/断开）
    void toggleMqttConnection(); // 根据当前状态连接或断开 MQTT Broker。

private: // 视频、MQTT 辅助函数及全部运行状态。
    // 处理完整帧列表：解码 H.264 并显示到界面
    // @param frames 重组器产生的一系列完整帧（可能为空）
    void handleCompleteFrames(const QVector<VideoRtp::CompleteFrame> &frames); // frames 是本次新产出的有序完整帧。

    // 重置视频会话状态（解码器 + RTP 重组器 + 流状态）
    void resetVideoSession(); // 清除 UDP 重组与解码同步状态。

    // 重置 TCP 会话状态（清空接收缓冲区和释放客户端）
    void resetTcpSession(); // 清空 TCP 缓冲并延迟释放客户端对象。

    // 在界面上显示解码后的视频帧
    // @param image 解码后的 QImage 视频帧
    void showFrame(const QImage &image); // image 是已解码 RGB 帧。

    // 更新 FPS、带宽、丢帧等统计信息（每 1 秒更新一次显示）
    // @param sequence 当前帧的帧序列号
    // @param localLatencyMs 本地处理延迟（解码耗时，毫秒）
    // @param endToEndLatencyMs 端到端延迟（毫秒），-1 表示时钟未同步
    void updateStatistics(quint32 sequence, qint64 localLatencyMs, // sequence 是帧号，localLatencyMs 是本地耗时。
                          qint64 endToEndLatencyMs); // 端到端延迟无效时为 -1。

    // 订阅设备相关的 MQTT 主题（status、events、command_ack）
    void subscribeDeviceTopics(); // 订阅当前 deviceId 的状态、事件和命令回执。

    // 处理接收到的 MQTT 消息（设备状态更新或事件/命令回执）
    // @param message MQTT 消息体（JSON 格式字节数组）
    // @param topic MQTT 主题字符串
    void handleMqttMessage(const QByteArray &message, const QString &topic); // message 为 JSON 字节，topic 决定解析用途。

    // 通过 MQTT 向设备发布远程控制命令
    // @param command 命令字符串（如 "start_capture"、"stream_on" 等）
    // @param arguments 附加参数（JSON 对象，默认为空）
    void publishCommand(const QString &command, // command 是设备端支持的命令名。
                        const QJsonObject &arguments = QJsonObject()); // arguments 是可选附加 JSON 字段。

    // 根据后缀生成完整的设备 MQTT 主题路径
    // @param suffix 主题后缀（如 "status", "events", "commands"）
    // @return 完整的 MQTT 主题字符串，格式为 "devices/{deviceId}/{suffix}"
    QString deviceTopic(const QString &suffix) const; // suffix 是 status/events/commands 等末级主题。

    // ==================== 网络/视频 UI 成员 ====================
    // UDP 套接字，用于接收 RTP/UDP 视频数据报
    QUdpSocket *videoSocket; // 绑定监听端口并接收视频 UDP 数据报。
    // TCP 服务器，用于监听 i.MX6ULL 设备发起的 TCP 推流连接
    QTcpServer *tcpServer; // 在同一端口监听设备 TCP 推流连接。
    // 活跃的 TCP 客户端套接字（当前连接的推流设备）
    QTcpSocket *tcpClient; // 当前唯一活动 TCP 推流连接，可为空。
    // TCP 接收缓冲区：缓存从 TCP 流中读取的原始字节，用于帧边界解析
    QByteArray tcpReceiveBuffer; // 跨 readyRead 保存未解析完整帧的 TCP 字节。
    // 帧重组定时器：周期触发 expireIncompleteFrames()，清理超时不完整帧
    QTimer *reassemblyTimer; // 每 20ms 驱动一次 UDP 超时清理。
    // 监听端口号输入框
    QSpinBox *portSpinBox; // 输入 TCP/UDP 共用的监听端口。
    // 开始/停止监听切换按钮
    QPushButton *listenButton; // 切换监听启停状态。
    // 连接状态显示标签
    QLabel *statusLabel; // 显示监听、连接和解码错误状态。
    // FPS/带宽/延迟统计信息显示标签
    QLabel *statisticsLabel; // 每秒显示 FPS、带宽、丢包与延迟。
    // 视频帧显示标签
    QLabel *frameLabel; // 显示缩放后的实时解码画面。
    // 事件录像归档浏览器控件
    ArchiveBrowser *archiveBrowser; // 第二标签页的录像下载与播放页面。

    // ==================== MQTT 相关成员 ====================
    // MQTT 客户端对象，用于远程控制和状态订阅
    QMqttClient *mqttClient; // 订阅设备消息并发布远程控制命令。
    // MQTT 服务器地址输入框
    QLineEdit *mqttHostEdit; // MQTT Broker 主机名或 IP 输入框。
    // MQTT 服务器端口输入框
    QSpinBox *mqttPortSpinBox; // MQTT Broker 端口输入框。
    // 目标设备 ID 输入框
    QLineEdit *deviceIdEdit; // 构造 devices/<id>/... 主题的设备 ID。
    // MQTT 连接/断开切换按钮
    QPushButton *mqttConnectButton; // 连接或断开 MQTT。
    // MQTT 连接状态显示标签
    QLabel *mqttStatusLabel; // 显示 MQTT 当前连接状态。
    // 设备状态显示标签（从 MQTT status 主题解析）
    QLabel *deviceStatusLabel; // 显示设备心跳中的开关和时间信息。
    // 设备事件/命令回执列表控件
    QListWidget *eventList; // 按新到旧显示事件和命令执行回执。

    // ==================== 视频处理成员 ====================
    // RTP 帧重组器：将 RTP 分包重组为完整 H.264 帧
    VideoRtp::Reassembler reassembler; // UDP 分片乱序缓存和完整帧产出器。
    // H.264 解码器：将 H.264 码流解码为 QImage
    H264Decoder decoder; // TCP 与 UDP 链路共同复用的 FFmpeg 解码器。
    // 缓存最后一帧图像（用于窗口缩放时重新渲染）
    QImage lastFrame; // 最近成功解码帧，窗口缩放时重新显示。
    // 统计周期计时器：控制 FPS/带宽统计每 1 秒更新一次
    QElapsedTimer statisticsTimer; // 统计累计值达到约 1 秒时刷新 UI。

    // ==================== 统计计数器 ====================
    // 统计周期内的累计接收字节数
    quint64 intervalBytes; // 当前统计周期接收的网络字节总数。
    // 统计周期内的累计帧数
    quint32 intervalFrames; // 当前统计周期成功解码的帧数。
    // 最后处理的帧序列号
    quint32 lastSequence; // 最近成功显示的帧序号。
    // 当前活跃的 RTP 流 ID（用于检测流切换）
    quint32 activeStreamId; // 当前 UDP 会话 ID。

    // ==================== 状态标志 ====================
    // 是否存在活跃的 RTP 流
    bool hasActiveStream; // 是否已建立 UDP streamId 和 peer 基准。
    // 是否正在等待关键帧（IDR 帧到达后才开始解码）
    bool waitingForKeyFrame; // 丢帧后为 true，忽略 P 帧直到关键帧。
    // 当前是否处于 TCP 传输模式（false 表示 UDP/RTP 模式）
    bool activeTcpTransport; // true 表示当前统计和解码输入来自 TCP。
    // 当前活跃的对端（推流设备）IP 地址
    QHostAddress activePeer; // 当前视频设备 IP，用于检测 UDP 会话切换。
};

// 头文件保护宏结束
#endif // RECEIVER_WINDOW_H：结束头文件保护。
