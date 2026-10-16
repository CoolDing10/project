/*
 * media_archive_server.h — 开发板事件录像归档服务声明
 *
 * 实时 H.264 推流使用 8888 端口；本服务默认使用 8890 端口，专门处理：
 *   1. PC 查询开发板上有哪些事件录像；
 *   2. PC 下载指定事件录像。
 *
 * 请求格式：一行 JSON，以换行符结束。
 * 响应格式：一行 JSON 头 + payloadSize 指定长度的载荷。
 * 大文件按 64 KiB 分块读取，避免一次性把整个录像装入内存。
 */
#ifndef MEDIA_ARCHIVE_SERVER_H // 防止本头文件被重复包含。
#define MEDIA_ARCHIVE_SERVER_H // 定义头文件保护宏。

#include <QByteArray> // 保存尚未组成完整一行 JSON 的 TCP 请求字节。
#include <QObject>    // 提供对象线程归属、信号和槽机制。

class QFile;      // 前向声明文件对象；实现文件中才需要完整定义。
class QTcpServer; // 前向声明 TCP 监听器；负责监听 PC 归档连接。
class QTcpSocket; // 前向声明单个 TCP 客户端连接。

class MediaArchiveServer final : public QObject // final 禁止继承；QObject 支持移入归档线程。
{
    Q_OBJECT // 启用 Qt 元对象系统，使跨线程槽调用和状态信号生效。

public:
    /**
     * 只初始化成员状态，不立即创建套接字或占用端口。
     * @param parent Qt 父对象；项目传 nullptr，之后将对象移动到归档工作线程。
     */
    explicit MediaArchiveServer(QObject *parent = nullptr);

public slots:
    /**
     * 在当前所属线程中创建 QTcpServer、QFile 并开始监听。
     * @param port 归档服务 TCP 端口，默认值 8890。
     */
    void initialize(quint16 port = 8890);

    /** 终止当前下载、关闭客户端连接并停止 TCP 监听。 */
    void stop();

signals:
    /**
     * 把启动、停止或监听错误状态送到页面线程。
     * @param status 可以直接显示或记录的中文状态文本。
     */
    void statusChanged(QString status);

private slots:
    /** 取出 QTcpServer 等待队列中的连接，并配置当前客户端。 */
    void acceptConnection();

    /** 累积 TCP 字节，找到换行符后取出一条完整 JSON 请求。 */
    void readRequest();

    /** 在套接字发送缓冲区有空间时，继续读取并排队下一批文件数据。 */
    void continueFileTransfer();

    /** 当前客户端断开后，释放套接字并复位请求和文件传输状态。 */
    void clientDisconnected();

private:
    /**
     * 解析一行 JSON，并分发 list 或 download 命令。
     * @param requestLine 不包含换行符的 UTF-8 JSON 字节。
     */
    void processRequest(const QByteArray &requestLine);

    /** 扫描 data/recordings 并向 PC 返回录像元数据 JSON。 */
    void sendArchiveList();

    /**
     * 验证请求路径、打开录像并开始定长文件传输。
     * @param category 文件类别；当前白名单只允许 recordings。
     * @param name 录像的纯文件名，不允许包含目录或 ../。
     */
    void startFileTransfer(const QString &category, const QString &name);

    /**
     * 返回统一错误响应并结束本次单请求连接。
     * @param message 写入日志和响应 JSON 的具体错误说明。
     */
    void sendError(const QString &message);

    /**
     * 写入以换行符结束的成功响应 JSON 头。
     * @param type 响应类型；当前为 list 或 file。
     * @param payloadSize JSON 头之后二进制载荷的精确字节数。
     * @param name 下载响应中的原始文件名；列表响应保持空字符串。
     */
    void sendResponseHeader(const QString &type,
                            qint64 payloadSize,
                            const QString &name = QString());

    /**
     * 把业务类别映射到可执行程序目录下的数据目录。
     * @param category 客户端请求的类别字符串。
     * @return recordings 对应目录；类别不在白名单中则返回空字符串。
     */
    QString archiveDirectory(const QString &category) const;

    /** 主动终止当前连接和文件传输，并清空协议状态。 */
    void closeClient();

    QTcpServer *server;            // 8890 端口监听器；initialize() 中创建一次。
    QTcpSocket *client;            // 当前唯一 PC 客户端；新连接会替换旧连接。
    QFile *transferFile;           // 当前正在读取的事件录像文件；对象在多次下载间复用。
    QByteArray requestBuffer;      // TCP 半包缓存；保存还没有读到换行符的请求字节。
    qint64 transferBytesRemaining; // 文件载荷中尚未交给 QTcpSocket 的字节数。
    bool transferQueuedCompletely; // true 表示完整载荷已进入 Qt 网络发送队列。
};

#endif // MEDIA_ARCHIVE_SERVER_H：结束头文件保护。
