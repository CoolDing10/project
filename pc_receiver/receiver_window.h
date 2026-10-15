// 防止头文件被重复包含的头文件保护宏
#ifndef RECEIVER_WINDOW_H
#define RECEIVER_WINDOW_H

// 引入 H.264 解码器类，用于将 H.264 码流解码为 QImage
#include "h264_decoder.h"
// 引入 RTP 帧重组器类，用于将 RTP 分包重组为完整帧
#include "rtp_reassembler.h"

// Qt 高精度计时器，用于统计更新周期计时
#include <QElapsedTimer>
// Qt 字节数组类，用于缓存 TCP 接收到的原始数据
#include <QByteArray>
// Qt IP 地址封装类，记录推流设备的 IP 地址
#include <QHostAddress>
// Qt 图像类，存储解码后的视频帧
#include <QImage>
// Qt JSON 对象类，用于构造 MQTT 命令和解析设备状态消息
#include <QJsonObject>
// Qt 窗口基类，ReceiverWindow 继承自 QWidget
#include <QWidget>

// 前向声明：抓拍与录像归档浏览器控件
class ArchiveBrowser;
// 前向声明：Qt 标签控件
class QLabel;
// 前向声明：Qt 单行文本编辑框
class QLineEdit;
// 前向声明：Qt 列表控件
class QListWidget;
// 前向声明：Qt 按钮控件
class QPushButton;
// 前向声明：Qt 窗口大小调整事件
class QResizeEvent;
// 前向声明：Qt 数值输入框
class QSpinBox;
// 前向声明：Qt 定时器
class QTimer;
// 前向声明：Qt TCP 服务器（监听推流连接）
class QTcpServer;
// 前向声明：Qt TCP 套接字（与推流设备通信）
class QTcpSocket;
// 前向声明：Qt UDP 套接字（接收 RTP/UDP 数据报）
class QUdpSocket;
// 前向声明：Qt MQTT 客户端（远程控制与状态订阅）
class QMqttClient;

// PC 端监控窗口类：集成视频接收（TCP/UDP）、MQTT 远程控制、录像管理功能
// final 关键字防止被继承
class ReceiverWindow final : public QWidget
{
    // Qt 元对象系统宏，使该类支持信号与槽机制
    Q_OBJECT
public:
    // 构造函数
    // @param parent 父窗口指针，默认为 nullptr
    explicit ReceiverWindow(QWidget *parent = nullptr);

protected:
    // 窗口大小变化事件处理（重写基类虚函数）
    // @param event 窗口大小调整事件对象
    void resizeEvent(QResizeEvent *event) override;

private slots:
    // 切换 TCP/UDP 监听状态（启动/停止）
    void toggleListening();
    // 读取并处理所有待处理的 UDP 数据报（RTP/UDP 模式）
    void readPendingDatagrams();
    // 接受新的 TCP 推流客户端连接
    void acceptTcpConnection();
    // 读取并解析 TCP 帧数据（自定义 28 字节帧头协议）
    void readTcpFrames();
    // TCP 客户端断开连接时调用的槽函数
    void tcpClientDisconnected();
    // 定时器触发：清理超时未完成重组的 RTP 帧
    void expireIncompleteFrames();
    // 切换 MQTT 连接状态（连接/断开）
    void toggleMqttConnection();

private:
    // 处理完整帧列表：解码 H.264 并显示到界面
    // @param frames 重组器产生的一系列完整帧（可能为空）
    void handleCompleteFrames(const QVector<VideoRtp::CompleteFrame> &frames);

    // 重置视频会话状态（解码器 + RTP 重组器 + 流状态）
    void resetVideoSession();

    // 重置 TCP 会话状态（清空接收缓冲区和释放客户端）
    void resetTcpSession();

    // 在界面上显示解码后的视频帧
    // @param image 解码后的 QImage 视频帧
    void showFrame(const QImage &image);

    // 更新 FPS、带宽、丢帧等统计信息（每 1 秒更新一次显示）
    // @param sequence 当前帧的帧序列号
    // @param localLatencyMs 本地处理延迟（解码耗时，毫秒）
    // @param endToEndLatencyMs 端到端延迟（毫秒），-1 表示时钟未同步
    void updateStatistics(quint32 sequence, qint64 localLatencyMs,
                          qint64 endToEndLatencyMs);

    // 订阅设备相关的 MQTT 主题（status、events、command_ack）
    void subscribeDeviceTopics();

    // 处理接收到的 MQTT 消息（设备状态更新或事件/命令回执）
    // @param message MQTT 消息体（JSON 格式字节数组）
    // @param topic MQTT 主题字符串
    void handleMqttMessage(const QByteArray &message, const QString &topic);

    // 通过 MQTT 向设备发布远程控制命令
    // @param command 命令字符串（如 "start_capture", "snapshot" 等）
    // @param arguments 附加参数（JSON 对象，默认为空）
    void publishCommand(const QString &command,
                        const QJsonObject &arguments = QJsonObject());

    // 根据后缀生成完整的设备 MQTT 主题路径
    // @param suffix 主题后缀（如 "status", "events", "commands"）
    // @return 完整的 MQTT 主题字符串，格式为 "devices/{deviceId}/{suffix}"
    QString deviceTopic(const QString &suffix) const;

    // ==================== 网络/视频 UI 成员 ====================
    // UDP 套接字，用于接收 RTP/UDP 视频数据报
    QUdpSocket *videoSocket;
    // TCP 服务器，用于监听 i.MX6ULL 设备发起的 TCP 推流连接
    QTcpServer *tcpServer;
    // 活跃的 TCP 客户端套接字（当前连接的推流设备）
    QTcpSocket *tcpClient;
    // TCP 接收缓冲区：缓存从 TCP 流中读取的原始字节，用于帧边界解析
    QByteArray tcpReceiveBuffer;
    // 帧重组定时器：周期触发 expireIncompleteFrames()，清理超时不完整帧
    QTimer *reassemblyTimer;
    // 监听端口号输入框
    QSpinBox *portSpinBox;
    // 开始/停止监听切换按钮
    QPushButton *listenButton;
    // 连接状态显示标签
    QLabel *statusLabel;
    // FPS/带宽/延迟统计信息显示标签
    QLabel *statisticsLabel;
    // 视频帧显示标签
    QLabel *frameLabel;
    // 抓拍与录像归档浏览器控件
    ArchiveBrowser *archiveBrowser;

    // ==================== MQTT 相关成员 ====================
    // MQTT 客户端对象，用于远程控制和状态订阅
    QMqttClient *mqttClient;
    // MQTT 服务器地址输入框
    QLineEdit *mqttHostEdit;
    // MQTT 服务器端口输入框
    QSpinBox *mqttPortSpinBox;
    // 目标设备 ID 输入框
    QLineEdit *deviceIdEdit;
    // MQTT 连接/断开切换按钮
    QPushButton *mqttConnectButton;
    // MQTT 连接状态显示标签
    QLabel *mqttStatusLabel;
    // 设备状态显示标签（从 MQTT status 主题解析）
    QLabel *deviceStatusLabel;
    // 设备事件/命令回执列表控件
    QListWidget *eventList;

    // ==================== 视频处理成员 ====================
    // RTP 帧重组器：将 RTP 分包重组为完整 H.264 帧
    VideoRtp::Reassembler reassembler;
    // H.264 解码器：将 H.264 码流解码为 QImage
    H264Decoder decoder;
    // 缓存最后一帧图像（用于窗口缩放时重新渲染）
    QImage lastFrame;
    // 统计周期计时器：控制 FPS/带宽统计每 1 秒更新一次
    QElapsedTimer statisticsTimer;

    // ==================== 统计计数器 ====================
    // 统计周期内的累计接收字节数
    quint64 intervalBytes;
    // 统计周期内的累计帧数
    quint32 intervalFrames;
    // 最后处理的帧序列号
    quint32 lastSequence;
    // 当前活跃的 RTP 流 ID（用于检测流切换）
    quint32 activeStreamId;

    // ==================== 状态标志 ====================
    // 是否存在活跃的 RTP 流
    bool hasActiveStream;
    // 是否正在等待关键帧（IDR 帧到达后才开始解码）
    bool waitingForKeyFrame;
    // 当前是否处于 TCP 传输模式（false 表示 UDP/RTP 模式）
    bool activeTcpTransport;
    // 当前活跃的对端（推流设备）IP 地址
    QHostAddress activePeer;
};

// 头文件保护宏结束
#endif