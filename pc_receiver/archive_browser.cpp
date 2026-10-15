#include "archive_browser.h"

#include <QAbstractSocket>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPixmap>
#include <QPushButton>
#include <QResizeEvent>
#include <QSaveFile>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace {
const int CategoryRole = Qt::UserRole + 1;             // 列表项隐藏保存snapshots/recordings类别。
const int NameRole = Qt::UserRole + 2;                 // 列表项隐藏保存服务端原始文件名。
const int SizeRole = Qt::UserRole + 3;                 // 列表项隐藏保存服务端报告的文件大小。
const qint64 MaximumListPayload = 8 * 1024 * 1024;     // 列表JSON最大8 MiB，防止异常响应耗尽内存。
const int MjpegFrameIntervalMs = 66;                   // 约15 FPS的本地MJPEG回放周期。

/** 把字节数格式化成适合文件列表显示的KB或MB文本。 */
QString readableSize(qint64 bytes)
{
    // 大于等于1 MiB时显示一位小数的MB。
    if (bytes >= 1024 * 1024)
        return QStringLiteral("%1 MB").arg(bytes / 1024.0 / 1024.0, 0, 'f', 1);
    // 小文件至少显示1 KB，避免用户误以为文件为空。
    return QStringLiteral("%1 KB").arg(qMax<qint64>(1, bytes / 1024));
}
} // namespace：协议显示常量和工具函数不导出到其他编译单元。

/** 创建PC归档页的控件、布局、网络状态机和MJPEG播放定时器。 */
ArchiveBrowser::ArchiveBrowser(QWidget *parent)
    : QWidget(parent), // 初始化QWidget父子关系。
      socket(new QTcpSocket(this)), // 创建复用的归档TCP客户端。
      deviceAddressEdit(new QLineEdit(QStringLiteral("192.168.10.50"), this)), // 默认开发板IP。
      archivePortSpinBox(new QSpinBox(this)), // 创建端口输入框。
      refreshButton(new QPushButton(QStringLiteral("刷新全部归档"), this)), // 列表请求按钮。
      downloadButton(new QPushButton(QStringLiteral("下载并查看"), this)), // 文件请求按钮。
      statusLabel(new QLabel(QStringLiteral("点击刷新，读取开发板中的抓拍和录像"), this)), // 状态提示。
      archiveList(new QListWidget(this)), // 文件清单控件。
      previewLabel(new QLabel(QStringLiteral("选择一张抓拍或一段录像"), this)), // 媒体预览控件。
      mjpegPlaybackTimer(new QTimer(this)), // 录像逐帧播放定时器。
      mjpegFile(new QFile(this)), // 本地录像读取对象。
      requestKind(NoRequest), // 启动时网络状态为空闲。
      requestedSize(0), // 尚无请求载荷长度。
      responseHeaderParsed(false), // 尚未解析响应头。
      payloadBytesRemaining(0), // 尚无待接收载荷。
      downloadFile(nullptr) // 尚未创建原子下载临时文件。
{
    // TCP端口合法范围为1~65535，0不作为用户可选监听端口。
    archivePortSpinBox->setRange(1, 65535);
    // 与开发板MediaArchiveServer默认端口一致。
    archivePortSpinBox->setValue(8890);
    // 防止IP输入框在窗口缩小时被布局压缩得无法辨认。
    deviceAddressEdit->setMinimumWidth(150);
    // 长错误文本允许自动换行而不是撑宽整个窗口。
    statusLabel->setWordWrap(true);
    // 给文件名、时间和大小信息保留足够显示宽度。
    archiveList->setMinimumWidth(340);
    // 图片和录像帧在黑色预览区域居中显示。
    previewLabel->setAlignment(Qt::AlignCenter);
    previewLabel->setMinimumSize(480, 320);
    // 独立设置深色预览区域，未显示图片时仍与监控平台主题一致。
    previewLabel->setStyleSheet(QStringLiteral(
        "background:#05070a; color:#96a4b5; border:1px solid #354150; "
        "border-radius:7px;"));

    // 打开下载目录按钮只在构造函数内连接，无需保存为成员。
    QPushButton *openDirectoryButton = new QPushButton(
        QStringLiteral("打开电脑下载目录"), this);
    // controls保存页面顶部的连接参数和操作按钮。
    QHBoxLayout *controls = new QHBoxLayout();
    controls->addWidget(new QLabel(QStringLiteral("开发板IP："), this));
    controls->addWidget(deviceAddressEdit);
    controls->addWidget(new QLabel(QStringLiteral("归档端口："), this));
    controls->addWidget(archivePortSpinBox);
    controls->addWidget(refreshButton);
    controls->addWidget(downloadButton);
    controls->addWidget(openDirectoryButton);
    // 弹性空间吸收右侧空白，避免按钮被均匀拉宽。
    controls->addStretch();

    // listPanel把标题和归档列表组织成左侧竖向面板。
    QWidget *listPanel = new QWidget(this);
    QVBoxLayout *listLayout = new QVBoxLayout(listPanel);
    // 外层布局已经负责页面边距，子面板无需重复留白。
    listLayout->setContentsMargins(0, 0, 0, 0);
    listLayout->addWidget(new QLabel(QStringLiteral("开发板媒体归档"), listPanel));
    listLayout->addWidget(archiveList, 1);

    // content按2:3比例分配文件列表与媒体预览空间。
    QHBoxLayout *content = new QHBoxLayout();
    content->addWidget(listPanel, 2);
    content->addWidget(previewLabel, 3);

    // 页面顶层采用“控制栏、状态栏、主体内容”的垂直结构。
    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->addLayout(controls);
    layout->addWidget(statusLabel);
    layout->addLayout(content, 1);

    // 用户点击刷新时发起列表请求。
    connect(refreshButton, &QPushButton::clicked,
            this, &ArchiveBrowser::refreshArchive);
    // 用户点击下载时处理当前选中项。
    connect(downloadButton, &QPushButton::clicked,
            this, &ArchiveBrowser::downloadSelected);
    // 使用系统默认文件管理器打开本地下载根目录。
    connect(openDirectoryButton, &QPushButton::clicked,
            this, &ArchiveBrowser::openDownloadDirectory);
    // 双击列表项等价于选择后点击下载按钮。
    connect(archiveList, &QListWidget::itemDoubleClicked,
            this, [this](QListWidgetItem *) { downloadSelected(); });
    // TCP三段式状态机：连接成功发送请求、可读时解析、错误时复位。
    connect(socket, &QTcpSocket::connected,
            this, &ArchiveBrowser::sendRequest);
    connect(socket, &QTcpSocket::readyRead,
            this, &ArchiveBrowser::readResponse);
    connect(socket,
            QOverload<QAbstractSocket::SocketError>::of(&QTcpSocket::error),
            this, [this](QAbstractSocket::SocketError) { handleSocketError(); });
    // 每66ms尝试解码下一张JPEG，形成约15 FPS播放效果。
    mjpegPlaybackTimer->setInterval(MjpegFrameIntervalMs);
    connect(mjpegPlaybackTimer, &QTimer::timeout,
            this, &ArchiveBrowser::showNextMjpegFrame);
}

/** 同步设备地址，并移除IPv4映射IPv6地址常见的::ffff:前缀。 */
void ArchiveBrowser::setDeviceAddress(const QString &address)
{
    // 使用副本规范化，避免修改调用者传入的常量引用。
    QString normalized = address;
    // QTcpSocket在双栈系统上可能把192.168.10.50表示为::ffff:192.168.10.50。
    if (normalized.startsWith(QLatin1String("::ffff:")))
        normalized.remove(0, 7);
    // 空地址不覆盖用户已经填写的有效开发板地址。
    if (!normalized.isEmpty())
        deviceAddressEdit->setText(normalized);
}

/** 使用统一入口发起归档列表请求。 */
void ArchiveBrowser::refreshArchive()
{
    beginRequest(ListRequest);
}

/** 验证当前选择，并从列表项自定义角色中取出下载参数。 */
void ArchiveBrowser::downloadSelected()
{
    // currentItem()返回用户当前选中的抓拍或录像条目。
    QListWidgetItem *item = archiveList->currentItem();
    // 未选择时仅提示用户，不建立无意义的网络连接。
    if (!item) {
        statusLabel->setText(QStringLiteral("请先选择一个抓拍或录像文件"));
        return;
    }
    // 服务端类别、文件名和显示大小均来自受控列表响应。
    beginRequest(FileRequest,
                 item->data(CategoryRole).toString(),
                 item->data(NameRole).toString(),
                 item->data(SizeRole).toLongLong());
}

/** 清理上一次状态、记录请求参数、锁定按钮并连接开发板。 */
void ArchiveBrowser::beginRequest(RequestKind kind, const QString &category,
                                  const QString &name, qint64 size)
{
    // 连接前去除首尾空白并拒绝空IP。
    if (deviceAddressEdit->text().trimmed().isEmpty()) {
        statusLabel->setText(QStringLiteral("请填写开发板IP"));
        return;
    }

    // abort立即终止可能残留的旧连接，避免不同响应字节混在一起。
    socket->abort();
    // 清除旧半包、临时文件和按钮状态。
    resetTransfer();
    // 保存本次请求上下文，后续connected/readyRead槽会使用这些成员。
    requestKind = kind;
    requestedCategory = category;
    requestedName = name;
    requestedSize = size;
    // 请求完成或失败前禁止重复点击形成并发状态机。
    refreshButton->setEnabled(false);
    downloadButton->setEnabled(false);
    // 根据请求类型显示明确的当前操作。
    statusLabel->setText(kind == ListRequest
        ? QStringLiteral("正在连接开发板并读取归档列表...")
        : QStringLiteral("正在下载：%1").arg(name));
    // 发起异步连接；成功后由connected信号调用sendRequest()。
    socket->connectToHost(deviceAddressEdit->text().trimmed(),
                          quint16(archivePortSpinBox->value()));
}

/** 将当前请求上下文序列化为以换行符结尾的紧凑JSON。 */
void ArchiveBrowser::sendRequest()
{
    // request保存与开发板协议一致的命令对象。
    QJsonObject request;
    // 列表请求发送list，文件请求发送download。
    request.insert(QStringLiteral("command"), requestKind == ListRequest
                   ? QStringLiteral("list") : QStringLiteral("download"));
    // 下载请求还需要服务端白名单类别和纯文件名。
    if (requestKind == FileRequest) {
        request.insert(QStringLiteral("category"), requestedCategory);
        request.insert(QStringLiteral("name"), requestedName);
    }
    // 换行符标记JSON请求结束，解决TCP没有消息边界的问题。
    socket->write(QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n');
}

/** 累积当前到达的TCP数据，先解析头部，再消费定长载荷。 */
void ArchiveBrowser::readResponse()
{
    // readAll只取本轮已经到达的字节，未完成部分保留在receiveBuffer。
    receiveBuffer.append(socket->readAll());
    // 尚未取得完整头部时等待下一次readyRead，不能把JSON当载荷处理。
    if (!responseHeaderParsed && !parseResponseHeader())
        return;
    // 头部解析完成后，缓存中可能已经同时带有一部分或全部载荷。
    consumePayload();
}

/** 解析第一行JSON响应头，校验载荷长度，并为文件下载创建QSaveFile。 */
bool ArchiveBrowser::parseResponseHeader()
{
    // 服务端用换行符分隔响应头和后续二进制载荷。
    const int newline = receiveBuffer.indexOf('\n');
    // 没有换行说明响应头仍未接收完整。
    if (newline < 0) {
        // 正常JSON头远小于4 KiB，超过该值视为协议错误。
        if (receiveBuffer.size() > 4096) {
            statusLabel->setText(QStringLiteral("归档响应头无效"));
            socket->abort();
            resetTransfer();
        }
        return false;
    }

    // error接收JSON解析结果，document保存服务端响应对象。
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(
        receiveBuffer.left(newline), &error);
    // 从缓存移除响应头和换行符，余下字节全部属于载荷。
    receiveBuffer.remove(0, newline + 1);
    // 只接受格式正确的JSON对象响应。
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        statusLabel->setText(QStringLiteral("无法解析归档响应"));
        socket->abort();
        resetTransfer();
        return false;
    }

    // header包含ok、type、payloadSize及可选name字段。
    const QJsonObject header = document.object();
    // 服务端业务校验失败时直接展示其中文错误并结束本次请求。
    if (!header.value(QStringLiteral("ok")).toBool()) {
        statusLabel->setText(QStringLiteral("开发板返回错误：%1")
            .arg(header.value(QStringLiteral("message")).toString()));
        socket->disconnectFromHost();
        resetTransfer();
        return false;
    }

    // JSON数字转回本地64位载荷长度；缺失字段得到-1并判非法。
    payloadBytesRemaining = qint64(
        header.value(QStringLiteral("payloadSize")).toDouble(-1));
    // 禁止负长度，并限制列表JSON最大8 MiB以保护PC端内存。
    if (payloadBytesRemaining < 0 ||
            (requestKind == ListRequest &&
             payloadBytesRemaining > MaximumListPayload)) {
        statusLabel->setText(QStringLiteral("归档载荷长度不合法"));
        socket->abort();
        resetTransfer();
        return false;
    }

    // 文件请求需要先创建可靠的本地写入目标。
    if (requestKind == FileRequest) {
        /* 以服务端本次实际声明的长度为准，用于显示下载进度。 */
        requestedSize = payloadBytesRemaining;
        // 再次提取纯文件名，阻止服务端异常响应导致本地目录穿越。
        const QString safeName = QFileInfo(requestedName).fileName();
        if (safeName != requestedName || safeName.isEmpty()) {
            statusLabel->setText(QStringLiteral("归档文件名无效"));
            socket->abort();
            resetTransfer();
            return false;
        }
        // 按类别分别保存到imx6ull_media/snapshots或recordings。
        const QString directory = QDir(downloadRoot()).filePath(requestedCategory);
        // 目录不存在时递归创建；失败则不能继续接收文件。
        if (!QDir().mkpath(directory)) {
            statusLabel->setText(QStringLiteral("无法创建电脑下载目录"));
            socket->abort();
            resetTransfer();
            return false;
        }
        // 组合最终文件绝对路径。
        downloadedFilePath = QDir(directory).filePath(safeName);
        // QSaveFile先写同目录临时文件，完成后原子替换目标文件。
        downloadFile = new QSaveFile(downloadedFilePath);
        // 无写权限或磁盘问题会导致打开失败。
        if (!downloadFile->open(QIODevice::WriteOnly)) {
            statusLabel->setText(QStringLiteral("无法保存下载文件：%1")
                                 .arg(downloadFile->errorString()));
            delete downloadFile;
            downloadFile = nullptr;
            socket->abort();
            resetTransfer();
            return false;
        }
    }

    // 标记状态机已经进入定长载荷阶段。
    responseHeaderParsed = true;
    return true;
}

/** 从接收缓存消费不超过剩余长度的数据，并更新列表缓存或下载文件。 */
void ArchiveBrowser::consumePayload()
{
    // 只有成功解析响应头后才知道载荷目标和边界。
    if (!responseHeaderParsed)
        return;
    // consumed取“剩余协议长度”和“当前缓存长度”中的较小值。
    const qint64 consumed = qMin<qint64>(payloadBytesRemaining,
                                         receiveBuffer.size());
    // 当前可能只收到响应头而没有载荷，此时等待下一次readyRead。
    if (consumed > 0) {
        // 从缓存头部复制本轮可以消费的载荷块。
        const QByteArray chunk = receiveBuffer.left(int(consumed));
        // 删除已经消费的字节，保留可能存在的后续数据。
        receiveBuffer.remove(0, int(consumed));
        // 列表载荷较小，完整累计后一次性解析JSON。
        if (requestKind == ListRequest) {
            listPayload.append(chunk);
        // 文件载荷直接流式写盘，避免大录像全部占用内存。
        } else if (!downloadFile || downloadFile->write(chunk) != chunk.size()) {
            statusLabel->setText(QStringLiteral("电脑端写入下载文件失败"));
            socket->abort();
            resetTransfer();
            return;
        }
        // 更新尚未接收的协议载荷字节数。
        payloadBytesRemaining -= consumed;
        // 文件下载期间根据总大小和剩余大小实时计算整数百分比。
        if (requestKind == FileRequest && requestedSize > 0) {
            const int percent = int((requestedSize - payloadBytesRemaining) *
                                    100 / requestedSize);
            statusLabel->setText(QStringLiteral("正在下载：%1  %2%")
                                 .arg(requestedName).arg(percent));
        }
    }
    // 定长载荷恰好收完时提交结果；不依赖服务端关闭连接判断边界。
    if (payloadBytesRemaining == 0)
        completeRequest();
}

/** 根据请求类型处理完整载荷，随后恢复为空闲状态。 */
void ArchiveBrowser::completeRequest()
{
    // 列表请求把完整JSON载荷转换为可交互列表。
    if (requestKind == ListRequest) {
        populateArchiveList(listPayload);
    // 文件请求必须存在一个成功打开的QSaveFile。
    } else if (requestKind == FileRequest && downloadFile) {
        // commit关闭临时文件并原子替换最终文件名。
        if (!downloadFile->commit()) {
            statusLabel->setText(QStringLiteral("下载文件提交失败：%1")
                                 .arg(downloadFile->errorString()));
        } else {
            // 提示最终路径，并立即加载刚下载的媒体。
            statusLabel->setText(QStringLiteral("已下载到电脑：%1")
                                 .arg(downloadedFilePath));
            displayDownloadedFile(downloadedFilePath, requestedCategory);
        }
        // commit完成后QSaveFile对象不再使用。
        delete downloadFile;
        downloadFile = nullptr;
    }
    // 单请求协议完成后主动结束TCP连接。
    socket->disconnectFromHost();
    // 清空协议状态，为下一次刷新或下载做准备。
    requestKind = NoRequest;
    responseHeaderParsed = false;
    receiveBuffer.clear();
    listPayload.clear();
    // 重新允许用户发起操作。
    refreshButton->setEnabled(true);
    downloadButton->setEnabled(true);
}

/** 解析服务端文件数组并创建带隐藏元数据的可视列表项。 */
void ArchiveBrowser::populateArchiveList(const QByteArray &payload)
{
    // 尝试把完整列表载荷解析为JSON文档。
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(payload, &error);
    // 根节点必须为对象，其中files字段应为数组。
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        statusLabel->setText(QStringLiteral("归档列表数据无效"));
        return;
    }

    // 刷新采用全量替换，先移除旧条目避免重复显示。
    archiveList->clear();
    // 读取根对象中的files数组；字段缺失时得到空数组。
    const QJsonArray files = document.object()
            .value(QStringLiteral("files")).toArray();
    // 将每个服务端文件对象转换成一个QListWidgetItem。
    for (const QJsonValue &value : files) {
        // file保存当前条目的协议字段。
        const QJsonObject file = value.toObject();
        const QString category = file.value(QStringLiteral("category")).toString();
        const QString name = file.value(QStringLiteral("name")).toString();
        const qint64 size = qint64(file.value(QStringLiteral("size")).toDouble());
        // ISODateWithMs与开发板生成格式一致，保留毫秒解析能力。
        const QDateTime modified = QDateTime::fromString(
            file.value(QStringLiteral("modified")).toString(), Qt::ISODateWithMs);
        // 将协议类别翻译为用户界面的中文类型。
        const QString kind = category == QLatin1String("snapshots")
                ? QStringLiteral("抓拍") : QStringLiteral("录像");
        // 第一行显示类型和文件名，第二行显示大小和修改时间。
        QListWidgetItem *item = new QListWidgetItem(
            QStringLiteral("[%1] %2\n%3  %4")
                .arg(kind, name, readableSize(size),
                     modified.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))));
        // 自定义角色保存请求下载所需的原始数据，不从显示文本反向解析。
        item->setData(CategoryRole, category);
        item->setData(NameRole, name);
        item->setData(SizeRole, size);
        archiveList->addItem(item);
    }
    // 根据数组是否为空给出清晰的最终刷新结果。
    statusLabel->setText(files.isEmpty()
        ? QStringLiteral("开发板归档目录目前为空")
        : QStringLiteral("已读取 %1 个文件；双击即可下载并查看")
              .arg(files.size()));
}

/** 计算跨桌面环境可写的统一下载根目录。 */
QString ArchiveBrowser::downloadRoot() const
{
    // 优先使用系统配置的Downloads目录，而不是硬编码/home/用户名。
    QString root = QStandardPaths::writableLocation(
        QStandardPaths::DownloadLocation);
    // 极简桌面环境可能没有配置下载目录，此时回退到用户主目录。
    if (root.isEmpty())
        root = QDir::homePath();
    // 所有项目媒体集中放入imx6ull_media子目录。
    return QDir(root).filePath(QStringLiteral("imx6ull_media"));
}

/** 停止旧预览，再按归档类别显示图片或启动MJPEG循环播放。 */
void ArchiveBrowser::displayDownloadedFile(const QString &filePath,
                                           const QString &category)
{
    // 切换媒体前停止定时器，避免它继续读取旧文件。
    mjpegPlaybackTimer->stop();
    // 若旧录像仍打开，先释放文件描述符。
    if (mjpegFile->isOpen())
        mjpegFile->close();
    // 清除上一个MJPEG尚未组成完整JPEG的残留字节。
    mjpegBuffer.clear();

    // 抓拍文件由QImage直接解码并显示。
    if (category == QLatin1String("snapshots")) {
        const QImage image(filePath);
        // 不支持或损坏的图片会产生空QImage。
        if (image.isNull()) {
            previewLabel->setText(QStringLiteral("抓拍图片解码失败"));
            return;
        }
        displayImage(image);
        return;
    }

    // 非抓拍类别按本项目生成的连续JPEG录像处理。
    mjpegFile->setFileName(filePath);
    // 本地文件无法打开时显示具体QFile错误。
    if (!mjpegFile->open(QIODevice::ReadOnly)) {
        previewLabel->setText(QStringLiteral("录像打开失败：%1")
                              .arg(mjpegFile->errorString()));
        return;
    }
    // 立即显示第一帧，避免等待一个定时周期后预览才有画面。
    showNextMjpegFrame();
    // 后续每66ms继续读取一帧。
    mjpegPlaybackTimer->start();
}

/** 从MJPEG连续字节中寻找FFD8起始标记和FFD9结束标记并解码。 */
bool ArchiveBrowser::readOneMjpegFrame(QImage *image)
{
    // 调用者必须提供输出对象，并且录像文件必须已打开。
    if (!image || !mjpegFile->isOpen())
        return false;

    // restarted防止空文件或损坏文件在循环播放逻辑中无限seek。
    bool restarted = false;
    while (true) {
        // JPEG SOI标记FFD8确定一帧起点。
        int start = mjpegBuffer.indexOf(QByteArray("\xff\xd8", 2));
        // 丢弃SOI之前不属于JPEG帧的噪声或残留字节。
        if (start > 0)
            mjpegBuffer.remove(0, start);
        // 尚未找到SOI时只保留最后一个字节，以处理标记跨读取块边界的情况。
        if (start < 0 && mjpegBuffer.size() > 1)
            mjpegBuffer = mjpegBuffer.right(1);

        // 从SOI之后搜索JPEG EOI结束标记FFD9。
        const int end = mjpegBuffer.indexOf(QByteArray("\xff\xd9", 2), 2);
        // 找到完整边界后取出单帧并从缓存删除。
        if (end >= 0) {
            const QByteArray jpeg = mjpegBuffer.left(end + 2);
            mjpegBuffer.remove(0, end + 2);
            // 让Qt JPEG插件把字节解码到调用者提供的QImage。
            return image->loadFromData(jpeg, "JPEG");
        }

        // 当前缓存不足一帧时继续从文件读取64 KiB。
        const QByteArray chunk = mjpegFile->read(64 * 1024);
        // 有新字节则追加后重新搜索边界。
        if (!chunk.isEmpty()) {
            mjpegBuffer.append(chunk);
            continue;
        }
        // 到文件末尾后只回绕一次，实现循环播放且避免损坏文件死循环。
        if (restarted || !mjpegFile->seek(0))
            return false;
        // 从文件头重新读取前清除尾部残留。
        mjpegBuffer.clear();
        restarted = true;
    }
}

/** 定时器回调：读取成功则显示，读取失败则停止播放并提示。 */
void ArchiveBrowser::showNextMjpegFrame()
{
    // frame接收本次解码的JPEG图像。
    QImage frame;
    if (readOneMjpegFrame(&frame))
        displayImage(frame);
    else {
        // 无法解码时停止重复尝试，避免持续占用CPU。
        mjpegPlaybackTimer->stop();
        previewLabel->setText(QStringLiteral("录像中没有可解码的JPEG帧"));
    }
}

/** 缓存原图并按预览控件当前尺寸保持宽高比缩放。 */
void ArchiveBrowser::displayImage(const QImage &image)
{
    // 保存未缩放图像，窗口变化时可以从原图重新生成而不累计失真。
    currentPreviewImage = image;
    // SmoothTransformation用于PC端高质量预览，性能压力远低于开发板。
    previewLabel->setPixmap(QPixmap::fromImage(image).scaled(
        previewLabel->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

/** 取消未完成传输并恢复UI；不会清除已经成功显示的预览。 */
void ArchiveBrowser::resetTransfer()
{
    // 回到尚未解析新响应头的初始状态。
    responseHeaderParsed = false;
    payloadBytesRemaining = 0;
    receiveBuffer.clear();
    listPayload.clear();
    // 存在临时下载时取消写入，QSaveFile会删除未提交的临时内容。
    if (downloadFile) {
        downloadFile->cancelWriting();
        delete downloadFile;
        downloadFile = nullptr;
    }
    // 清除本次未成功提交的最终路径记录。
    downloadedFilePath.clear();
    // 允许用户重新发起请求。
    refreshButton->setEnabled(true);
    downloadButton->setEnabled(true);
}

/** 处理连接拒绝、超时等网络错误，并避免空闲期断开产生误报。 */
void ArchiveBrowser::handleSocketError()
{
    // 正常disconnectFromHost后也可能出现套接字状态变化，空闲时无需提示。
    if (requestKind == NoRequest)
        return;
    // 把Qt底层错误文本与业务提示组合显示。
    statusLabel->setText(QStringLiteral("无法访问开发板归档服务：%1")
                         .arg(socket->errorString()));
    requestKind = NoRequest;
    resetTransfer();
}

/** 确保下载根目录存在，并交给操作系统默认文件管理器打开。 */
void ArchiveBrowser::openDownloadDirectory()
{
    // 即使尚未下载过文件，也先创建目录供用户查看。
    QDir().mkpath(downloadRoot());
    QDesktopServices::openUrl(QUrl::fromLocalFile(downloadRoot()));
}

/** QWidget尺寸变化后使用原始缓存图重新计算预览缩放。 */
void ArchiveBrowser::resizeEvent(QResizeEvent *event)
{
    // 先让QWidget基类完成布局和默认尺寸事件处理。
    QWidget::resizeEvent(event);
    // 没有已显示媒体时无需执行缩放。
    if (!currentPreviewImage.isNull())
        displayImage(currentPreviewImage);
}
