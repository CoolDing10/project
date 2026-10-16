// 包含本模块的头文件声明
#include "h264_decoder.h" // 引入 H264Decoder 类声明。

// C 标准库字符串操作函数（memcpy 等）
#include <cstring> // 提供 memcpy()，把 QByteArray 复制进 AVPacket。

// FFmpeg C 库头文件，使用 extern "C" 包裹以避免 C++ 名称修饰问题
extern "C" // FFmpeg 是 C 库，禁止 C++ 名称修饰。
{
// FFmpeg 编解码核心 API：avcodec_find_decoder、avcodec_open2、avcodec_send_packet、avcodec_receive_frame 等
#include <libavcodec/avcodec.h> // 提供 H.264 解码上下文、帧、包和 send/receive API。
// FFmpeg 错误码定义和错误描述函数：AVERROR、av_strerror
#include <libavutil/error.h> // 提供错误码宏和 av_strerror()。
// FFmpeg 像素格式转换 API：sws_getCachedContext、sws_scale、sws_freeContext
#include <libswscale/swscale.h> // 提供任意解码像素格式到 RGB24 的转换。
}

// 匿名命名空间：工具函数仅当前 .cpp 文件可见
namespace // 辅助函数只在本实现文件内可见。
{
    // 将 FFmpeg 错误码转换为可读的错误描述字符串
    // @param code FFmpeg API 返回的负值错误码
    // @return 人类可读的错误描述字符串
    QString ffmpegError(int code) // code 是 FFmpeg 负错误码，返回可显示文本。
    {
        // FFmpeg 错误描述缓冲区
        char text[AV_ERROR_MAX_STRING_SIZE] = {0}; // text 接收 FFmpeg 写入的零结尾错误说明。
        // 调用 FFmpeg API 将错误码转为字符串
        av_strerror(code, text, sizeof(text)); // 按缓冲区容量安全转换错误码。
        // 转为 QString 返回
        return QString::fromLocal8Bit(text); // 转为 Qt 字符串供 UI 使用。
    }
}

// 构造函数：所有 FFmpeg 资源指针初始化为 nullptr（惰性初始化）
H264Decoder::H264Decoder() // 构造阶段不分配 FFmpeg 资源，采用惰性初始化。
    : context(nullptr),     // 解码器上下文为空
      frame(nullptr),       // 解码输出帧为空
      packet(nullptr),      // 输入数据包为空
      scaleContext(nullptr) // 像素转换器为空
{
}

// 析构函数：按序释放所有 FFmpeg 资源（先释放像素转换器，再释放包、帧、上下文）
H264Decoder::~H264Decoder() // 按转换器、包、帧、上下文的顺序释放资源。
{
    // 释放像素格式转换上下文（如果存在）
    if (scaleContext) // 转换器已经创建时，
        sws_freeContext(scaleContext); // 释放 libswscale 内部缓存。
    // 释放输入数据包（av_packet_free 同时释放包并置空指针）
    av_packet_free(&packet); // 释放包及其数据，并把指针置空。
    // 释放解码输出帧
    av_frame_free(&frame); // 释放复用输出帧并置空。
    // 释放解码器上下文
    avcodec_free_context(&context); // 关闭解码器、释放参考帧和上下文并置空。
}

// 初始化 FFmpeg H.264 解码器
// @param error 可选输出参数：失败时接收错误描述
// @return true 成功，false 失败
bool H264Decoder::initialize(QString *error) // error 可为空，非空时接收失败原因。
{
    // 如果已初始化，直接返回成功（幂等性保证）
    if (context) // 上下文存在表示已经成功初始化。
        return true; // 重复调用保持幂等，不重新分配。
    // 查找 H.264 解码器
    auto codec = avcodec_find_decoder(AV_CODEC_ID_H264); // codec 指向已注册的 H.264 解码器描述。
    if (!codec)                                          // 空指针表示当前 FFmpeg 不支持 H.264 解码。
    {
        // FFmpeg 未编译 H.264 解码支持
        if (error) // 调用者提供输出位置时，
            *error = QStringLiteral("FFmpeg 中未找到 H.264 解码器"); // 写入直接原因。
        return false; // 无法继续分配上下文。
    }
    // 分配解码器上下文
    context = avcodec_alloc_context3(codec); // 创建与所选解码器关联的运行上下文。
    // 分配解码输出帧
    frame = av_frame_alloc(); // 创建复用的解码输出帧容器。
    // 分配输入数据包
    packet = av_packet_alloc(); // 创建复用的压缩输入包容器。
    // 检查内存分配是否全部成功
    if (!context || !frame || !packet) // 任一内存分配失败都不能形成完整解码流水线。
    {
        if (error) // 调用者需要错误文本时，
            *error = QStringLiteral("FFmpeg 解码器内存分配失败"); // 报告资源分配失败。
        // 释放可能部分分配的资源
        av_packet_free(&packet);          // 释放可能已成功分配的包。
        av_frame_free(&frame);            // 释放可能已成功分配的帧。
        avcodec_free_context(&context);   // 释放可能已成功分配的上下文。
        return false;                     // 初始化失败。
    }
    // 启用低延迟解码标志
    context->flags |= AV_CODEC_FLAG_LOW_DELAY; // 优先减少内部缓冲带来的显示延迟。
    // 启用快速解码标志
    context->flags2 |= AV_CODEC_FLAG2_FAST; // 允许采用更快的非严格优化路径。
    // 解码线程数设为 1（单线程保证帧顺序，适合嵌入式场景）
    context->thread_count = 1; // 避免帧线程重排，保持实时流顺序简单。
    // 打开解码器（无额外选项）
    const int result = avcodec_open2(context, codec, nullptr); // result 为 0 表示解码器成功打开。
    if (result < 0)                                           // 负值表示参数或运行环境错误。
    {
        // 解码器打开失败
        if (error) // 调用者需要错误说明时，
            *error = QStringLiteral("H.264 解码器初始化失败：%1").arg(ffmpegError(result)); // 填入 FFmpeg 原因。
        av_packet_free(&packet);        // 回收输入包。
        av_frame_free(&frame);          // 回收输出帧。
        avcodec_free_context(&context); // 回收失败的上下文。
        return false;                   // 初始化失败。
    }
    return true; // 解码器、帧和包均准备完成。
}

// 重置解码器内部状态：刷新内部缓冲区
void H264Decoder::reset() // 清除依赖旧流或丢失参考帧的内部状态。
{
    // 刷新解码器内部缓冲区（丢帧后需要清除旧的参考帧和状态）
    if (context) // 只有已打开上下文才可刷新，
        avcodec_flush_buffers(context); // 丢弃缓存包、参考图像和延迟输出。
}

// 解码一帧 H.264 访问单元为 QImage
// @param accessUnit H.264 访问单元（一帧完整的编码数据）
// @param image 输出参数：解码后的 QImage（RGB888）
// @param error 可选输出参数：失败时接收错误描述
// @return true 成功产生图像，false 失败
bool H264Decoder::decode(const QByteArray &accessUnit, QImage *image, QString *error) // 输入一帧压缩数据，输出 RGB 图像和可选错误。
{
    // 输出参数空检查、输入数据空检查、解码器未初始化则尝试初始化
    if (!image || accessUnit.isEmpty() || (!context && !initialize(error))) // 输出指针、输入数据及初始化状态必须都有效。
        return false; // 参数或初始化失败时不访问后续 FFmpeg 资源。
    // 释放 packet 中的旧数据引用（避免内存泄漏）
    av_packet_unref(packet); // 释放上次输入的引用，复用同一个 AVPacket 对象。
    // 为 packet 分配 accessUnit 大小的新缓冲区
    const int allocation = av_new_packet(packet, accessUnit.size()); // 分配与访问单元等长并含 FFmpeg padding 的包缓冲区。
    if (allocation < 0)                                              // 负值表示内存分配失败。
    {
        // 内存分配失败
        if (error) // 调用者提供错误输出时，
            *error = ffmpegError(allocation); // 写入分配错误文本。
        return false; // 包没有有效缓冲区，不能复制。
    }
    // 将 H.264 码流数据拷贝到 packet 的数据缓冲区
    memcpy(packet->data, accessUnit.constData(), size_t(accessUnit.size())); // 把 Qt 字节完整复制到 FFmpeg 管理的包内存。
    // 将编码数据包送入解码器
    int result = avcodec_send_packet(context, packet); // result 保存解码器是否接受本访问单元。
    if (result < 0)                                    // 负值表示码流或解码器状态错误。
    {
        // 送入失败
        if (error) // 调用者需要错误说明时，
            *error = QStringLiteral("avcodec_send_packet: %1").arg(ffmpegError(result)); // 标明失败 API。
        return false; // 输入未被接受，不进入取帧循环。
    }

    // 是否产生了图像输出（一次 send_packet 可能产生多帧）
    bool produced = false; // 最终返回是否至少取得并转换了一帧图像。
    // 循环从解码器取出解码后的帧
    while (true) // 一个访问单元理论上可能产生多个解码帧，持续读取到 EAGAIN。
    {
        // 从解码器接收解码后的帧
        result = avcodec_receive_frame(context, frame); // 复用 frame 接收下一张解码图像。
        // EAGAIN：解码器需要更多输入数据；EOF：已刷新完毕
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) // 需要更多输入或流已结束时，
            break;                                             // 正常结束本次取帧循环。
        if (result < 0)                                        // 其他负值是真正解码错误。
        {
            // 解码错误
            if (error) // 调用者提供输出位置时，
                *error = QStringLiteral("avcodec_receive_frame: %1").arg(ffmpegError(result)); // 写入取帧错误。
            return false; // 不返回可能不完整的图像。
        }
        // 创建 RGB888 格式的目标 QImage，尺寸与解码帧一致
        QImage decoded(frame->width, frame->height, QImage::Format_RGB888); // 分配目标尺寸的三通道 RGB 图像内存。
        // 获取或创建像素格式转换上下文（YUV420P → RGB888）
        // sws_getCachedContext 会复用已有 context 如果参数未变
        scaleContext = sws_getCachedContext( // 参数不变时复用转换器，分辨率或格式变化时自动更新。
            scaleContext,                              // 现有的转换上下文（可为 nullptr）
            frame->width,                              // 源宽度
            frame->height,                             // 源高度
            static_cast<AVPixelFormat>(frame->format), // 源像素格式（通常为 YUV420P）
            frame->width,                              // 目标宽度（与源相同，不做缩放）
            frame->height,                             // 目标高度
            AV_PIX_FMT_RGB24,                          // 目标像素格式：RGB888
            SWS_FAST_BILINEAR,                         // 快速双线性插值算法
            nullptr, nullptr, nullptr);                // 无额外参数
        // 检查转换上下文和图像是否有效
        if (!scaleContext || decoded.isNull()) // 转换器创建失败或 QImage 像素内存分配失败。
        {
            if (error) // 调用者需要错误说明时，
                *error = QStringLiteral("解码图像格式转换失败"); // 写入统一的转换阶段错误。
            return false; // 无法安全执行 sws_scale。
        }
        // 设置目标数据指针数组：RGB 只有平面 0
        uint8_t *destination[4] = {decoded.bits(), nullptr, nullptr, nullptr}; // RGB24 只使用第 0 输出平面。
        // 设置目标行跨度数组
        int lines[4] = {decoded.bytesPerLine(), 0, 0, 0}; // 第 0 项是目标 QImage 每行实际字节跨度。
        // 执行像素格式转换：YUV420P → RGB888
        sws_scale(scaleContext, frame->data, frame->linesize, 0, frame->height, // 转换解码帧全部有效行，
                  destination, lines);                                         // 输出到 QImage RGB 缓冲区。
        // 将转换后的图像赋给输出参数
        *image = decoded; // 通过 QImage 隐式共享把结果交给调用者。
        // 标记已产生输出
        produced = true; // 标记本次输入至少产生了可显示图像。
    }
    // 返回是否成功产出了图像
    return produced; // EAGAIN 且未产帧时返回 false，否则返回 true。
}
