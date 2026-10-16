/*
 * Copyright (C) Deng Zhimao Co., Ltd. 2021-2030.
 * All rights reserved.
 */
#include "capture_thread.h"        // 引入 CaptureThread 类声明。
#include "video_pipeline_config.h" // 引入预览、推流和分析统一使用的帧率常量。

#include <QDebug>        // 提供 qWarning() 采集错误日志。
#include <QElapsedTimer> // 使用单调时钟限制三条消费者链路的分发帧率。

#if defined(__linux__) && defined(__arm__) // V4L2 设备操作只在 ARM Linux 开发板上参与编译。
#include <algorithm>        // 提供 std::min()，限制实际缓冲区数量不超过本地数组容量。
#include <cerrno>           // 提供 errno、EAGAIN 和 EINTR。
#include <cstring>          // 提供 std::strerror() 错误文本转换。
#include <fcntl.h>          // 提供 open()、O_RDWR 和 O_NONBLOCK。
#include <linux/videodev2.h> // 提供 V4L2 结构体、像素格式和 ioctl 命令。
#include <poll.h>           // 提供 poll() 和 pollfd 数据就绪等待接口。
#include <sys/ioctl.h>      // 提供 ioctl() 设备控制系统调用。
#include <sys/mman.h>       // 提供 mmap()、munmap() 和内存映射标志。
#include <sys/time.h>       // 提供 V4L2 相关时间结构的系统定义。
#include <unistd.h>         // 提供 close() 等 POSIX 系统调用。
#endif // 结束 ARM Linux 专用头文件区域。

/*
 * ===== CaptureThread 构造函数 =====
 * 参数 parent：Qt 父对象指针（nullptr 表示无父对象）
 * 初始化列表：将所有 atomic 标志置为 false
 * 函数体为空：所有初始化都在初始化列表中完成
 */
CaptureThread::CaptureThread(QObject *parent) // parent 是负责管理线程对象生命周期的 Qt 父对象。
    : QThread(parent),                 // 调用基类 QThread 的构造函数
      captureEnabled(false),           // 采集开关初始关闭
      networkStreamingEnabled(false),  // 网络推流初始关闭
      localPreviewEnabled(false),      // 本地预览初始关闭
      analysisEnabled(false)           // 视觉分析初始关闭
{
    // 构造函数体为空：所有成员已在初始化列表中完成赋值
}

/*
 * ===== CaptureThread 析构函数 =====
 * 三步安全退出：
 *   1. captureEnabled.store(false)  → 通知采集循环停止
 *   2. requestInterruption()         → 设置中断标志（while 条件检查）
 *   3. wait()                        → 阻塞等待线程 run() 执行完毕
 */
CaptureThread::~CaptureThread() // 无论页面从何种状态退出，都等待采集线程安全结束。
{
    // 将采集使能标志置为 false（原子存储），采集循环中的 while 条件将变为 false
    captureEnabled.store(false); // 原子关闭循环总开关，避免与采集线程并发读写产生数据竞争。
    // 请求线程中断：isInterruptionRequested() 将返回 true，双重保险确保退出
    requestInterruption(); // 设置 QThread 中断标志，让 run() 的第二个循环条件失效。
    // 阻塞等待线程 run() 函数返回（超时则死等），确保资源已被 cleanup() 释放
    wait(); // 阻塞析构线程，直到 run() 完成 V4L2 清理并返回。
}

/*
 * ===== setCaptureEnabled() — 采集总开关 =====
 * 参数 enabled：true 启动采集线程，false 停止采集线程
 * store() 是 std::atomic 的写入方法，保证写操作是原子的。
 * QThread::start() 会创建新线程并在其中执行 run()。
 * requestInterruption() 设置中断标志让 run() 中的 while 条件变为 false。
 */
void CaptureThread::setCaptureEnabled(bool enabled) // enabled 为 true 启动采集，为 false 请求停止。
{
    captureEnabled.store(enabled); // 原子写入：更新采集使能标志

    if (enabled) // 开启采集分支。
    {
        // 用户点击"开始采集"
        if (!isRunning())     // 如果线程还没有在运行
            QThread::start(); // 创建新线程 → 调用 run() → 进入 V4L2 采集循环
    }
    else // 关闭采集分支。
    {
        // 用户点击"停止采集"
        requestInterruption(); // 请求线程中断：isInterruptionRequested() 返回 true
    }
}

/*
 * ===== setNetworkStreamingEnabled() — 网络推流开关 =====
 * 参数 enabled：true 开始向 TcpFrameSender 发送帧，false 停止
 * 只在采集循环中被读取（poll→DQBUF→判断 networkEnabled→决定是否发射 networkFrameReady）
 */
void CaptureThread::setNetworkStreamingEnabled(bool enabled) // enabled 控制网络编码链路是否接收帧。
{
    networkStreamingEnabled.store(enabled); // 原子写入：更新网络推流标识
}

/*
 * ===== setLocalPreviewEnabled() — 本地预览开关 =====
 * 参数 enabled：true 开始向主线程发送预览帧，false 停止
 */
void CaptureThread::setLocalPreviewEnabled(bool enabled) // enabled 控制主线程本地预览是否接收帧。
{
    localPreviewEnabled.store(enabled); // 原子写入：更新本地预览标识
}

/*
 * ===== setAnalysisEnabled() — 视觉分析开关 =====
 * 参数 enabled：true 开始向 VisionEventDetector 发送帧，false 停止
 */
void CaptureThread::setAnalysisEnabled(bool enabled) // enabled 控制视觉变化检测是否接收帧。
{
    analysisEnabled.store(enabled); // 原子写入：更新视觉分析标识
}

/*
 * ===== run() — 线程主函数（V4L2 采集流水线核心实现） =====
 *
 * 由 QThread::start() 在独立线程中调用，不阻塞主线程。
 * 完整 V4L2 流程：
 *   open → S_FMT → REQBUFS → QUERYBUF → mmap → QBUF
 *        → STREAMON → (poll → DQBUF → 处理 → QBUF) 循环
 *        → STREAMOFF → munmap → 释放缓冲区 → close
 *
 * #if 条件编译：在非 ARM Linux 平台（如 x86 PC）上，此函数直接返回，
 *    因为 V4L2 是 Linux 内核接口，macOS/Windows 不支持。
 */
void CaptureThread::run() // QThread::start() 创建的工作线程从这里开始执行。
{
    /*
     * ===== 平台检查：只有 ARM Linux 才执行 V4L2 代码 =====
     * 在 x86 PC 上开发调试时，摄像头采集功能不启用。
     */
#if !defined(__linux__) || !defined(__arm__) // 桌面构建不编译下方 V4L2 设备访问代码。
    // 直接访问 V4L2 的实现只在 Linux ARM 开发板上启用。
    captureEnabled.store(false); // 非 ARM Linux 平台：强制关闭采集
    return;                      // 直接返回，不执行任何 V4L2 操作
#else // ARM Linux 开发板执行完整采集实现。
    // ===== V4L2 采集仅在 ARM Linux 平台编译和执行 =====

    /*
     * ===== MappedBuffer 结构体：保存一个 mmap() 映射的 V4L2 缓冲区 =====
     * start  缓冲区映射到本进程后的虚拟地址（可直接当数组访问像素数据）
     * length 映射长度（字节数），munmap() 释放时需要这个值
     */
    struct MappedBuffer // 描述一个驱动缓冲区在当前进程中的映射地址和长度。
    {
        void *start;   // 缓冲区映射到本进程后的虚拟地址
        size_t length; // 映射长度，释放时传给 munmap()
    };

    // 应用最多管理 VIDEO_BUFFER_COUNT（当前为 3）个缓冲区的数组
    // MAP_FAILED 表示该元素尚未成功建立映射（mmap 失败时的返回值）
    MappedBuffer buffers[VIDEO_BUFFER_COUNT]; // 静态数组，3 个槽位
    for (unsigned int i = 0; i < VIDEO_BUFFER_COUNT; ++i) // i 是本地缓冲区数组下标。
    {
        buffers[i].start = MAP_FAILED; // 初始化所有缓冲区为"未映射"状态
        buffers[i].length = 0;         // 映射长度初始为 0
    }

    // ===== 采集状态变量 =====
    int videoFd = -1;              // open() 返回的摄像头设备文件描述符，-1 表示未打开
    unsigned int mappedCount = 0;  // 已成功 mmap 的缓冲区数量（用于 cleanup 中逐个 munmap）
    bool streaming = false;        // 流是否已启动（VIDIOC_STREAMON 成功 → true）
    bool buffersRequested = false; // 是否已申请缓冲区（VIDIOC_REQBUFS 成功 → true）

    /*
     * ===== logIoctlError Lambda：V4L2 操作失败时打印日志 =====
     * 参数 operation：失败的 ioctl 操作名称（如 "VIDIOC_S_FMT"）
     * 使用 errno 获取最后一次系统调用的错误码，std::strerror 将其转为可读信息
     */
    auto logIoctlError = [](const char *operation) // operation 是失败的系统调用或 ioctl 操作名。
    {
        // qWarning 打印警告级别日志，全局变量 errno 保存最近一次系统调用的错误码
        qWarning("%s failed: %s", operation, std::strerror(errno)); // 同时输出操作名和当前 errno 文本。
    };

    /*
     * ===== cleanup Lambda：资源安全回收 =====
     * [&] 表示按引用捕获本函数中的局部变量（videoFd、buffers、streaming 等）
     * 无论初始化进行到哪个阶段失败，调用 cleanup() 都可以安全回收已分配的资源。
     * 释放顺序与申请顺序基本相反：STREAMOFF → munmap → REQBUFS(0) → close
     */
    auto cleanup = [&]() // 按引用捕获本函数资源状态，统一处理任意失败阶段的释放。
    {
        // videoFd < 0 表示设备没有成功打开，无资源可释放
        if (videoFd < 0) // 设备未打开时没有后续 V4L2 资源需要清理。
            return; // 直接返回，什么都不做

        if (streaming) // 只有 STREAMON 成功过才允许发送 STREAMOFF。
        {
            // 声明缓冲区类型为视频采集（VIDIOC_STREAMOFF 的参数要求）
            enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE; // type 指定要停止单平面视频采集队列。
            // ioctl(fd, request, arg)：发送控制命令给设备驱动
            //   videoFd          要控制的设备文件描述符
            //   VIDIOC_STREAMOFF 停止指定类型的视频流
            //   &type            指向缓冲区类型的地址（enum 变量的指针）
            if (ioctl(videoFd, VIDIOC_STREAMOFF, &type) < 0) // 请求驱动停止 DMA 采集并回收排队缓冲区。
                logIoctlError("VIDIOC_STREAMOFF"); // 记录失败但继续清理
            streaming = false;                     // 更新状态标志
        }

        // 逐个取消 mmap 映射
        for (unsigned int i = 0; i < mappedCount; ++i) // 仅遍历已经成功 mmap 的前 mappedCount 项。
        {
            // 只处理已成功映射的缓冲区
            if (buffers[i].start != MAP_FAILED) // 跳过未成功建立的映射。
            {
                // munmap(address, length)：取消用户空间的内存映射
                //   address = mmap() 当初返回的虚拟地址
                //   length  = 当初映射的字节数
                if (munmap(buffers[i].start, buffers[i].length) < 0) // 取消该段用户虚拟地址映射。
                    logIoctlError("munmap");                        // 记录失败但继续释放其他资源。
                // 重置为未映射状态
                buffers[i].start = MAP_FAILED; // 防止后续清理重复 munmap 同一地址。
                buffers[i].length = 0;         // 清除与该映射对应的旧长度。
            }
        }

        if (buffersRequested) // REQBUFS 成功后需要显式以 count=0 释放驱动队列。
        {
            // 对 VIDIOC_REQBUFS 传入 count=0，通知驱动释放队列中先前分配的所有 MMAP 缓冲区
            struct v4l2_requestbuffers releaseBuffers = {};    // {} 清零整个结构体
            releaseBuffers.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; // 指定视频采集队列
            releaseBuffers.memory = V4L2_MEMORY_MMAP;          // 指定 mmap 方式
            releaseBuffers.count = 0;                          // count=0 表示释放所有缓冲区
            if (ioctl(videoFd, VIDIOC_REQBUFS, &releaseBuffers) < 0) // 通知驱动释放其分配的 MMAP 缓冲区。
                logIoctlError("VIDIOC_REQBUFS(count=0)");           // 失败只记日志，仍继续关闭设备。
            buffersRequested = false; // 缓冲区已释放
        }

        // 关闭设备文件描述符；之后这个整数不能再用于 ioctl
        close(videoFd); // 释放 /dev/video1 文件描述符和驱动打开引用。
        videoFd = -1; // 标记为已关闭
    };

    /*
     * ===== 第 1 步：打开摄像头设备 =====
     * open(path, flags)：
     *   VIDEO_DEV  目标 V4L2 设备节点 = "/dev/video1"
     *   O_RDWR     以读写方式打开，V4L2 流式采集要求此模式
     *   O_NONBLOCK 非阻塞模式；暂时无数据时 DQBUF 返回 EAGAIN，
     *              因此后面先用 poll() 等待数据就绪
     * 返回值：成功返回非负整数的文件描述符，失败返回 -1
     */
    videoFd = open(VIDEO_DEV, O_RDWR | O_NONBLOCK); // 以读写、非阻塞模式打开摄像头设备。
    if (videoFd < 0)                                // -1 表示设备不存在、忙或权限不足。
    {
        // 打开失败：打印错误信息并退出
        qWarning("Unable to open %s: %s", VIDEO_DEV, std::strerror(errno)); // 输出设备路径和内核错误原因。
        captureEnabled.store(false); // 同步采集状态为"已停止"
        return;                      // 直接返回，线程结束
    }

    /*
     * ===== 第 2 步：协商图像格式（VIDIOC_S_FMT） =====
     * v4l2_format 描述应用希望获得的视频格式。
     * {} 将整个结构体清零，避免保留字段中出现未初始化的随机值。
     */
    struct v4l2_format format = {};                   // 清零初始化
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;        // 单平面视频采集队列
    format.fmt.pix.width = 640;                       // 请求的图像宽度（像素）
    format.fmt.pix.height = 480;                      // 请求的图像高度（像素）
    format.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565; // 每像素 16 bit（R5G6B5）
    format.fmt.pix.field = V4L2_FIELD_ANY;            // 场类型交给驱动选择（逐行/隔行）
    format.fmt.pix.colorspace = V4L2_COLORSPACE_SRGB; // 请求 sRGB 色彩空间

    /*
     * VIDIOC_S_FMT 与驱动协商图像格式。
     * 第三个参数必须指向 v4l2_format。调用成功后，驱动可能会修改
     * width、height、pixelformat、bytesperline、sizeimage 等字段，
     * 所以后续必须使用调用返回后的 format，而不能只相信请求值。
     */
    if (ioctl(videoFd, VIDIOC_S_FMT, &format) < 0) // 提交格式请求，并让驱动把最终协商值写回 format。
    {
        logIoctlError("VIDIOC_S_FMT"); // 记录失败原因
        cleanup();                     // 回收已打开的设备文件描述符
        captureEnabled.store(false);   // 同步状态
        return;                        // 格式未建立，结束线程。
    }

    /*
     * 检查驱动是否真正接受了 RGB565 格式。
     * 后面使用 QImage::Format_RGB16 解释缓冲区字节，因此驱动实际返回的
     * 像素格式必须仍是 RGB565，否则相同字节会被错误地解释为颜色。
     */
    if (format.fmt.pix.pixelformat != V4L2_PIX_FMT_RGB565) // 实际格式不匹配 QImage::Format_RGB16 时拒绝继续。
    {
        qWarning("%s did not accept RGB565", VIDEO_DEV); // 打印警告
        cleanup();                                       // 回收资源
        captureEnabled.store(false);                     // 同步状态
        return;                                              // 避免错误解释像素内存。
    }

    /*
     * ===== 第 3 步：申请 MMAP 缓冲区（VIDIOC_REQBUFS） =====
     * 请求驱动为视频采集队列建立 MMAP 缓冲区：
     *   count  期望数量（不保证驱动一定原样满足）
     *   type   必须和 format.type、后续 buffer.type 保持一致
     *   memory 表示缓冲区由驱动管理，并通过 mmap 映射给应用
     */
    struct v4l2_requestbuffers request = {};    // 清零初始化
    request.count = VIDEO_BUFFER_COUNT;         // 请求 3 个缓冲区
    request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; // 视频采集类型
    request.memory = V4L2_MEMORY_MMAP;          // 使用 mmap 内存映射方式

    // 第三个参数必须指向 v4l2_requestbuffers
    if (ioctl(videoFd, VIDIOC_REQBUFS, &request) < 0) // 请求驱动创建 MMAP 缓冲区队列。
    {
        logIoctlError("VIDIOC_REQBUFS"); // 输出申请失败原因。
        cleanup();                        // 关闭已打开的设备。
        captureEnabled.store(false);      // 同步外部采集状态。
        return;                           // 结束线程。
    }
    buffersRequested = true; // 标记缓冲区已申请（cleanup 中需要释放）

    /*
     * ioctl 返回后 request.count 是驱动实际建立的数量。
     * 本地数组最多只能容纳 VIDEO_BUFFER_COUNT 个，所以取两者较小值。
     * std::min(a, b)：返回 a 和 b 中较小的值。
     * static_cast<unsigned int>(VIDEO_BUFFER_COUNT)：C 风格类型转换的安全替代。
     */
    const unsigned int bufferCount = // bufferCount 保存驱动实际数量与本地容量中的较小值。
        std::min(request.count, static_cast<unsigned int>(VIDEO_BUFFER_COUNT)); // 防止访问 buffers 数组边界外。
    // 至少两个缓冲区：一个驱动在填充，一个应用在读取，形成流水线
    if (bufferCount < 2) // 少于两个缓冲区无法稳定形成采集与处理流水线。
    {
        qWarning("%s returned only %u capture buffer(s)", VIDEO_DEV, bufferCount); // 输出驱动实际返回数量。
        cleanup();                          // 释放驱动缓冲区并关闭设备。
        captureEnabled.store(false);        // 对外标记采集失败。
        return;                             // 结束线程。
    }

    /*
     * ===== 第 4 步：查询缓冲区信息并 mmap 映射（QUERYBUF → mmap） =====
     * 逐一查询驱动缓冲区的信息（长度、偏移量），并映射到本进程地址空间。
     */
    for (unsigned int i = 0; i < bufferCount; ++i) // 逐个查询并映射驱动实际提供的缓冲区。
    { // 遍历每一个缓冲区
        // v4l2_buffer：描述单个 V4L2 缓冲区
        struct v4l2_buffer buffer = {};            // 清零初始化
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; // 查询采集队列
        buffer.memory = V4L2_MEMORY_MMAP;          // 查询 MMAP 缓冲区
        buffer.index = i;                          // 要查询的缓冲区编号（0, 1, 2, ...）

        /*
         * VIDIOC_QUERYBUF：查询第 i 个缓冲区的物理信息。
         * 成功后驱动返回：
         *   buffer.length   第 i 个缓冲区的总长度（字节数）
         *   buffer.m.offset 第 i 个缓冲区用于 mmap 的设备偏移量
         */
        if (ioctl(videoFd, VIDIOC_QUERYBUF, &buffer) < 0) // 让驱动填入第 i 个缓冲区的长度和映射偏移。
        {
            logIoctlError("VIDIOC_QUERYBUF"); // 输出查询失败原因。
            cleanup();                         // 清理由前几次迭代已建立的映射。
            captureEnabled.store(false);       // 同步采集停止状态。
            return;                            // 结束线程。
        }

        buffers[i].length = buffer.length; // 保存缓冲区长度（后面 munmap 需要）
        /*
         * mmap(addr, length, prot, flags, fd, offset)：
         *   nullptr                 由内核选择用户空间的虚拟地址
         *   buffer.length           要映射的字节数
         *   PROT_READ | PROT_WRITE  映射区域可读、可写
         *   MAP_SHARED              与驱动共享映射内容（驱动写入后用户立即可见）
         *   videoFd                 /dev/video1 的文件描述符
         *   buffer.m.offset         QUERYBUF 返回的设备偏移量
         *
         * mmap 不会复制整帧图像，而是建立用户虚拟地址到驱动物理缓冲区的
         * 映射关系。成功后可通过 buffers[i].start 直接访问图像字节。
         * 这是零拷贝的核心：用户代码读的就是 DMA 硬件写入的原始数据。
         */
        buffers[i].start = mmap(nullptr, buffer.length,             // 由内核选择地址，并映射 buffer.length 字节。
                                PROT_READ | PROT_WRITE, MAP_SHARED, // 允许读写且与驱动共享相同物理页。
                                videoFd, buffer.m.offset);           // 使用摄像头描述符和 QUERYBUF 返回的偏移。
        // mmap 失败返回 MAP_FAILED（即 (void *)-1），不是 nullptr
        if (buffers[i].start == MAP_FAILED) // mmap 失败返回特殊值 MAP_FAILED。
        {
            logIoctlError("mmap");      // 输出映射失败原因。
            cleanup();                   // 释放此前成功映射的缓冲区。
            captureEnabled.store(false); // 同步采集停止状态。
            return;                      // 结束线程。
        }
        ++mappedCount; // 已成功映射的缓冲区数 +1
    } // for 循环结束：所有缓冲区已 mmap 完毕

    /*
     * ===== 第 5 步：将所有空缓冲区入队（QBUF） =====
     * QBUF 后缓冲区的所有权暂时交给驱动；驱动开始用 DMA 向其中填入摄像头数据。
     * 应用必须等 DQBUF 成功后（驱动交还所有权）才能读取对应缓冲区中的完整图像。
     */
    for (unsigned int i = 0; i < bufferCount; ++i) // 将每个已映射缓冲区依次交给驱动填充。
    {
        struct v4l2_buffer buffer = {};            // 清零初始化
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; // 视频采集队列
        buffer.memory = V4L2_MEMORY_MMAP;          // mmap 方式
        buffer.index = i;                          // 第 i 个缓冲区

        // VIDIOC_QBUF：将空缓冲区放入驱动的入队队列
        if (ioctl(videoFd, VIDIOC_QBUF, &buffer) < 0) // 把编号为 i 的空缓冲区加入采集输入队列。
        {
            logIoctlError("VIDIOC_QBUF"); // 输出入队失败原因。
            cleanup();                     // 停止并释放当前已建立的资源。
            captureEnabled.store(false);   // 同步采集停止状态。
            return;                        // 结束线程。
        }
    }

    /*
     * ===== 第 6 步：启动视频流（VIDIOC_STREAMON） =====
     * 第三个参数是缓冲区类型的地址（enum v4l2_buf_type 变量的指针），而不是 v4l2_buffer。
     * STREAMON 前必须至少有缓冲区经过 QBUF 入队，否则驱动无处存放采集到的图像。
     */
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE; // 视频采集流类型
    if (ioctl(videoFd, VIDIOC_STREAMON, &type) < 0) // 启动摄像头 DMA，将画面写入已排队缓冲区。
    {
        logIoctlError("VIDIOC_STREAMON"); // 输出启动视频流失败原因。
        cleanup();                         // 取消映射、释放队列并关闭设备。
        captureEnabled.store(false);       // 同步采集状态。
        return;                            // 结束线程。
    }
    streaming = true; // 流已启动，cleanup 中需要执行 STREAMOFF

    /*
     * ===== 第 7 步：初始化三条消费者链路的固定 FPS 限速 =====
     * 三条链路只使用时间间隔限制帧发送频率。
     */
    QElapsedTimer localPreviewFrameTimer; // 记录距离上次发送本地预览帧经过的时间。
    localPreviewFrameTimer.start();       // 启动单调计时，之后 elapsed() 返回毫秒数。
    const qint64 localPreviewFrameIntervalMs = // 普通预览模式的最小帧间隔。
        1000 / VideoPipelineConfig::StandardFramesPerSecond; // 1000ms/30，整数结果约 33ms。
    const qint64 localPreviewStreamingFrameIntervalMs = // 推流期间为 UI 保留的更长预览帧间隔。
        1000 / VideoPipelineConfig::StreamingPreviewFramesPerSecond; // 1000ms/10 = 100ms。

    QElapsedTimer networkFrameTimer; // 记录距离上次向编码线程送帧经过的时间。
    networkFrameTimer.start();       // 启动网络链路计时器。
    const qint64 networkFrameIntervalMs = // 网络链路最小帧间隔。
        1000 / VideoPipelineConfig::StandardFramesPerSecond; // 目标上限为每秒 30 帧。

    QElapsedTimer analysisFrameTimer; // 记录距离上次向视觉线程送帧经过的时间。
    analysisFrameTimer.start();       // 启动分析链路计时器。
    const qint64 analysisFrameIntervalMs = // 图像分析链路最小帧间隔。
        1000 / VideoPipelineConfig::StandardFramesPerSecond; // 目标上限为每秒 30 帧。

    // copy 警告定时器：防止 QImage::copy() 失败时日志刷屏
    QElapsedTimer frameCopyWarningTimer;  // 警告计时器
    bool frameCopyWarningPrinted = false; // 本周期内是否已打印过警告

    /*
     * ===== 第 8 步：主采集循环 =====
     * 两个条件任何一个不满足都会安全退出：
     *   captureEnabled == false         用户点击了"停止采集"
     *   isInterruptionRequested() == true Qt 收到了线程中断请求（析构时触发）
     */
    while (captureEnabled.load() && !isInterruptionRequested()) // 总开关开启且未收到线程中断请求时持续采集。
    {
        /*
         * ===== 8.1 等待摄像头数据就绪（poll） =====
         * pollfd 描述要等待的文件描述符及事件：
         *   fd     等待 videoFd（/dev/video1）
         *   events POLLIN：等待设备产生可取出的采集数据
         *   revents poll() 返回时填充：实际发生的事件类型
         */
        struct pollfd descriptor = {}; // 清零初始化
        descriptor.fd = videoFd;       // 要等待的设备文件描述符
        descriptor.events = POLLIN;    // 关注"有数据可读"事件

        /*
         * poll(fds, nfds, timeout)：
         *   &descriptor 指向 pollfd 数组的指针（这里只含一个元素）
         *   1           数组元素数量
         *   200         最长等待 200 毫秒（超时后 poll 返回 0）
         * 返回值：<0 出错，=0 超时，>0 有事件发生（即摄像头有数据）
         */
        const int pollResult = poll(&descriptor, 1, 200); // 阻塞等待，最多 200ms
        if (pollResult < 0) // 负值表示 poll 系统调用失败。
        {
            // poll 返回值 < 0 表示出错
            if (errno == EINTR)    // 被信号打断不是摄像头故障，重新等待即可
                continue;          // 回到 while 循环开头
            logIoctlError("poll"); // 真正的 poll 错误
            break;                 // 退出 while 循环
        }
        // poll 返回 0 表示 200ms 超时（摄像头无新数据），重新检查 while 条件
        if (pollResult == 0) // 0 表示 200ms 内没有新帧，并非错误。
            continue;        // 返回循环顶部重新检查停止条件并继续等待。
        // 检查设备是否出现错误（POLLERR）、挂断（POLLHUP）或无效（POLLNVAL）
        // & 是按位与运算：检查 revents 中是否包含这些错误标志位
        if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) // 检测设备错误、断开或无效描述符事件。
        {
            qWarning("Camera poll failed, revents=0x%x", descriptor.revents); // 以十六进制输出内核返回事件位。
            break; // 设备错误不能继续采集，退出循环
        }

        /*
         * ===== 8.2 从驱动队列取出一帧（DQBUF） =====
         * DQBUF（Dequeue Buffer）：从驱动完成队列取出一帧已填充的图像数据。
         * DQBUF 前只需说明队列类型和内存方式；调用成功后驱动会把
         * buffer.index、bytesused、timestamp、sequence 等结果字段填好。
         */
        struct v4l2_buffer buffer = {};            // 清零初始化
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; // 指定操作视频采集队列
        buffer.memory = V4L2_MEMORY_MMAP;          // 指定缓冲区为 mmap 方式

        if (ioctl(videoFd, VIDIOC_DQBUF, &buffer) < 0) // 尝试取得一块已由摄像头填满的缓冲区。
        {
            /*
             * 两种可安全重试的错误：
             *   EAGAIN：非阻塞设备暂时没有完成的帧（因为前面已经 poll 过了，这里极少出现）
             *   EINTR：被信号打断
             */
            if (errno == EAGAIN || errno == EINTR) // 暂无帧或被信号打断时允许安全重试。
                continue; // 安全地重新进入循环
            logIoctlError("VIDIOC_DQBUF"); // 其他错误说明采集队列异常。
            break; // 不可恢复的错误
        }

        // 安全检查：驱动返回的缓冲区索引必须在有效范围内
        // || 是逻辑或：索引越界 或 缓冲区未映射 → 出错
        if (buffer.index >= bufferCount ||                         // 驱动返回的下标不能越过实际数组范围；或者
            buffers[buffer.index].start == MAP_FAILED)             // 对应缓冲区必须已经成功映射。
        {
            qWarning("Driver returned invalid capture buffer index %u", buffer.index); // 输出异常索引。
            break;                                                                // 数据地址不可信，停止循环。
        }

        /*
         * ===== 8.3 创建 QImage（零拷贝引用 mmap 缓冲区） =====
         * bytesperline 是驱动返回的每行实际跨度（字节数）。
         * 可能因内存对齐而大于 width * 2。若旧驱动返回 0，
         * 则按 RGB565 每像素 2 字节计算后备值。
         */
        const int bytesPerLine = format.fmt.pix.bytesperline != 0                    // 三元运算符：判断驱动是否返回了有效值
                                     ? static_cast<int>(format.fmt.pix.bytesperline) // 是 → 使用驱动返回的值
                                     : static_cast<int>(format.fmt.pix.width * 2);   // 否 → 后备计算：640 × 2 = 1280
        /*
         * QImage(data, width, height, bytesPerLine, format)：
         *   data         DQBUF 返回索引所对应的 mmap 缓冲区虚拟地址
         *                 注意：static_cast<const uchar*> 将 void* 转为 uchar*（QImage 要求的类型）
         *   width/height VIDIOC_S_FMT 后驱动确认的实际尺寸
         *   bytesPerLine 驱动缓冲区中相邻两行的字节跨度
         *   Format_RGB16 与 V4L2_PIX_FMT_RGB565 对应（每像素 16 bit）
         *
         * 关键：frame 只引用驱动缓冲区，并不拥有这些像素数据。
         * 这是浅引用而非深拷贝，必须在 QBUF 之前执行 frame.copy()。
         */
        const QImage frame( // frame 只包装当前 mmap 内存，不复制也不取得其所有权。
            static_cast<const uchar *>(buffers[buffer.index].start), // mmap 映射的虚拟地址
            static_cast<int>(format.fmt.pix.width),                  // 宽度：640
            static_cast<int>(format.fmt.pix.height),                 // 高度：480
            bytesPerLine,                                            // 行跨度
            QImage::Format_RGB16);                                   // 像素格式 RGB565
        // ===== 8.4 读取三个消费者的使能标志 =====
        const bool localDisplayEnabled = localPreviewEnabled.load(); // 本地预览是否启用
        const bool networkEnabled = networkStreamingEnabled.load();  // RTP/UDP 推流是否启用
        const bool visionAnalysisEnabled = analysisEnabled.load();   // 图像分析是否启用
        bool previewThisFrame = false;                               // 本帧是否发送给本地预览
        bool sendThisFrame = false;                                  // 本帧是否发送给网络
        bool analyzeThisFrame = false;                               // 本帧是否发送给图像分析

        /*
         * ===== 8.5 固定 FPS 限速 =====
         * 普通预览、网络推流、图像分析统一为 30 FPS；推流时预览为 10 FPS。
         */

        // 本地预览：普通模式 30 FPS，推流期间 10 FPS
        const qint64 activePreviewIntervalMs =              // 保存当前状态下实际采用的预览帧间隔。
            networkEnabled ? localPreviewStreamingFrameIntervalMs // 推流时选择 100ms，即 10 FPS，
                           : localPreviewFrameIntervalMs;           // 未推流时选择约 33ms，即 30 FPS。
        if (localDisplayEnabled &&                                  // 用户已开启本地预览，并且
            localPreviewFrameTimer.elapsed() >= activePreviewIntervalMs) // 距上次投递已达到所需间隔时，
            previewThisFrame = true;                                // 标记本帧需要交给主线程显示。
        // 网络推流：最多 30 FPS
        if (networkEnabled &&                                       // 网络推流或事件录像需要网络链路，并且
            networkFrameTimer.elapsed() >= networkFrameIntervalMs)  // 距上次编码帧已达到约 33ms 时，
            sendThisFrame = true;                                   // 标记本帧需要交给 TcpFrameSender。
        // 图像分析：最多 30 FPS
        if (visionAnalysisEnabled &&                                // 用户开启了变化检测，并且
            analysisFrameTimer.elapsed() >= analysisFrameIntervalMs) // 距上次分析帧已达到约 33ms 时，
            analyzeThisFrame = true;                                // 标记本帧需要交给视觉线程。

        /*
         * ===== 8.6 深拷贝 + 归还缓冲区（QBUF） =====
         * 只有任一消费者需要这一帧时才深拷贝一次。
         * frame.copy() 创建一份拥有独立内存的 QImage 副本。
         * 复制必须发生在 QBUF 之前：QBUF 后驱动可能立即用新帧数据覆盖
         * mmap 缓冲区，此时 frame 引用的原始数据就变成垃圾了。
         */
        QImage image;                                              // 声明空的 QImage（默认为 null）
        if (previewThisFrame || sendThisFrame || analyzeThisFrame) // 至少有一个消费者需要此帧
            image = frame.copy();                                  // 深拷贝：从 mmap 缓冲区复制到独立内存

        /*
         * 本帧处理完成后必须再次入队，把缓冲区所有权还给驱动。
         * 如果一直 DQBUF 而不 QBUF，可用缓冲区会耗尽（3个都被应用占着），
         * 驱动无处存放新图像，采集将停止。这里直接复用 DQBUF 返回的 buffer，
         * 其中 index 仍指向刚刚处理完的那个缓冲区。
         */
        if (ioctl(videoFd, VIDIOC_QBUF, &buffer) < 0) // 深拷贝完成后立即把原驱动缓冲区重新入队。
        {
            logIoctlError("VIDIOC_QBUF"); // 无法归还缓冲区时记录驱动错误。
            break; // QBUF 失败 → 停止采集
        }

        /*
         * ===== 8.7 空图像检查 =====
         * 内存不足时 QImage::copy() 会返回空图像（isNull() == true）。
         * 此时不能把空图像交给 QPixmap 或 JPEG 编码器。
         */
        if ((previewThisFrame || sendThisFrame || analyzeThisFrame) && // 本帧原本至少有一个消费者需要，并且
            image.isNull())                                            // 深拷贝结果为空时进入内存错误处理。
        {
            // 限速日志输出：最多每秒打印一次警告
            if (!frameCopyWarningPrinted || frameCopyWarningTimer.elapsed() >= 1000) // 首次失败或距上次日志已满 1 秒才打印。
            {
                qWarning() << "摄像头帧深拷贝失败，可用内存可能不足"; // 告知可能的内存耗尽原因。
                frameCopyWarningTimer.restart(); // 重启计时器
                frameCopyWarningPrinted = true;  // 标记已打印
            }
            continue; // 跳过本帧，回到 while 循环顶部
        }

        /*
         * ===== 8.8 发射分析帧信号 =====
         * 分析线程接收当前深拷贝图像，执行画面变化检测。
         */
        if (analyzeThisFrame) // 当前帧达到视觉分析分发间隔。
        {
            analysisFrameTimer.restart();   // 从本次发送重新计算分析帧间隔
            emit analysisFrameReady(image); // 发射信号 → VisionEventDetector::analyzeFrame()
        }

        /*
         * ===== 8.9 发射本地预览帧信号 =====
         * 本地显示使用当前深拷贝图像。
         * 这个信号发往主线程的 updateLocalPreview() 槽函数，
         * 通过 Qt::QueuedConnection 将 QImage 投递到主线程事件队列。
         */
        if (previewThisFrame) // 当前帧达到本地预览分发间隔。
        {
            localPreviewFrameTimer.restart(); // 从本次发送重新计算预览帧间隔
            emit localPreviewReady(image);    // 发射信号 → CameraStreamingPage::updateLocalPreview()
        }

        /*
         * ===== 8.10 发射网络推流帧信号 =====
         * 网络帧以固定 30 FPS 上限投递给编码发送线程。
         */
        if (sendThisFrame) // 当前帧达到网络编码分发间隔。
        {
            networkFrameTimer.restart();   // 从本次发送重新计算网络帧间隔
            emit networkFrameReady(image); // 发射信号 → TcpFrameSender::sendFrame()
        }

    } // while 循环结束

    /*
     * ===== 第 9 步：资源清理 =====
     * 正常停止（用户点击停止采集）和循环中的错误都汇聚到同一个资源清理出口。
     * cleanup() 执行：STREAMOFF → munmap → REQBUFS(0) → close
     */
    cleanup(); // 无论正常停止还是循环错误，都从此唯一出口释放全部 V4L2 资源。

    // 将公开状态同步为"采集已经结束"
    captureEnabled.store(false); // 采集已停止
#endif // __linux__ && __arm__
} // run() 结束
