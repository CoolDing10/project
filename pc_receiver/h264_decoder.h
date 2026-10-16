// 防止头文件被重复包含的头文件保护宏
#ifndef H264_DECODER_H // 如果尚未包含过本头文件，才编译以下声明。
#define H264_DECODER_H // 定义保护宏，避免 H264Decoder 重复声明。

// Qt 字节数组类，用于传递 H.264 码流数据
#include <QByteArray> // decode() 接收 H.264 访问单元的字节容器。
// Qt 图像类，用于返回解码后的视频帧
#include <QImage> // decode() 输出 RGB888 图像的 Qt 类型。
// Qt 字符串类，用于返回错误信息
#include <QString> // initialize()/decode() 输出可读错误信息。

// 前向声明：FFmpeg 编解码器上下文，管理 H.264 解码参数和状态
struct AVCodecContext; // 仅保存指针，完整 FFmpeg 定义放在 .cpp 中。
// 前向声明：FFmpeg 视频帧，存储解码后的 YUV 原始帧数据
struct AVFrame; // 前向声明复用的解码输出帧。
// 前向声明：FFmpeg 数据包，存储输入的 H.264 压缩数据
struct AVPacket; // 前向声明复用的压缩输入包。
// 前向声明：FFmpeg 像素格式转换上下文，用于 YUV → RGB 转换
struct SwsContext; // 前向声明 YUV 到 RGB 的转换上下文。

// H.264 解码器类：使用 FFmpeg 将 H.264 Annex B 码流解码为 QImage（RGB888 格式）
class H264Decoder // 封装 FFmpeg H.264 解码与 QImage 格式转换资源。
{
public: // 对接收窗口开放的生命周期和逐帧解码接口。
    // 构造函数：初始化所有 FFmpeg 资源指针为 nullptr
    H264Decoder(); // 把四个 FFmpeg 资源指针初始化为空，等待首次解码。

    // 析构函数：释放所有 FFmpeg 资源
    ~H264Decoder(); // 释放转换器、输入包、输出帧和解码器上下文。

    // 初始化 FFmpeg H.264 解码器（仅在首次解码或 reset 后调用，重复调用无副作用）
    // @param error 可选输出参数：初始化失败时接收错误描述字符串
    // @return true 初始化成功，false 失败
    bool initialize(QString *error = nullptr); // error 可为空；非空时接收初始化失败原因。

    // 重置解码器内部状态（刷新内部缓冲区），通常在丢帧或流切换时调用
    void reset(); // 清空解码器参考帧和内部缓存，用于流切换或丢帧恢复。

    // 解码一帧 H.264 访问单元
    // @param accessUnit H.264 访问单元（一帧完整的编码数据，可包含多个 NAL 单元）
    // @param image 输出参数：解码后的 QImage（RGB888 格式）
    // @param error 可选输出参数：解码失败时接收错误描述字符串
    // @return true 解码成功并产生图像，false 解码失败
    bool decode(const QByteArray &accessUnit, QImage *image, // accessUnit 是完整压缩帧，image 接收 RGB 图像。
                QString *error = nullptr); // error 可选接收具体 FFmpeg 错误说明。

private: // 本类独占并复用的 FFmpeg C 资源指针。
    // FFmpeg 解码器上下文：存储解码器配置和状态
    AVCodecContext *context; // 保存 H.264 解码器参数、参考帧及内部状态。
    // FFmpeg 解码输出帧：存储解码后的 YUV420P 原始数据
    AVFrame *frame; // 接收解码器输出的 YUV 像素平面。
    // FFmpeg 输入数据包：存储待解码的 H.264 压缩数据
    AVPacket *packet; // 保存复制后的单帧 H.264 输入字节。
    // FFmpeg 像素格式转换上下文：YUV420P → RGB888 转换
    SwsContext *scaleContext; // 缓存源像素格式到 RGB24 的转换参数。
};

// 头文件保护宏结束
#endif // H264_DECODER_H：结束头文件保护。
