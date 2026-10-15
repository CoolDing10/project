// 包含本类的头文件声明
#include "receiver_window.h"
// 引入抓拍与录像归档浏览器控件
#include "archive_browser.h"
// 引入 RTP 分包协议定义（帧重组、分包解析）
#include "rtp_packet.h"

// 引入 Qt MQTT 客户端类，用于设备远程控制和状态订阅
#include <mqtt/qmqttclient.h>

// Qt 颜色类，用于事件列表项的前景色设置
#include <QColor>
// Qt 日期时间类，用于获取当前时间戳
#include <QDateTime>
// Qt 网格布局类，用于命令按钮的网格排列
#include <QGridLayout>
// Qt 水平布局类，用于水平排列控件
#include <QHBoxLayout>
// Qt JSON 文档类，用于解析 MQTT 消息中的 JSON 数据
#include <QJsonDocument>
// Qt JSON 解析错误类，用于检测 JSON 解析是否成功
#include <QJsonParseError>
// Qt 标签控件，用于显示文本信息
#include <QLabel>
// Qt 单行文本编辑框，用于输入 MQTT 主机地址和设备 ID
#include <QLineEdit>
// Qt 列表控件，用于显示设备事件和命令回执
#include <QListWidget>
// Qt 列表项控件，列表中的每一项
#include <QListWidgetItem>
// Qt 像素图类，用于在标签上显示图像帧
#include <QPixmap>
// Qt 按钮控件，用于触发各种操作
#include <QPushButton>
// Qt 窗口大小调整事件类，响应窗口缩放
#include <QResizeEvent>
// Qt 数值输入框，用于输入端口号
#include <QSpinBox>
// Qt 选项卡控件，用于切换"实时监控"和"抓拍与录像"页面
#include <QTabWidget>
// Qt TCP 服务器类，用于监听 TCP 推流连接
#include <QTcpServer>
// Qt TCP 套接字类，用于与推流客户端通信
#include <QTcpSocket>
// Qt 定时器类，用于统计更新和不完整帧过期检查
#include <QTimer>
// Qt UDP 套接字类，用于接收 RTP/UDP 数据报
#include <QUdpSocket>
// Qt 垂直布局类，用于垂直排列控件
#include <QVBoxLayout>
// Qt 数据流类，用于从 TCP 字节流中解析帧头
#include <QDataStream>

// 匿名命名空间：这些常量仅在当前 .cpp 文件内可见
namespace
{
    // 最大合理的时钟延迟阈值（毫秒）：超过此值认为两端时钟未同步
    constexpr qint64 MaximumSensibleClockLatencyMs = 10000;
    // TCP 帧魔术字："IMXV"，与发送端一致，用于帧边界识别
    constexpr quint32 TcpFrameMagic = 0x494d5856; // "IMXV"
    // TCP 帧协议版本号（必须与发送端一致）
    constexpr quint16 TcpFrameVersion = 2;
    // TCP 帧头大小：28 字节
    constexpr quint16 TcpFrameHeaderSize = 28;
    // TCP 最大载荷大小：10 MB，超过此值的载荷视为无效（防止恶意数据）
    constexpr quint32 MaximumTcpPayloadSize = 10 * 1024 * 1024;
    // TCP 帧魔术字字节数组，用于快速前缀匹配（QByteArray::startsWith）
    const QByteArray TcpFrameMagicBytes("IMXV", 4);
}
// 构造函数：创建 PC 端监控窗口的所有 UI 控件和网络资源
// @param parent 父窗口指针
ReceiverWindow::ReceiverWindow(QWidget *parent)
    : QWidget(parent),                                                                        // 调用基类构造函数
      videoSocket(new QUdpSocket(this)),                                                      // 创建 UDP 套接字，用于接收 RTP/UDP 视频流
      tcpServer(new QTcpServer(this)),                                                        // 创建 TCP 服务器，用于监听 TCP 推流连接
      tcpClient(nullptr),                                                                     // TCP 客户端初始为 nullptr（连接到来时创建）
      reassemblyTimer(new QTimer(this)),                                                      // 重组定时器，用于周期性清理不完整帧
      portSpinBox(new QSpinBox(this)),                                                        // 监听端口选择框
      listenButton(new QPushButton(QStringLiteral("开始监听"), this)),                        // 监听/停止切换按钮
      statusLabel(new QLabel(QStringLiteral("尚未监听"), this)),                              // 状态信息显示标签
      statisticsLabel(new QLabel(QStringLiteral("FPS: 0.0  带宽: 0.00 Mbps"), this)),         // 统计信息标签
      frameLabel(new QLabel(QStringLiteral("等待 H.264/TCP 或 H.264/RTP/UDP 视频流"), this)), // 视频显示标签
      archiveBrowser(new ArchiveBrowser(this)),                                               // 创建归档浏览器控件（抓拍/录像管理）
      mqttClient(new QMqttClient(this)),                                                      // 创建 MQTT 客户端
      mqttHostEdit(new QLineEdit(QStringLiteral("192.168.10.100"), this)),                    // MQTT 服务器地址输入框（默认 IP）
      mqttPortSpinBox(new QSpinBox(this)),                                                    // MQTT 端口选择框
      deviceIdEdit(new QLineEdit(QStringLiteral("imx6ull-001"), this)),                       // 设备 ID 输入框
      mqttConnectButton(new QPushButton(QStringLiteral("连接MQTT"), this)),                   // MQTT 连接/断开按钮
      mqttStatusLabel(new QLabel(QStringLiteral("MQTT已断开"), this)),                        // MQTT 连接状态标签
      deviceStatusLabel(new QLabel(QStringLiteral("尚未收到设备心跳"), this)),                // 设备状态显示标签
      eventList(new QListWidget(this)),                                                       // 设备事件/命令回执列表
      reassembler(80),                                                                        // 帧重组器：超时时间 80ms
      intervalBytes(0),                                                                       // 统计周期内的字节数清零
      intervalFrames(0),                                                                      // 统计周期内的帧数清零
      lastSequence(0),                                                                        // 最后接收的帧序列号清零
      activeStreamId(0),                                                                      // 当前活跃的流 ID 清零
      hasActiveStream(false),                                                                 // 无活跃流
      waitingForKeyFrame(true),                                                               // 等待关键帧（IDR 帧）
      activeTcpTransport(false)                                                               // 默认非 TCP 传输模式
{
    // 设置窗口标题
    setWindowTitle(QStringLiteral("i.MX6ULL TCP/UDP 边缘视觉监控平台"));
    // 设置窗口初始大小为 1100x720 像素
    resize(1100, 720);
    // 设置对象名称，用于 QSS 样式选择器
    setObjectName(QStringLiteral("pcMonitorWindow"));
    // 设置全局 QSS 样式表：深色主题、圆角按钮、输入框样式等
    setStyleSheet(QStringLiteral(
        "#pcMonitorWindow { background:#171c24; color:#eef2f7; }"
        "QLabel { color:#e8edf3; }"
        "QLineEdit, QSpinBox { min-height:28px; background:#f8fafc; color:#17202a; border:1px solid #8793a3; border-radius:4px; }"
        "QPushButton { min-height:32px; padding:2px 10px; background:#e8edf2; color:#17202a; border:none; border-radius:5px; }"
        "QPushButton:hover { background:#ffffff; }"
        "QListWidget { background:#10151c; color:#e8edf3; border:1px solid #354150; border-radius:6px; }"));

    // 设置视频监听端口范围 1~65535
    portSpinBox->setRange(1, 65535);
    // 默认监听端口 8888
    portSpinBox->setValue(8888);
    // 设置 MQTT 端口范围 1~65535
    mqttPortSpinBox->setRange(1, 65535);
    // 默认 MQTT 端口 1883（标准 MQTT 端口）
    mqttPortSpinBox->setValue(1883);
    // 设置状态标签最小宽度 260px，保证文本不换行
    statusLabel->setMinimumWidth(260);
    // 视频帧标签居中对齐
    frameLabel->setAlignment(Qt::AlignCenter);
    // 视频帧标签最小尺寸 480x360
    frameLabel->setMinimumSize(480, 360);
    // 视频帧标签样式：黑底白字
    frameLabel->setStyleSheet(QStringLiteral("background:black;color:white;"));
    // 设备状态标签启用自动换行
    deviceStatusLabel->setWordWrap(true);
    // 事件列表最小宽度 300px
    eventList->setMinimumWidth(300);

    // === 视频监听控制行 ===
    // 创建水平布局：监听端口标签 + 端口输入框 + 监听按钮 + 状态标签（拉伸） + 统计标签
    QHBoxLayout *videoControls = new QHBoxLayout;
    videoControls->addWidget(new QLabel(QStringLiteral("TCP/UDP监听端口："), this));
    videoControls->addWidget(portSpinBox);
    videoControls->addWidget(listenButton);
    videoControls->addWidget(statusLabel, 1); // stretch factor=1，占用剩余空间
    videoControls->addWidget(statisticsLabel);

    // === MQTT 控制行 ===
    // 创建水平布局：MQTT服务器标签 + 地址输入框 + 端口标签 + 端口输入框 + 设备ID标签 + 设备ID输入框 + 连接按钮 + 状态标签
    QHBoxLayout *mqttControls = new QHBoxLayout;
    mqttControls->addWidget(new QLabel(QStringLiteral("MQTT服务器："), this));
    mqttControls->addWidget(mqttHostEdit);
    mqttControls->addWidget(new QLabel(QStringLiteral("端口："), this));
    mqttControls->addWidget(mqttPortSpinBox);
    mqttControls->addWidget(new QLabel(QStringLiteral("设备ID："), this));
    mqttControls->addWidget(deviceIdEdit);
    mqttControls->addWidget(mqttConnectButton);
    mqttControls->addWidget(mqttStatusLabel, 1);

    // === 远程命令按钮网格 ===
    // 创建网格布局，4 列排列命令按钮
    QGridLayout *commandLayout = new QGridLayout;
    // 定义命令按钮列表：标题 → MQTT 命令字符串
    const struct
    {
        const char *title;
        const char *command;
    } commands[] = {
        {"开始采集", "start_capture"}, {"停止采集", "stop_capture"},
        {"开启推流", "stream_on"}, {"停止推流", "stream_off"},
        {"开启视觉检测", "vision_on"}, {"停止视觉检测", "vision_off"}};
    // 遍历所有命令，逐个创建按钮并放入网格
    for (int i = 0; i < int(sizeof(commands) / sizeof(commands[0])); ++i)
    {
        // 创建按钮，文本为中文标题
        QPushButton *button = new QPushButton(QString::fromUtf8(commands[i].title), this);
        // 获取对应的 MQTT 命令字符串
        const QString command = QString::fromLatin1(commands[i].command);
        // 按钮点击 → 发布对应的 MQTT 命令
        connect(button, &QPushButton::clicked, this,
                [this, command]
                { publishCommand(command); });
        // 放入网格：行 = i/4，列 = i%4（每行 4 个按钮）
        commandLayout->addWidget(button, i / 4, i % 4);
    }

    // === 视频面板（左侧/主区域）===
    // 视频面板容器
    QWidget *videoPanel = new QWidget(this);
    // 垂直布局，放视频帧和设备状态
    QVBoxLayout *videoLayout = new QVBoxLayout(videoPanel);
    // 去除外边距
    videoLayout->setContentsMargins(0, 0, 0, 0);
    // 添加视频帧标签，stretch=1 占用主要空间
    videoLayout->addWidget(frameLabel, 1);
    // 添加设备状态标签
    videoLayout->addWidget(deviceStatusLabel);

    // === 事件面板（右侧）===
    // 事件面板容器
    QWidget *eventPanel = new QWidget(this);
    // 垂直布局
    QVBoxLayout *eventLayout = new QVBoxLayout(eventPanel);
    // 去除外边距
    eventLayout->setContentsMargins(0, 0, 0, 0);
    // 添加标题标签
    eventLayout->addWidget(new QLabel(QStringLiteral("设备事件/命令回执"), this));
    // 添加事件列表，stretch=1 占用主要空间
    eventLayout->addWidget(eventList, 1);

    // === 视频 + 事件水平布局 ===
    // 创建水平布局：视频面板占 3 份宽度，事件面板占 2 份宽度
    QHBoxLayout *contentLayout = new QHBoxLayout;
    contentLayout->addWidget(videoPanel, 3);
    contentLayout->addWidget(eventPanel, 2);

    // === 实时监控选项卡 ===
    // 创建"实时监控"页面
    QWidget *liveTab = new QWidget(this);
    liveTab->setLayout(contentLayout);

    // === 选项卡控件 ===
    // 创建 QTabWidget，包含"实时监控"和"抓拍与录像"两个页面
    QTabWidget *tabs = new QTabWidget(this);
    tabs->addTab(liveTab, QStringLiteral("实时监控"));
    tabs->addTab(archiveBrowser, QStringLiteral("抓拍与录像"));
    // 切换到"抓拍与录像"页面时自动刷新归档列表
    connect(tabs, &QTabWidget::currentChanged, this, [this, tabs](int index)
            {
        if (tabs->widget(index) == archiveBrowser) archiveBrowser->refreshArchive(); });

    // === 主布局 ===
    // 创建窗口主垂直布局
    QVBoxLayout *layout = new QVBoxLayout(this);
    // 依次添加：视频控制栏、MQTT 控制栏、命令按钮网格、选项卡（stretch=1 占满剩余空间）
    layout->addLayout(videoControls);
    layout->addLayout(mqttControls);
    layout->addLayout(commandLayout);
    layout->addWidget(tabs, 1);

    // === 信号与槽连接 ===
    // 监听按钮点击 → 切换监听状态
    connect(listenButton, &QPushButton::clicked, this, &ReceiverWindow::toggleListening);
    // UDP 数据报就绪 → 读取并处理数据报
    connect(videoSocket, &QUdpSocket::readyRead, this, &ReceiverWindow::readPendingDatagrams);
    // TCP 新连接到来 → 接受 TCP 连接
    connect(tcpServer, &QTcpServer::newConnection, this,
            &ReceiverWindow::acceptTcpConnection);
    // 重组定时器每 20ms 触发一次 → 清理超时的不完整帧
    reassemblyTimer->setInterval(20);
    connect(reassemblyTimer, &QTimer::timeout, this, &ReceiverWindow::expireIncompleteFrames);
    // MQTT 连接按钮点击 → 连接/断开 MQTT
    connect(mqttConnectButton, &QPushButton::clicked, this, &ReceiverWindow::toggleMqttConnection);
    // MQTT 连接状态变更时更新 UI
    connect(mqttClient, &QMqttClient::stateChanged, this,
            [this](QMqttClient::ClientState state)
            {
                if (state == QMqttClient::Connected)
                {
                    // 已连接：更新状态标签、按钮文本，禁用输入框
                    mqttStatusLabel->setText(QStringLiteral("MQTT已连接"));
                    mqttConnectButton->setText(QStringLiteral("断开MQTT"));
                    mqttHostEdit->setEnabled(false);
                    mqttPortSpinBox->setEnabled(false);
                    deviceIdEdit->setEnabled(false);
                    subscribeDeviceTopics();
                }
                else if (state == QMqttClient::Connecting)
                {
                    // 正在连接
                    mqttStatusLabel->setText(QStringLiteral("MQTT正在连接..."));
                }
                else
                {
                    // 已断开：恢复输入框和按钮状态
                    mqttStatusLabel->setText(QStringLiteral("MQTT已断开"));
                    mqttConnectButton->setText(QStringLiteral("连接MQTT"));
                    mqttHostEdit->setEnabled(true);
                    mqttPortSpinBox->setEnabled(true);
                    deviceIdEdit->setEnabled(true);
                }
            });
    // MQTT 消息接收 → 处理设备状态/事件消息
    connect(mqttClient, &QMqttClient::messageReceived, this,
            [this](const QByteArray &message, const QMqttTopicName &topic)
            {
                handleMqttMessage(message, topic.name());
            });

    // 初始化 H.264 解码器，失败则显示错误信息
    QString decoderError;
    if (!decoder.initialize(&decoderError))
        statusLabel->setText(decoderError);
    // 自动启动监听
    toggleListening();
}

// 切换监听状态：启动或停止 TCP/UDP 监听
void ReceiverWindow::toggleListening()
{
    // 如果当前已在监听（UDP 已绑定或 TCP 服务器在监听），则执行停止操作
    if (videoSocket->state() == QAbstractSocket::BoundState || tcpServer->isListening())
    {
        // 关闭 UDP 套接字
        videoSocket->close();
        // 关闭 TCP 服务器
        tcpServer->close();
        // 断开当前 TCP 客户端连接（如果有）
        if (tcpClient)
            tcpClient->disconnectFromHost();
        // 重置 TCP 会话状态（清空缓冲区、释放客户端指针）
        resetTcpSession();
        // 停止帧重组定时器
        reassemblyTimer->stop();
        // 恢复端口输入框的可编辑状态
        portSpinBox->setEnabled(true);
        // 按钮文本改为"开始监听"
        listenButton->setText(QStringLiteral("开始监听"));
        // 更新状态信息
        statusLabel->setText(QStringLiteral("TCP/UDP监听已停止"));
        // 重置视频会话（解码器和 RTP 重组器状态）
        resetVideoSession();
        return;
    }
    // === 启动监听 ===
    // 获取端口号（从输入框读取）
    const quint16 port = quint16(portSpinBox->value());
    // 绑定 UDP 套接字到所有 IPv4 地址的指定端口，使用共享地址和端口复用选项
    if (!videoSocket->bind(QHostAddress::AnyIPv4, port,
                           QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint))
    {
        // UDP 绑定失败：显示错误
        statusLabel->setText(QStringLiteral("UDP监听失败：%1").arg(videoSocket->errorString()));
        return;
    }
    // 启动 TCP 服务器监听
    if (!tcpServer->listen(QHostAddress::AnyIPv4, port))
    {
        // TCP 监听失败：保存错误信息，关闭 UDP，显示错误
        const QString error = tcpServer->errorString();
        videoSocket->close();
        statusLabel->setText(QStringLiteral("TCP监听失败：%1").arg(error));
        return;
    }
    // 监听成功：禁用端口输入框（监听中不允许换端口）
    portSpinBox->setEnabled(false);
    // 按钮文本改为"停止监听"
    listenButton->setText(QStringLiteral("停止监听"));
    // 显示监听状态
    statusLabel->setText(QStringLiteral("正在监听 H.264/TCP + H.264/RTP/UDP 0.0.0.0:%1")
                             .arg(port));
    // 重启统计计时器
    statisticsTimer.restart();
    // 启动帧重组定时器（20ms 周期）
    reassemblyTimer->start();
}

// 重置 TCP 会话状态：清空接收缓冲区和释放客户端
void ReceiverWindow::resetTcpSession()
{
    // 清空 TCP 接收缓冲区（可能残留不完整帧数据）
    tcpReceiveBuffer.clear();
    // 如果存在 TCP 客户端，使用 deleteLater 安全释放
    if (tcpClient)
    {
        tcpClient->deleteLater();
        tcpClient = nullptr;
    }
}

// 接受 TCP 客户端连接（TcpServer::newConnection 信号处理）
void ReceiverWindow::acceptTcpConnection()
{
    // 循环处理所有待处理的连接（可能同时有多个连接请求）
    while (tcpServer->hasPendingConnections())
    {
        // 获取下一个待处理的连接（返回 QTcpSocket 指针）
        QTcpSocket *incoming = tcpServer->nextPendingConnection();
        // 如果已有旧客户端，断开并释放它（只保留最新一个连接）
        if (tcpClient)
        {
            tcpClient->disconnect(this); // 断开所有与 this 相关的信号连接
            tcpClient->abort();          // 立即中止连接
            tcpClient->deleteLater();    // 安全释放
        }
        // 将新的连接赋给 tcpClient
        tcpClient = incoming;
    }
    // 如果最终没有客户端（理论上不会），返回
    if (!tcpClient)
        return;
    // 清空 TCP 接收缓冲区
    tcpReceiveBuffer.clear();
    // 启用 TCP_NODELAY：禁用 Nagle 算法，降低延迟
    tcpClient->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    // 启用 TCP KeepAlive：定期探测连接是否存活
    tcpClient->setSocketOption(QAbstractSocket::KeepAliveOption, 1);
    // 连接 TCP 数据就绪信号
    connect(tcpClient, &QTcpSocket::readyRead, this, &ReceiverWindow::readTcpFrames);
    // 连接 TCP 断开信号
    connect(tcpClient, &QTcpSocket::disconnected, this,
            &ReceiverWindow::tcpClientDisconnected);
    // 记录对端 IP 地址（用于归档浏览器）
    activePeer = tcpClient->peerAddress();
    // 设置 TCP 传输模式标志
    activeTcpTransport = true;
    // 重置解码器（新的 TCP 连接需要新的解码会话）
    decoder.reset();
    // 重置统计计数器
    intervalBytes = intervalFrames = 0;
    // 重启统计计时器
    statisticsTimer.restart();
    // 将设备 IP 传递给归档浏览器
    archiveBrowser->setDeviceAddress(activePeer.toString());
    // 更新状态显示
    statusLabel->setText(QStringLiteral("正在接收 H.264/TCP %1:%2")
                             .arg(activePeer.toString())
                             .arg(tcpClient->peerPort()));
}

// TCP 客户端断开连接处理
void ReceiverWindow::tcpClientDisconnected()
{
    // 获取发送信号的 socket 并转为 QTcpSocket*
    QTcpSocket *disconnected = qobject_cast<QTcpSocket *>(sender());
    // 安全检查：确保断开的是当前活跃的客户端
    if (disconnected != tcpClient)
        return;
    // 更新状态：提示客户端已断开
    statusLabel->setText(QStringLiteral("H.264/TCP 客户端已断开，继续等待 TCP/UDP 视频流"));
    // 清空 TCP 接收缓冲区
    tcpReceiveBuffer.clear();
    // 安全释放 TCP 客户端
    tcpClient->deleteLater();
    tcpClient = nullptr;
}

// 读取并解析 TCP 帧数据（TCP 自定义协议：28 字节帧头 + H.264 载荷）
void ReceiverWindow::readTcpFrames()
{
    // 如果客户端不存在，直接返回
    if (!tcpClient)
        return;
    // 读取所有可用数据
    const QByteArray incoming = tcpClient->readAll();
    // 累加统计周期内的接收字节数
    intervalBytes += quint64(incoming.size());
    // 追加到接收缓冲区
    tcpReceiveBuffer.append(incoming);
    // 循环解析：一次可能收到多个完整帧
    while (true)
    {
        // 数据不足一个帧头，等待更多数据
        if (tcpReceiveBuffer.size() < TcpFrameHeaderSize)
            return;
        // 数据不是以 IMXV 魔术字开头 → 需要帧同步（查找下一个魔术字位置）
        if (!tcpReceiveBuffer.startsWith(TcpFrameMagicBytes))
        {
            // 从位置 1 开始搜索魔术字
            const int marker = tcpReceiveBuffer.indexOf(TcpFrameMagicBytes, 1);
            if (marker < 0)
            {
                // 未找到魔术字：仅保留最后 3 字节（可能包含部分魔术字），丢弃前面数据
                tcpReceiveBuffer = tcpReceiveBuffer.right(3);
                return;
            }
            // 找到魔术字：丢弃魔术字之前的所有无效数据
            tcpReceiveBuffer.remove(0, marker);
            continue;
        }

        // === 解析 28 字节 TCP 帧头（大端序）===
        // 创建数据流，绑定到接收缓冲区
        QDataStream stream(tcpReceiveBuffer);
        // 设置大端字节序
        stream.setByteOrder(QDataStream::BigEndian);
        // 帧头字段变量
        quint32 magic = 0, payloadSize = 0, sequence = 0;
        quint16 version = 0, headerSize = 0, width = 0, height = 0;
        qint64 timestampMs = 0;
        // 从流中依次读取：魔术字 → 版本 → 头大小 → 载荷大小 → 帧序号 → 时间戳 → 宽度 → 高度
        stream >> magic >> version >> headerSize >> payloadSize >> sequence >> timestampMs >> width >> height;
        // 校验帧头合法性：魔术字匹配、版本匹配、头大小匹配、载荷非空且不超过上限、宽高有效
        if (magic != TcpFrameMagic || version != TcpFrameVersion ||
            headerSize != TcpFrameHeaderSize || payloadSize == 0 ||
            payloadSize > MaximumTcpPayloadSize || width == 0 || height == 0)
        {
            // 帧头无效：丢弃 1 字节，重新同步
            tcpReceiveBuffer.remove(0, 1);
            continue;
        }
        // 计算完整帧的总大小（帧头 + 载荷）
        const qint64 totalSize = qint64(headerSize) + payloadSize;
        // 数据不足一个完整帧，等待更多数据
        if (tcpReceiveBuffer.size() < totalSize)
            return;
        // 从缓冲区中提取 H.264 访问单元（载荷部分）
        const QByteArray accessUnit = tcpReceiveBuffer.mid(headerSize, payloadSize);
        // 从缓冲区中移除已消耗的数据
        tcpReceiveBuffer.remove(0, int(totalSize));

        // 如果刚从 UDP 模式切换到 TCP 模式，重置解码器和统计
        if (!activeTcpTransport)
        {
            activeTcpTransport = true;
            decoder.reset();
            intervalBytes = quint64(accessUnit.size() + headerSize);
            intervalFrames = 0;
            statisticsTimer.restart();
        }
        // 创建解码耗时计时器
        QElapsedTimer decodeTimer;
        decodeTimer.start();
        // 解码后的图像
        QImage image;
        // 解码错误描述
        QString decodeError;
        // 调用 H.264 解码器解码访问单元
        if (!decoder.decode(accessUnit, &image, &decodeError))
        {
            // 解码失败：显示错误（非空错误信息才显示）
            if (!decodeError.isEmpty())
                statusLabel->setText(QStringLiteral("H.264/TCP 解码失败：%1")
                                         .arg(decodeError));
            continue;
        }
        // 计算时钟延迟：当前时间 - 发送方时间戳
        const qint64 clockLatency = QDateTime::currentMSecsSinceEpoch() - timestampMs;
        // 端到端延迟：仅当时钟延迟在合理范围内才有效（0 ~ 10s），否则标记为不同步
        const qint64 endToEndLatency = clockLatency >= 0 &&
                                               clockLatency <= MaximumSensibleClockLatencyMs
                                           ? clockLatency
                                           : -1;
        // 显示解码后的图像到界面
        showFrame(image);
        // 更新 FPS/带宽统计信息
        updateStatistics(sequence, decodeTimer.elapsed(), endToEndLatency);
    }
}

// 重置视频会话状态：重置 RTP 重组器和 H.264 解码器
void ReceiverWindow::resetVideoSession()
{
    // 重置 RTP 帧重组器状态
    reassembler.reset();
    // 重置 H.264 解码器状态
    decoder.reset();
    // 无活跃流
    hasActiveStream = false;
    // 等待关键帧（IDR 帧）
    waitingForKeyFrame = true;
    // 重置统计计数器
    intervalBytes = intervalFrames = 0;
}

// 读取并处理所有待处理的 UDP 数据报（RTP/UDP 模式）
void ReceiverWindow::readPendingDatagrams()
{
    // 循环处理所有待读数据报
    while (videoSocket->hasPendingDatagrams())
    {
        // 分配与数据报大小相同的缓冲区
        QByteArray datagram(int(videoSocket->pendingDatagramSize()), Qt::Uninitialized);
        // 对端 IP 地址
        QHostAddress peer;
        // 对端端口号
        quint16 peerPort = 0;
        // 读取数据报到缓冲区（同时获取对端地址和端口）
        if (videoSocket->readDatagram(datagram.data(), datagram.size(), &peer, &peerPort) < 0)
            continue; // 读取失败，跳过
        // 累加统计周期内的接收字节数
        intervalBytes += quint64(datagram.size());
        // RTP 分包头部
        VideoRtp::Header header;
        // RTP 分包载荷
        QByteArray payload;
        // 解析 RTP 数据报（验证头部、提取载荷）
        if (!VideoRtp::parseDatagram(datagram, &header, &payload))
            continue; // 解析失败，跳过
        // 如果刚从 TCP 模式切换到 UDP 模式，重置统计
        if (activeTcpTransport)
        {
            activeTcpTransport = false;
            intervalBytes = quint64(datagram.size());
            intervalFrames = 0;
            statisticsTimer.restart();
        }
        // 检测流切换：无活跃流、流 ID 变化或对端 IP 变化 → 重置视频会话
        if (!hasActiveStream || header.streamId != activeStreamId || peer != activePeer)
        {
            resetVideoSession();
            hasActiveStream = true;
            activeStreamId = header.streamId;
            activePeer = peer;
            // 将设备 IP 传递给归档浏览器
            archiveBrowser->setDeviceAddress(peer.toString());
            // 显示新的流信息
            statusLabel->setText(QStringLiteral("正在接收 %1:%2，流ID %3")
                                     .arg(peer.toString())
                                     .arg(peerPort)
                                     .arg(activeStreamId));
            // 重启统计计时器
            statisticsTimer.restart();
        }
        // 将 RTP 分包推入重组器，立即处理可能产生的完整帧
        handleCompleteFrames(reassembler.push(header, payload,
                                              QDateTime::currentMSecsSinceEpoch()));
    }
}

// 定时器触发的过期清理：将超时未完成的帧标记为过期并处理
void ReceiverWindow::expireIncompleteFrames()
{
    // 从重组器中取出所有超时分包，返回可能部分完整的帧
    handleCompleteFrames(reassembler.expire(QDateTime::currentMSecsSinceEpoch()));
}

// 处理完整帧：解码并显示
// @param frames 重组器产生的一系列完整帧（可能为空）
void ReceiverWindow::handleCompleteFrames(const QVector<VideoRtp::CompleteFrame> &frames)
{
    // 遍历每一帧完整帧
    for (const VideoRtp::CompleteFrame &frame : frames)
    {
        // 如果帧前面发生了不连续（丢帧），重置解码器并等待新的关键帧
        if (frame.discontinuityBefore)
        {
            decoder.reset();
            waitingForKeyFrame = true;
        }
        // 如果正在等待关键帧且当前不是关键帧，跳过此帧
        if (waitingForKeyFrame && !frame.keyFrame)
            continue;
        // 收到关键帧：结束等待状态
        if (frame.keyFrame)
            waitingForKeyFrame = false;

        // 创建解码计时器
        QElapsedTimer decodeTimer;
        decodeTimer.start();
        // 解码后的图像
        QImage image;
        // 解码错误描述
        QString error;
        // 解码 H.264 访问单元
        if (!decoder.decode(frame.accessUnit, &image, &error))
        {
            // 解码失败：显示错误
            if (!error.isEmpty())
                statusLabel->setText(QStringLiteral("H.264解码失败：%1").arg(error));
            continue;
        }
        // 本地处理延迟 = 重组耗时 + 解码耗时
        const qint64 localLatency = frame.reassemblyMilliseconds + decodeTimer.elapsed();
        // 当前时间戳（微秒）
        const qint64 nowUs = QDateTime::currentMSecsSinceEpoch() * 1000;
        // 时钟延迟 = 当前时间 - 发送时间戳（转为毫秒）
        const qint64 clockLatency = (nowUs - qint64(frame.sendTimestampUs)) / 1000;
        // 端到端延迟：仅当时钟延迟在合理范围内才有效
        const qint64 endToEndLatency = clockLatency >= 0 &&
                                               clockLatency <= MaximumSensibleClockLatencyMs
                                           ? clockLatency
                                           : -1;
        // 显示解码图像
        showFrame(image);
        // 更新 FPS/带宽/丢帧统计
        updateStatistics(frame.sequence, localLatency, endToEndLatency);
    }
}

// 在界面上显示解码后的视频帧
// @param image 解码后的 QImage 视频帧
void ReceiverWindow::showFrame(const QImage &image)
{
    // 保存最后一帧（用于窗口缩放时重新渲染）
    lastFrame = image;
    // 将 QImage 转为 QPixmap，按标签尺寸等比例缩放后设置到显示标签
    frameLabel->setPixmap(QPixmap::fromImage(lastFrame).scaled(
        frameLabel->size(), Qt::KeepAspectRatio, Qt::FastTransformation));
}

// 更新 FPS、带宽、丢帧等统计信息（每 1 秒更新一次）
// @param sequence 当前帧的帧序列号
// @param localLatencyMs 本地处理延迟（解码+重组耗时，毫秒）
// @param endToEndLatencyMs 端到端延迟（毫秒），-1 表示时钟未同步
void ReceiverWindow::updateStatistics(quint32 sequence, qint64 localLatencyMs,
                                      qint64 endToEndLatencyMs)
{
    // 记录最后处理的帧序列号
    lastSequence = sequence;
    // 统计周期内的帧数加 1
    ++intervalFrames;
    // 距离上次统计更新的经过时间
    const qint64 elapsed = statisticsTimer.elapsed();
    // 如果不足 1 秒，暂不更新统计显示
    if (elapsed < 1000)
        return;
    // 计算 FPS：帧数 / 秒数
    const double fps = intervalFrames * 1000.0 / elapsed;
    // 计算带宽 Mbps：字节数 * 8 / 秒数 / 1000
    const double mbps = intervalBytes * 8.0 / elapsed / 1000.0;
    // 格式化端到端延迟字符串
    const QString e2e = endToEndLatencyMs >= 0
                            ? QStringLiteral("%1 ms").arg(endToEndLatencyMs)
                            : QStringLiteral("时钟未同步");
    // 根据传输模式显示不同的统计信息
    if (activeTcpTransport)
        // TCP 模式统计：FPS、带宽、解码耗时、端到端延迟、最后一帧序号
        statisticsLabel->setText(
            QStringLiteral("TCP/H.264  FPS:%1  %2 Mbps  解码:%3 ms  端到端:%4  帧:%5")
                .arg(fps, 0, 'f', 1)
                .arg(mbps, 0, 'f', 2)
                .arg(localLatencyMs)
                .arg(e2e)
                .arg(lastSequence));
    else
        // UDP 模式统计：FPS、带宽、丢帧数、丢片数、重组+解码耗时、端到端延迟、最后一帧序号
        statisticsLabel->setText(
            QStringLiteral("UDP/H.264  FPS:%1  %2 Mbps  丢帧:%3  丢片:%4  重组+解码:%5 ms  端到端:%6  帧:%7")
                .arg(fps, 0, 'f', 1)
                .arg(mbps, 0, 'f', 2)
                .arg(reassembler.lostFrames())
                .arg(reassembler.lostFragments())
                .arg(localLatencyMs)
                .arg(e2e)
                .arg(lastSequence));
    // 重置统计周期计数器
    intervalBytes = intervalFrames = 0;
    // 重启统计计时器
    statisticsTimer.restart();
}

// 切换 MQTT 连接状态：连接或断开
void ReceiverWindow::toggleMqttConnection()
{
    // 如果当前不是断开状态 → 执行断开操作
    if (mqttClient->state() != QMqttClient::Disconnected)
    {
        mqttClient->disconnectFromHost();
        return;
    }
    // 检查服务器地址和设备 ID 是否已填写
    if (mqttHostEdit->text().trimmed().isEmpty() || deviceIdEdit->text().trimmed().isEmpty())
    {
        mqttStatusLabel->setText(QStringLiteral("请填写MQTT服务器和设备ID"));
        return;
    }
    // 设置 MQTT 服务器主机名
    mqttClient->setHostname(mqttHostEdit->text().trimmed());
    // 设置 MQTT 服务器端口
    mqttClient->setPort(quint16(mqttPortSpinBox->value()));
    // 设置客户端 ID（pc-monitor- + 当前毫秒时间戳，确保唯一性）
    mqttClient->setClientId(QStringLiteral("pc-monitor-%1").arg(QDateTime::currentMSecsSinceEpoch()));
    // 发起异步连接
    mqttClient->connectToHost();
}

// 根据后缀生成完整的设备 MQTT 主题路径
// @param suffix 主题后缀（如 "status", "events", "commands"）
// @return 完整的 MQTT 主题字符串，格式为 "devices/{deviceId}/{suffix}"
QString ReceiverWindow::deviceTopic(const QString &suffix) const
{
    return QStringLiteral("devices/%1/%2").arg(deviceIdEdit->text().trimmed(), suffix);
}

// 订阅设备相关的 MQTT 主题（状态、事件、命令回执）
void ReceiverWindow::subscribeDeviceTopics()
{
    // 需要订阅的主题后缀列表
    const QStringList suffixes = {QStringLiteral("status"), QStringLiteral("events"),
                                  QStringLiteral("command_ack")};
    // QoS 1 级别订阅每个主题
    for (const QString &suffix : suffixes)
        if (!mqttClient->subscribe(deviceTopic(suffix), 1))
            mqttStatusLabel->setText(QStringLiteral("订阅失败：%1").arg(suffix));
}

// 处理接收到的 MQTT 消息：设备状态更新或事件/命令回执
// @param message MQTT 消息体（JSON 格式字节数组）
// @param topic MQTT 主题字符串
void ReceiverWindow::handleMqttMessage(const QByteArray &message, const QString &topic)
{
    // JSON 解析错误对象
    QJsonParseError error;
    // 解析 MQTT 消息为 JSON 文档
    const QJsonDocument document = QJsonDocument::fromJson(message, &error);
    // 如果解析失败或不是 Object 类型，使用空 Object
    const QJsonObject object = document.isObject() ? document.object() : QJsonObject();

    // 如果是设备状态主题
    if (topic == deviceTopic(QStringLiteral("status")))
    {
        // JSON 解析成功且为 Object 类型时，格式化设备状态信息
        if (error.error == QJsonParseError::NoError)
            deviceStatusLabel->setText(QStringLiteral(
                                           "设备: %1  采集: %2  推流: %3  画面检测: %4  录像: %5\n"
                                           "时间: %6")
                                           // 设备 ID
                                           .arg(object.value("deviceId").toString(),
                                                // 采集状态：ON/OFF
                                                object.value("captureEnabled").toBool() ? "ON" : "OFF",
                                                // 推流状态：ON/OFF
                                                object.value("streamingEnabled").toBool() ? "ON" : "OFF",
                                                // 视觉检测状态：ON/OFF
                                                object.value("visionEnabled").toBool() ? "ON" : "OFF",
                                                // 录像状态
                                                object.value("recording").toBool() ? "ON" : "OFF",
                                                // 时间戳
                                                object.value("timestamp").toString()));
        return;
    }
    // 提取消息时间戳
    const QString timestamp = object.value(QStringLiteral("timestamp")).toString();
    // 根据主题类型提取描述文本：
    // 事件主题 → 取 message 字段；命令回执主题 → 格式化 command + message
    const QString description = topic.endsWith(QLatin1String("/events"))
                                    ? object.value(QStringLiteral("message")).toString()
                                    : QStringLiteral("命令 %1：%2").arg(object.value(QStringLiteral("command")).toString(), object.value(QStringLiteral("message")).toString());
    // 创建事件列表项：时间戳 + 描述
    QListWidgetItem *item = new QListWidgetItem(QStringLiteral("%1  %2").arg(timestamp, description));
    // 事件类型的消息使用橙色前景色
    if (topic.endsWith(QLatin1String("/events")))
        item->setForeground(QColor(QStringLiteral("#d35400")));
    // 插入到列表顶部（最新的显示在最前面）
    eventList->insertItem(0, item);
    // 限制事件列表最多保留 200 条，超出时从尾部删除
    while (eventList->count() > 200)
        delete eventList->takeItem(eventList->count() - 1);
}

// 通过 MQTT 向设备发布远程控制命令
// @param command 命令字符串（如 "start_capture"、"stream_on" 等）
// @param arguments 附加参数（JSON 对象）
void ReceiverWindow::publishCommand(const QString &command, const QJsonObject &arguments)
{
    // 检查 MQTT 是否已连接
    if (mqttClient->state() != QMqttClient::Connected)
    {
        mqttStatusLabel->setText(QStringLiteral("请先连接MQTT"));
        return;
    }
    // 构造请求 JSON 对象（以附加参数为基础）
    QJsonObject request = arguments;
    // 插入命令名称
    request.insert(QStringLiteral("command"), command);
    // 插入当前时间戳（ISO 格式，含毫秒）
    request.insert(QStringLiteral("timestamp"), QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
    // 发布到设备命令主题，QoS=1 至少一次传递，不保留消息
    if (mqttClient->publish(deviceTopic(QStringLiteral("commands")),
                            QJsonDocument(request).toJson(QJsonDocument::Compact), 1, false) < 0)
        mqttStatusLabel->setText(QStringLiteral("远程命令发布失败"));
}

// 窗口大小变化事件：重新缩放视频帧以适应新窗口尺寸
// @param event 窗口大小调整事件
void ReceiverWindow::resizeEvent(QResizeEvent *event)
{
    // 调用基类处理（必须调用以保证 Qt 内部布局正常工作）
    QWidget::resizeEvent(event);
    // 如果有缓存的最后一帧，重新缩放到新尺寸
    if (!lastFrame.isNull())
        showFrame(lastFrame);
}
