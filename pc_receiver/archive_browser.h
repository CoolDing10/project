#ifndef ARCHIVE_BROWSER_H // 防止归档浏览器类被重复声明。
#define ARCHIVE_BROWSER_H // 定义头文件保护宏。

#include <QByteArray> // 保存 TCP 半包、列表 JSON、下载块和 MJPEG 字节。
#include <QImage>     // 保存并显示下载的静态图片或 MJPEG 当前帧。
#include <QWidget>    // ArchiveBrowser 的页面基类。

class QFile;        // 顺序读取旧版 MJPEG 录像。
class QLabel;       // 显示状态文本、表单标签和图片/录像画面。
class QLineEdit;    // 输入开发板IPv4地址。
class QListWidget;  // 展示开发板返回的事件录像文件列表。
class QMediaPlayer; // 播放新生成的 H.264 录像。
class QPushButton;  // 刷新、下载以及打开目录按钮。
class QResizeEvent; // 窗口缩放事件类型。
class QSaveFile;    // 原子下载文件：成功commit前不会破坏同名正式文件。
class QSpinBox;     // 输入归档TCP端口。
class QStackedWidget; // 在图片/MJPEG预览和 H.264 播放画面之间切换。
class QTcpSocket;   // 连接开发板8890媒体归档服务。
class QTimer;       // 按固定间隔播放旧版 MJPEG 中的下一 JPEG 帧。
class QVideoWidget; // QMediaPlayer 的 H.264 视频输出控件。

/* PC端事件录像浏览器：从开发板8890端口读取列表并按需下载。 */
class ArchiveBrowser final : public QWidget // 独立管理归档网络请求、下载及预览状态。
{
    Q_OBJECT // 启用Qt信号槽以及元对象信息。

public: // 页面构造和开发板地址同步接口。
    /** @brief 创建并布局PC归档浏览页面，同时初始化网络和播放状态机。 */
    explicit ArchiveBrowser(QWidget *parent = nullptr); // parent 是主窗口标签页容器。

    /**
     * @brief 从实时视频连接同步开发板地址，减少用户重复输入。
     * @param address IPv4地址，兼容Qt可能返回的::ffff:IPv4形式。
     */
    void setDeviceAddress(const QString &address); // address 是视频连接得到的 IPv4 或 IPv4 映射地址。

public slots: // 可由标签页切换或用户按钮调用的公开槽。
    /** @brief 请求开发板重新扫描并返回全部事件录像元数据。 */
    void refreshArchive(); // 发起 list 短连接请求。

protected: // QWidget 事件重写。
    /** @brief 页面尺寸改变后按新预览区域重新等比缩放当前图像。 */
    void resizeEvent(QResizeEvent *event) override; // event 描述新尺寸，用于重新缩放缓存图像。

private slots: // 由按钮、socket 和定时器信号触发的内部动作。
    /** @brief 下载列表中当前选中的文件并在完成后自动预览。 */
    void downloadSelected(); // 下载当前列表中被用户选中的归档文件。
    /** @brief TCP连接建立后发送一行JSON请求。 */
    void sendRequest(); // TCP连接建立后发送当前归档请求。
    /** @brief 累积TCP字节并依次解析响应头与定长载荷。 */
    void readResponse(); // 读取并解析归档服务返回的TCP字节流。
    /** @brief 将套接字错误显示到UI并清理未完成请求。 */
    void handleSocketError(); // 处理归档服务连接或传输错误。
    /** @brief 从本地MJPEG文件读取并显示下一张JPEG图像。 */
    void showNextMjpegFrame(); // 从旧版MJPEG录像读取并显示下一帧。
    /** @brief 调用桌面文件管理器打开统一下载目录。 */
    void openDownloadDirectory(); // 使用系统文件管理器打开归档下载目录。

private: // 请求状态机、协议解析、预览辅助函数和成员状态。
    /** @brief 当前网络请求状态；单个ArchiveBrowser同一时刻只处理一个请求。 */
    enum RequestKind { // 区分当前短连接应解释为列表还是文件载荷。
        NoRequest,   // 当前空闲，没有等待中的服务端响应。
        ListRequest, // 正在获取JSON归档列表。
        FileRequest  // 正在下载一个图片或录像文件。
    };

    /**
     * @brief 复位旧连接并开始一次新的列表或文件请求。
     * @param kind 请求类型。
     * @param category 文件请求的类别，列表请求为空。
     * @param name 文件请求的纯文件名，列表请求为空。
     * @param size 列表中记录的预估大小；收到响应头后以服务端实际值覆盖。
     */
    void beginRequest(RequestKind kind, const QString &category = QString(), // kind 决定命令，category 是文件分类。
                      const QString &name = QString(), qint64 size = 0); // name 是纯文件名，size 是列表预估长度。

    /** @brief 从receiveBuffer解析换行结尾的JSON头并初始化载荷接收目标。 */
    bool parseResponseHeader(); // 成功解析换行 JSON 头时返回 true。

    /** @brief 按响应头声明的长度消费缓存，将数据加入列表或写入下载文件。 */
    void consumePayload(); // 从 receiveBuffer 消费不超过剩余长度的数据。

    /** @brief 提交完整下载或解析列表，并恢复按钮及网络状态。 */
    void completeRequest(); // 完成列表解析或 QSaveFile 原子提交。

    /** @brief 取消临时文件、清除半包数据并回到可发起请求状态。 */
    void resetTransfer(); // 取消临时下载并清空当前协议解析状态。

    /** @brief 将服务端JSON列表转换成带自定义角色数据的QListWidgetItem。 */
    void populateArchiveList(const QByteArray &payload); // payload 是服务端 files JSON 文档。

    /** @brief 返回~/Downloads/imx6ull_media；无下载目录时回退到用户主目录。 */
    QString downloadRoot() const; // 返回电脑端统一归档下载根目录。

    /** @brief 根据类别把已下载文件作为静态图片或录像显示。 */
    void displayDownloadedFile(const QString &filePath); // filePath 是下载完成的本地绝对路径。

    /** @brief 保存当前图像，并按previewLabel尺寸等比平滑缩放。 */
    void displayImage(const QImage &image); // image 是要缓存并等比缩放显示的帧。

    /** @brief 从字节流搜索JPEG SOI/EOI标记，解码一帧MJPEG。 */
    bool readOneMjpegFrame(QImage *image); // image 接收解析出的下一张 JPEG，成功返回 true。

    QTcpSocket *socket;                 // 归档协议的短连接TCP客户端。
    QLineEdit *deviceAddressEdit;       // 开发板IP输入框，默认192.168.10.50。
    QSpinBox *archivePortSpinBox;       // 归档服务端口输入框，默认8890。
    QPushButton *refreshButton;         // 发起ListRequest。
    QPushButton *downloadButton;        // 发起当前文件的FileRequest。
    QLabel *statusLabel;                // 显示连接、下载进度和错误信息。
    QListWidget *archiveList;           // 保存服务端文件条目及其隐藏元数据。
    QLabel *previewLabel;               // 显示旧版 MJPEG 当前帧或播放提示。
    QStackedWidget *previewStack;       // 切换静态/MJPEG与 H.264 画面。
    QMediaPlayer *h264Player;           // 播放下载后的 H.264 裸码流。
    QVideoWidget *h264VideoOutput;      // H.264 播放画面输出。
    QTimer *mjpegPlaybackTimer;          // 每66ms推进一帧，约15 FPS。
    QFile *mjpegFile;                   // 当前打开的本地MJPEG录像。

    RequestKind requestKind;            // 当前请求状态机枚举值。
    QString requestedCategory;          // 当前文件类别：recordings。
    QString requestedName;              // 当前下载的服务端文件名。
    qint64 requestedSize;               // 当前载荷总长度，用于计算下载百分比。
    bool responseHeaderParsed;          // 是否已经从TCP流中剥离完整JSON头。
    qint64 payloadBytesRemaining;        // 距离当前定长载荷完成还差多少字节。
    QByteArray receiveBuffer;            // TCP拆包/粘包接收缓存。
    QByteArray listPayload;              // ListRequest完整JSON载荷累计缓存。
    QSaveFile *downloadFile;             // FileRequest临时文件，成功后原子commit。
    QString downloadedFilePath;          // 当前文件最终保存到电脑的绝对路径。

    QByteArray mjpegBuffer;              // 尚未组成完整JPEG帧的录像字节缓存。
    QImage currentPreviewImage;           // 最近显示图像，窗口缩放时用于重新渲染。
};

#endif // ARCHIVE_BROWSER_H：结束头文件保护。
