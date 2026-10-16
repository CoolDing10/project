/*
 * vision_event_detector.cpp — 相邻帧差分画面变化检测
 *
 * 算法步骤：
 *   1. 把输入帧缩小到 160×120 并转成灰度图；
 *   2. 逐像素计算当前帧与上一帧灰度值的绝对差；
 *   3. 差值达到 30 的像素记为“发生变化”；
 *   4. 变化像素比例连续 2 帧达到配置阈值时产生事件；
 *   5. 事件产生后进入 10 秒冷却期，避免同一动作重复报警。
 */
#include "vision_event_detector.h" // 引入检测器类、成员变量和信号槽声明。

#include <QDebug>   // 输出检测配置和异常图像日志。
#include <QtGlobal> // 提供 qBound() 等 Qt 通用工具函数。

#include <cstdlib> // 提供 std::abs() 整数绝对值函数。

namespace // 匿名命名空间使下面常量只在当前 .cpp 内可见。
{
constexpr int AnalysisWidth = 160; // 分析图宽度；缩小图像可显著减少逐像素计算量。
constexpr int AnalysisHeight = 120; // 分析图高度；160×120 一帧只有 19200 个像素。
constexpr int PixelDifferenceThreshold = 30; // 单像素灰度差达到 30 才认为真的发生变化。
constexpr int RequiredConsecutiveFrames = 2; // 必须连续两帧超过比例阈值，过滤单帧抖动。
constexpr int VisualEventCooldownMs = 10000; // 两次视觉事件至少间隔 10000ms，即 10 秒。
} // 结束仅供本实现文件使用的常量区域。

/**
 * 创建画面变化检测器。
 *
 * @param parent Qt 父对象。实际项目传 nullptr，随后将对象移动到分析线程。
 */
VisionEventDetector::VisionEventDetector(QObject *parent)
    : QObject(parent),              // 把父对象传给 QObject，建立可选的父子生命周期关系。
      detectionEnabled(false),      // 默认不分析画面，必须由 configure() 显式开启。
      changedRatioThreshold(0.12),  // 默认变化比例阈值是 12%，即 0.12。
      consecutiveChangedFrames(0)   // 刚创建时还没有连续超阈值帧。
{
} // previousGrayFrame 默认是空图，eventCooldownTimer 默认尚未启动。

/**
 * 更新检测开关和变化比例阈值。
 *
 * @param enabled 是否启用实际帧差计算。
 * @param changedPixelPercent UI 输入的百分比整数，例如 12 表示 12%。
 */
void VisionEventDetector::configure(bool enabled, int changedPixelPercent)
{
    detectionEnabled = enabled;                                 // 保存最新检测开关。
    changedRatioThreshold =                                     // 把 UI 百分比转换为 0.0～1.0 比例。
        qBound(1, changedPixelPercent, 90) / 100.0;              // 先限制到 1～90，再用浮点数除以 100。
    consecutiveChangedFrames = 0;                               // 配置改变后重新累计连续变化帧。
    if (!enabled)                                               // 关闭检测时不应保留旧帧作为以后比较基准。
        previousGrayFrame = QImage();                           // 赋空图像，释放/解除旧灰度帧数据。

    qInfo() << "视觉事件检测配置：启用=" << enabled            // 输出最终启用状态。
            << "变化像素阈值=" << changedPixelPercent << '%';  // 输出用户输入百分比供调试。
}

/**
 * 对一帧摄像头图像执行变化检测。
 *
 * @param frame CaptureThread 分发的输入帧。函数不修改该对象；
 *              跨线程排队连接已确保帧数据在本函数执行期间有效。
 */
void VisionEventDetector::analyzeFrame(const QImage &frame)
{
    if (frame.isNull())                                         // 空 QImage 没有尺寸和像素数据，不能分析。
    {
        qWarning() << "视觉检测器收到空图像";                  // 输出上游采集或内存分配异常。
        return;                                                 // 立即结束，避免访问空图像扫描线。
    }

    if (!detectionEnabled)                                      // 关闭检测时跳过缩放和逐像素计算。
    {
        previousGrayFrame = QImage();                           // 清空基准，防止重启后拿很旧的帧比较。
        emit analysisCompleted(frame, 0.0);                     // 仍回传原图，让 UI 知道该帧已处理。
        return;                                                 // 当前帧不再参与后续检测逻辑。
    }

    const QImage grayFrame = frame                              // 从本次输入图像开始构造分析图。
        .scaled(AnalysisWidth,                                  // 把宽度固定为 160。
                AnalysisHeight,                                 // 把高度固定为 120。
                Qt::IgnoreAspectRatio,                          // 强制得到固定尺寸，便于与上一帧逐点比较。
                Qt::FastTransformation)                         // 使用快速缩放，降低 i.MX6ULL CPU 消耗。
        .convertToFormat(QImage::Format_Grayscale8);            // 转为每像素 8 位灰度，只比较亮度。
    if (grayFrame.isNull())                                     // 内存不足等异常可能导致缩放或转换失败。
    {
        qWarning() << "视觉检测缩放或灰度转换失败";            // 记录本帧无法分析的原因。
        return;                                                 // 不覆盖旧基准帧，也不发出错误结果。
    }

    int changedPixelCount = 0;                                  // 统计与上一帧相比发生明显变化的像素数。
    const int totalPixelCount =                                 // 计算当前分析图的总像素数。
        grayFrame.width() * grayFrame.height();                  // 160×120 正常结果为 19200。
    if (!previousGrayFrame.isNull())                            // 第一帧没有上一帧，只能保存为比较基准。
    {
        for (int row = 0; row < grayFrame.height(); ++row)      // 从第 0 行遍历到最后一行。
        {
            const uchar *currentLine =                          // currentLine 指向当前灰度图本行首字节。
                grayFrame.constScanLine(row);                    // constScanLine() 只读访问，不触发图像分离。
            const uchar *previousLine =                         // previousLine 指向上一灰度图对应行。
                previousGrayFrame.constScanLine(row);            // 两张图尺寸固定相同，可按相同列索引比较。
            for (int column = 0;                                // 从当前行第 0 个像素开始。
                 column < grayFrame.width();                    // 到达本行宽度后结束。
                 ++column)                                     // 每轮移动到下一个灰度像素。
            {
                if (std::abs(                                   // 计算两帧同一像素的亮度变化绝对值。
                        int(currentLine[column]) -               // 当前灰度值先转 int，避免 uchar 运算问题。
                        int(previousLine[column])) >=            // 减去上一帧同位置灰度值并取绝对值。
                    PixelDifferenceThreshold)                   // 差值至少为 30 才排除成像小噪声。
                    ++changedPixelCount;                        // 当前像素计入变化像素总数。
            }
        }
    }

    const double changedRatio =                                 // 计算本帧的变化像素比例。
        totalPixelCount > 0                                     // 防御异常零尺寸图像导致除以零。
            ? double(changedPixelCount) / totalPixelCount       // 正常时用变化数量除以总数量。
            : 0.0;                                              // 无像素时把变化比例定义为 0。
    previousGrayFrame = grayFrame;                              // 当前灰度帧成为下一次分析的比较基准。
    emit analysisCompleted(frame, changedRatio);                // 把原图和比例送回页面用于显示。

    if (changedRatio >= changedRatioThreshold)                  // 当前变化比例达到用户配置阈值。
        ++consecutiveChangedFrames;                             // 连续变化帧计数加一。
    else                                                        // 当前帧没有达到阈值。
        consecutiveChangedFrames = 0;                           // 连续性被打断，从零重新累计。

    const bool cooldownExpired =                                // 判断现在是否允许再次产生事件。
        !eventCooldownTimer.isValid() ||                         // 从未产生过事件时定时器无效，允许触发。
        eventCooldownTimer.elapsed() >= VisualEventCooldownMs;  // 或距离上次事件已经至少 10 秒。
    if (cooldownExpired &&                                      // 首先要求不在事件冷却期。
        consecutiveChangedFrames >= RequiredConsecutiveFrames)  // 同时要求连续两帧达到变化阈值。
    {
        eventCooldownTimer.restart();                           // 从本次事件重新开始计算 10 秒冷却期。
        consecutiveChangedFrames = 0;                           // 事件已消费当前累计，下一事件重新计数。
        emit visualMotionDetected(changedRatio);                // 通知页面记录事件并触发 H.264 录像。
    }
}
