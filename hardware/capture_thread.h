/*
 * capture_thread.h — V4L2 摄像头采集线程与三路固定帧率分发器
 *
 * CaptureThread 在独立线程中完成 V4L2 采集，再将同一份深拷贝图像分发给：
 *   localPreviewReady  → 主线程本地预览
 *   networkFrameReady  → 网络线程 H.264 编码与发送
 *   analysisFrameReady → 视觉线程画面变化检测
 *
 * 各链路只按 video_pipeline_config.h 中的固定帧率分发。
 */
#ifndef CAPTURE_THREAD_H // 如果尚未包含本声明，才继续编译。
#define CAPTURE_THREAD_H // 定义保护宏，避免 CaptureThread 重复声明。

#include <atomic> // 使用线程安全的 std::atomic_bool 保存跨线程开关。

#include <QImage>  // 三路信号传递的图像帧类型。
#include <QThread> // CaptureThread 继承的 Qt 线程基类。

#define VIDEO_DEV "/dev/video1" // 开发板 V4L2 摄像头设备节点。
#define VIDEO_BUFFER_COUNT 3     // 向 V4L2 驱动申请并循环使用的 MMAP 缓冲区数量。

class CaptureThread : public QThread // 独立执行 V4L2 阻塞采集循环的生产者线程。
{
    Q_OBJECT // 启用 Qt 信号槽，使采集帧可以安全投递到其他线程。

public: // 创建、销毁采集线程的公共接口。
    /** @param parent Qt 父对象；通常为 CameraStreamingPage。 */
    explicit CaptureThread(QObject *parent = nullptr); // 初始化四个原子开关为 false。
    ~CaptureThread() override;                         // 请求采集循环退出并等待线程结束。

signals: // 由采集线程发出，Qt 队列连接把图像交给相应消费者线程。
    /** @param image 已脱离 V4L2 缓冲区生命周期的深拷贝预览帧。 */
    void localPreviewReady(QImage image); // 发送给主线程进行本地画面显示。
    /** @param image 已深拷贝的网络编码输入帧。 */
    void networkFrameReady(QImage image); // 发送给 TcpFrameSender 工作线程。
    /** @param image 已深拷贝的视觉分析输入帧。 */
    void analysisFrameReady(QImage image); // 发送给 VisionEventDetector 工作线程。

public slots: // 由主线程的按钮和复选框调用的线程安全控制入口。
    /** @param enabled true 启动采集线程，false 请求停止采集。 */
    void setCaptureEnabled(bool enabled); // 修改总开关，并按需 start() 或 requestInterruption()。
    /** @param enabled 是否向网络编码线程分发帧。 */
    void setNetworkStreamingEnabled(bool enabled); // 原子更新网络分发开关。
    /** @param enabled 是否向主线程分发本地预览帧。 */
    void setLocalPreviewEnabled(bool enabled); // 原子更新预览分发开关。
    /** @param enabled 是否向视觉线程分发分析帧。 */
    void setAnalysisEnabled(bool enabled); // 原子更新分析分发开关。

private: // 线程执行入口和跨线程状态。
    void run() override; // 在工作线程内完成 V4L2 初始化、采集循环及资源释放。

    std::atomic_bool captureEnabled;          // 采集循环总开关，主线程写、采集线程读。
    std::atomic_bool networkStreamingEnabled; // 是否按网络帧率发出 networkFrameReady。
    std::atomic_bool localPreviewEnabled;     // 是否按预览帧率发出 localPreviewReady。
    std::atomic_bool analysisEnabled;         // 是否按分析帧率发出 analysisFrameReady。

};

#endif // CAPTURE_THREAD_H：结束头文件保护。
