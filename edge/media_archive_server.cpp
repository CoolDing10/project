#include "media_archive_server.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>

namespace {
// 单条JSON请求的最大长度，防止异常客户端持续发送数据耗尽内存。
const int MaximumRequestBytes = 4096;
// 每次从文件读取64 KiB，在系统调用次数和i.MX6ULL内存占用间取平衡。
const qint64 TransferChunkBytes = 64 * 1024;
// QTcpSocket最多积压512 KiB，超过后等待bytesWritten信号继续发送。
const qint64 MaximumQueuedBytes = 512 * 1024;
} // namespace：以上常量只允许当前实现文件使用。

/**
 * 构造阶段仅初始化状态；QTcpServer和QFile必须在initialize()所在线程创建，
 * 这样它们的线程归属与archiveWorkerThread一致。
 */
MediaArchiveServer::MediaArchiveServer(QObject *parent)
    : QObject(parent),                    // 把Qt父对象交给QObject基类管理。
      server(nullptr),                    // 尚未创建端口监听器。
      client(nullptr),                    // 启动时没有PC客户端连接。
      transferFile(nullptr),              // 启动时没有正在读取的文件。
      transferBytesRemaining(0),          // 剩余发送字节初始为0。
      transferQueuedCompletely(false)     // 尚不存在已完整排队的文件传输。
{
}

/** 初始化归档网络服务；重复调用时直接返回，避免重复监听同一端口。 */
void MediaArchiveServer::initialize(quint16 port)
{
    // server非空说明初始化已经执行过，即使listen失败也不重复创建对象。
    if (server)
        return;

    // 子对象使用this作为父对象，归档服务析构时由Qt自动回收。
    server = new QTcpServer(this);
    // QFile复用于每一次下载，避免每个请求重复分配文件对象。
    transferFile = new QFile(this);
    // 监听器发现新连接时，转入acceptConnection()完成套接字配置。
    connect(server, &QTcpServer::newConnection,
            this, &MediaArchiveServer::acceptConnection);

    // AnyIPv4允许PC通过开发板任意IPv4网卡访问8890端口。
    if (!server->listen(QHostAddress::AnyIPv4, port)) {
        // 拼接Qt提供的具体错误，例如端口占用或权限不足。
        const QString message = QStringLiteral("媒体归档服务监听失败：%1")
                .arg(server->errorString());
        // 保留终端诊断输出，便于开发板现场排查。
        qWarning() << message;
        // 同时把失败状态通知上层UI。
        emit statusChanged(message);
        return;
    }

    // 监听成功后同时输出绑定地址和实际端口。
    const QString message = QStringLiteral("媒体归档服务已监听 0.0.0.0:%1")
            .arg(port);
    qInfo() << message;
    emit statusChanged(message);
}

/** 停止服务时先终止活动传输，再关闭监听器，确保文件句柄不会泄漏。 */
void MediaArchiveServer::stop()
{
    // 复用统一清理函数关闭套接字、文件及协议状态。
    closeClient();
    // server可能尚未initialize，因此关闭前必须判空。
    if (server)
        server->close();
    // 保留明确的停止日志，区分主动退出和网络异常。
    qInfo() << "媒体归档服务已停止";
}

/**
 * 取出Qt监听队列里的全部连接。协议当前只支持单客户端，因此保留最后一个
 * 连接，并主动清理旧连接，避免卡死客户端长期占用服务。
 */
void MediaArchiveServer::acceptConnection()
{
    // hasPendingConnections()可能一次报告多个排队连接，所以使用while取完。
    while (server && server->hasPendingConnections()) {
        // incoming的所有权稍后由deleteLater()安全释放。
        QTcpSocket *incoming = server->nextPendingConnection();
        /* 当前协议一次只服务一个请求；新连接会替换已失效/卡住的旧连接。 */
        closeClient();
        // 将最新连接记录为当前协议连接。
        client = incoming;
    }
    // 防御nextPendingConnection()异常返回空指针的情况。
    if (!client)
        return;

    // 启用TCP KeepAlive，使系统能够发现长时间失效的对端。
    client->setSocketOption(QAbstractSocket::KeepAliveOption, 1);
    // 收到请求字节时进入readRequest()累积并按换行符拆包。
    connect(client, &QTcpSocket::readyRead,
            this, &MediaArchiveServer::readRequest);
    // 内核发送走一部分数据后继续填充文件块，实现非阻塞流式下载。
    connect(client, &QTcpSocket::bytesWritten,
            this, [this](qint64) { continueFileTransfer(); });
    // 对端关闭时释放套接字并复位传输状态。
    connect(client, &QTcpSocket::disconnected,
            this, &MediaArchiveServer::clientDisconnected);
    // 新连接不能继承上一个连接残留的半包请求。
    requestBuffer.clear();
    // 新连接开始时不存在活动文件载荷。
    transferBytesRemaining = 0;
    transferQueuedCompletely = false;
}

/** 读取TCP字节流，并以换行符作为单行JSON请求的结束标记。 */
void MediaArchiveServer::readRequest()
{
    // 下载进行期间不允许同一连接再发第二条请求，保持协议状态机简单可靠。
    if (!client || transferBytesRemaining > 0 || transferQueuedCompletely)
        return;

    // readAll()只取得当前已到达字节；TCP拆包时需要继续等待后续readyRead。
    requestBuffer.append(client->readAll());
    // 请求超过4 KiB通常表示客户端异常或没有发送换行符。
    if (requestBuffer.size() > MaximumRequestBytes) {
        sendError(QStringLiteral("请求过大"));
        return;
    }

    // 搜索一条请求的协议结束符。
    const int newline = requestBuffer.indexOf('\n');
    // 尚未收齐一行时保留缓存，等待下次readyRead。
    if (newline < 0)
        return;

    // 截取第一行并移除首尾空白，随后交给JSON解析器。
    const QByteArray requestLine = requestBuffer.left(newline).trimmed();
    // 单连接只处理一个请求，因此清空其余缓存。
    requestBuffer.clear();
    processRequest(requestLine);
}

/** 解析请求对象，并根据command字段分发列表或下载操作。 */
void MediaArchiveServer::processRequest(const QByteArray &requestLine)
{
    // error保存解析失败的类型和偏移，document保存解析结果。
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(requestLine, &error);
    // 协议只接受JSON对象，不接受数组、数字或损坏的JSON。
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        sendError(QStringLiteral("请求JSON无效"));
        return;
    }

    // 提取请求对象以及命令名称。
    const QJsonObject request = document.object();
    const QString command = request.value(QStringLiteral("command")).toString();
    // list返回全部媒体元数据，不携带文件内容。
    if (command == QLatin1String("list")) {
        sendArchiveList();
    // download要求客户端同时提供类别和纯文件名。
    } else if (command == QLatin1String("download")) {
        startFileTransfer(
            request.value(QStringLiteral("category")).toString(),
            request.value(QStringLiteral("name")).toString());
    // 未知命令必须显式返回错误，不能静默断开。
    } else {
        sendError(QStringLiteral("不支持的归档命令"));
    }
}

/** 只把白名单类别映射到应用程序目录下的数据目录。 */
QString MediaArchiveServer::archiveDirectory(const QString &category) const
{
    // 白名单限制是后续路径安全校验的第一层。
    if (category != QLatin1String("snapshots") &&
            category != QLatin1String("recordings"))
        return QString();
    // 数据目录与可执行文件并列，符合开发板/opt/project/bin部署结构。
    return QDir(QCoreApplication::applicationDirPath())
            .filePath(QStringLiteral("data/%1").arg(category));
}

/** 扫描两类归档文件并生成紧凑JSON列表响应。 */
void MediaArchiveServer::sendArchiveList()
{
    // files最终包含所有可下载文件的类别、名称、大小和修改时间。
    QJsonArray files;
    // 服务端只暴露业务所需的两个固定目录。
    const QStringList categories = {
        QStringLiteral("snapshots"), QStringLiteral("recordings")};

    // 分别扫描抓拍目录和录像目录。
    for (const QString &category : categories) {
        // 根据类别构造安全目录对象。
        const QDir directory(archiveDirectory(category));
        // 新录像统一为 H.264；保留旧 MJPEG 文件的下载兼容性。
        const QStringList filters = category == QLatin1String("snapshots")
                ? QStringList{QStringLiteral("*.jpg"), QStringLiteral("*.jpeg"),
                              QStringLiteral("*.png"), QStringLiteral("*.bmp")}
                : QStringList{QStringLiteral("*.h264"),
                              QStringLiteral("*.mjpeg"), QStringLiteral("*.mjpg")};
        // 仅列出可读普通文件，并按照修改时间从新到旧排列。
        const QFileInfoList entries = directory.entryInfoList(
            filters, QDir::Files | QDir::Readable, QDir::Time);
        // 把每个QFileInfo转换成与PC端约定的JSON对象。
        for (const QFileInfo &entry : entries) {
            QJsonObject item;
            item.insert(QStringLiteral("category"), category);
            item.insert(QStringLiteral("name"), entry.fileName());
            // JSON数字使用double；对本项目媒体文件大小仍能保持整数精度。
            item.insert(QStringLiteral("size"), double(entry.size()));
            item.insert(QStringLiteral("modified"),
                        entry.lastModified().toString(Qt::ISODateWithMs));
            files.append(item);
        }
    }

    // 使用根对象包装数组，为以后增加设备ID等字段保留扩展空间。
    QJsonObject payloadObject;
    payloadObject.insert(QStringLiteral("files"), files);
    // Compact格式减少局域网传输字节和PC端等待时间。
    const QByteArray payload = QJsonDocument(payloadObject)
            .toJson(QJsonDocument::Compact);
    // 必须先发带长度的响应头，接收端才能正确切分TCP字节流。
    sendResponseHeader(QStringLiteral("list"), payload.size());
    // 连接可能在扫描目录期间被对端关闭，因此写入前再次判空。
    if (client) {
        client->write(payload);
        // 单请求单连接：载荷写入Qt缓冲区后进入优雅断开状态。
        client->disconnectFromHost();
    }
    qInfo() << "媒体归档列表已发送，文件数=" << files.size();
}

/** 校验文件类别和名称，打开文件并固定本次响应的载荷长度。 */
void MediaArchiveServer::startFileTransfer(const QString &category,
                                            const QString &name)
{
    // category只有通过白名单时才会得到非空目录。
    const QString directoryPath = archiveDirectory(category);
    /* 只接受纯文件名，禁止 ../ 或绝对路径访问归档目录以外的文件。 */
    if (directoryPath.isEmpty() || name.isEmpty() ||
            QFileInfo(name).fileName() != name) {
        sendError(QStringLiteral("归档文件参数无效"));
        return;
    }

    // fileInfo表示白名单目录与客户端文件名拼接后的候选文件。
    const QFileInfo fileInfo(QDir(directoryPath).filePath(name));
    // canonicalPath消除符号链接和../，用于最终目录边界校验。
    const QString canonicalDirectory = QDir(directoryPath).canonicalPath();
    const QString canonicalFile = fileInfo.canonicalFilePath();
    // 同时检查存在性、类型、权限以及规范路径是否仍位于归档目录内。
    if (!fileInfo.exists() || !fileInfo.isFile() || !fileInfo.isReadable() ||
            canonicalDirectory.isEmpty() ||
            !canonicalFile.startsWith(canonicalDirectory + QDir::separator())) {
        sendError(QStringLiteral("归档文件不存在或不可读"));
        return;
    }

    // 复用QFile对象并指向已经通过安全检查的规范路径。
    transferFile->setFileName(canonicalFile);
    // 以只读方式打开，服务端不会修改开发板上的媒体文件。
    if (!transferFile->open(QIODevice::ReadOnly)) {
        sendError(QStringLiteral("归档文件打开失败：%1")
                  .arg(transferFile->errorString()));
        return;
    }

    /* 固定本次传输长度；即使正在录像的文件继续增长也不破坏协议边界。 */
    transferBytesRemaining = fileInfo.size();
    // 空文件没有数据块需要排队，可在响应头发送完成后直接断开。
    transferQueuedCompletely = transferBytesRemaining == 0;
    if (transferQueuedCompletely)
        transferFile->close();
    // 响应头携带固定长度和文件名，PC端据此创建下载任务。
    sendResponseHeader(QStringLiteral("file"), transferBytesRemaining, name);
    qInfo() << "开始发送媒体归档：" << canonicalFile
            << "字节=" << transferBytesRemaining;
    // 主动发送第一批文件数据，后续批次由bytesWritten信号驱动。
    continueFileTransfer();
}

/**
 * 使用QTcpSocket写缓冲区水位实现背压：缓冲未满时连续读取64 KiB文件块，
 * 达到512 KiB后返回事件循环，待网络实际发送后再由bytesWritten继续。
 */
void MediaArchiveServer::continueFileTransfer()
{
    // 没有活动连接时不能继续访问套接字。
    if (!client)
        return;
    // 文件已全部交给Qt后，只需等待发送缓冲区清空再优雅断开。
    if (transferQueuedCompletely) {
        if (client->bytesToWrite() == 0)
            client->disconnectFromHost();
        return;
    }
    // 非空传输必须有一个已打开的只读文件。
    if (!transferFile || !transferFile->isOpen())
        return;

    // 只要仍有数据且网络缓冲未超过高水位，就继续排入文件块。
    while (transferBytesRemaining > 0 &&
           client->bytesToWrite() < MaximumQueuedBytes) {
        // 最后一块可能小于64 KiB，不能读取超过协议声明的固定长度。
        const qint64 wanted = qMin(TransferChunkBytes, transferBytesRemaining);
        const QByteArray chunk = transferFile->read(wanted);
        // 预期仍有数据时读到空块，说明文件读取发生异常或文件被截断。
        if (chunk.isEmpty()) {
            qWarning() << "媒体归档读取中断：" << transferFile->fileName();
            client->abort();
            return;
        }
        // write()把数据交给Qt发送缓冲区，并不会阻塞等待对端接收。
        const qint64 accepted = client->write(chunk);
        // 对QTcpSocket而言正常写入应接受整个QByteArray；否则终止损坏传输。
        if (accepted != chunk.size()) {
            qWarning() << "媒体归档网络写入失败：" << client->errorString();
            client->abort();
            return;
        }
        // 从尚未排队的协议载荷长度中扣除本次已接受字节。
        transferBytesRemaining -= accepted;
    }

    // 剩余长度归零表示完整载荷已经进入Qt网络发送队列。
    if (transferBytesRemaining == 0) {
        transferQueuedCompletely = true;
        // 文件内容已经不再需要，尽早释放开发板文件描述符。
        transferFile->close();
    }
    // 若网络也已全部发完，则结束本次单请求连接。
    if (transferQueuedCompletely && client->bytesToWrite() == 0)
        client->disconnectFromHost();
}

/** 构造统一的单行JSON响应头；换行符是协议头和二进制载荷的分隔符。 */
void MediaArchiveServer::sendResponseHeader(const QString &type,
                                             qint64 payloadSize,
                                             const QString &name)
{
    // 对端可能已断开；此时不再创建无效响应。
    if (!client)
        return;
    // header保存PC端解析响应所需的最小元数据。
    QJsonObject header;
    header.insert(QStringLiteral("ok"), true);
    header.insert(QStringLiteral("type"), type);
    // Qt JSON没有64位整数类型，使用double编码本项目可接受的文件长度。
    header.insert(QStringLiteral("payloadSize"), double(payloadSize));
    // 只有文件响应才需要返回文件名。
    if (!name.isEmpty())
        header.insert(QStringLiteral("name"), name);
    // Compact JSON后追加换行符，接收端先readLine()再按长度读取载荷。
    client->write(QJsonDocument(header).toJson(QJsonDocument::Compact) + '\n');
}

/** 记录错误、返回失败响应头，并按单请求协议关闭连接。 */
void MediaArchiveServer::sendError(const QString &message)
{
    // 开发板日志保留具体原因，便于区分非法请求和文件系统问题。
    qWarning() << "媒体归档请求失败：" << message;
    // 没有客户端时只能记录本地日志。
    if (!client)
        return;
    // 错误响应不包含后续载荷，因此payloadSize固定为0。
    QJsonObject header;
    header.insert(QStringLiteral("ok"), false);
    header.insert(QStringLiteral("message"), message);
    header.insert(QStringLiteral("payloadSize"), 0);
    client->write(QJsonDocument(header).toJson(QJsonDocument::Compact) + '\n');
    // 使用优雅断开，尽量保证错误JSON先发到PC端。
    client->disconnectFromHost();
}

/** 响应QTcpSocket::disconnected，延迟销毁对象并复位下一连接所需状态。 */
void MediaArchiveServer::clientDisconnected()
{
    // sender()是触发当前槽的套接字；qobject_cast同时完成运行时类型检查。
    QTcpSocket *disconnected = qobject_cast<QTcpSocket *>(sender());
    // deleteLater()确保对象在所属线程事件循环中安全析构。
    if (disconnected)
        disconnected->deleteLater();
    // 只有当前客户端断开时才清空成员指针。
    if (client == disconnected)
        client = nullptr;
    // 未完成下载不能把打开的文件遗留给下一连接。
    if (transferFile && transferFile->isOpen())
        transferFile->close();
    // 清除文件长度、水位状态和半包请求。
    transferBytesRemaining = 0;
    transferQueuedCompletely = false;
    requestBuffer.clear();
}

/** 主动终止当前客户端；用于服务停止以及新连接替换旧连接。 */
void MediaArchiveServer::closeClient()
{
    // 首先关闭可能仍在读取的媒体文件。
    if (transferFile && transferFile->isOpen())
        transferFile->close();
    // 无论是否存在客户端，都先复位协议状态。
    transferBytesRemaining = 0;
    transferQueuedCompletely = false;
    requestBuffer.clear();
    // 没有客户端时清理工作已经完成。
    if (!client)
        return;
    // 断开本对象接收的全部套接字信号，防止abort后槽函数重复清理。
    client->disconnect(this);
    // abort立即丢弃未发送数据；本函数用于替换或停止，而非正常完成响应。
    client->abort();
    // 在套接字所属线程安全销毁。
    client->deleteLater();
    // 立即清空悬空风险指针。
    client = nullptr;
}
