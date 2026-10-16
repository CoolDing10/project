/*
 * opencv.h — “图像分析”页面声明
 *
 * OpenCvDemoPage 加载一张本地图片，并提供灰度化、直方图、均衡化、锐化
 * 以及多种边缘检测算法。该页面只处理静态图片，不接收实时摄像头帧。
 */
#ifndef OPENCV_H // 防止本头文件被重复包含。
#define OPENCV_H // 定义头文件保护宏。

#include <QWidget> // OpenCvDemoPage 的 Qt 页面基类。

#include <opencv2/core.hpp> // 提供 cv::Mat 图像矩阵类型和基础 OpenCV 数据结构。

class QLabel;       // 前向声明图像显示标签，减少头文件依赖。
class QPushButton;  // 前向声明图片选择按钮。

class OpenCvDemoPage final : public QWidget // final 表示该页面不允许继续派生。
{
    Q_OBJECT // 启用 Qt 元对象系统，使按钮可以连接到 private slots。

public:
    /**
     * 创建图像分析页面、控件和按钮，并尝试加载默认测试图片。
     * @param parent Qt 父控件，负责页面的对象树生命周期。
     */
    explicit OpenCvDemoPage(QWidget *parent = nullptr);

    /** 使用 QWidget 默认析构行为，子控件由 Qt 父子对象机制自动释放。 */
    ~OpenCvDemoPage() override = default;

private slots:
    /** 打开文件选择框，并把用户选择的图片设为当前源图。 */
    void selectImage();

    /** 将当前源图转换为灰度图并显示。 */
    void convertToGrayscale();

    /** 统计 256 个灰度级的像素数量并绘制灰度直方图。 */
    void showGrayscaleHistogram();

    /** 对灰度图执行直方图均衡化，提高整体对比度。 */
    void equalizeHistogram();

    /** 使用一阶梯度结果增强源图细节。 */
    void applyGradientSharpening();

    /** 使用 Laplacian 二阶导数增强源图细节。 */
    void applyLaplacianSharpening();

    /** 使用 Roberts 交叉算子检测边缘。 */
    void detectRobertsEdges();

    /** 使用 Sobel 算子检测水平和垂直边缘。 */
    void detectSobelEdges();

    /** 使用 Prewitt 算子检测水平和垂直边缘。 */
    void detectPrewittEdges();

    /** 使用 Laplacian 算子检测二阶变化边缘。 */
    void detectLaplacianEdges();

    /** 使用 Canny 多阶段算法检测边缘。 */
    void detectCannyEdges();

    /** 使用八方向 Kirsch 模板检测最强方向边缘。 */
    void detectKirschEdges();

private:
    /**
     * 从磁盘读取图片，并更新 sourceImage、grayscaleImage 和预览。
     * @param filePath 要读取的图片路径。
     * @return OpenCV 成功解码图片返回 true，否则返回 false。
     */
    bool loadSourceImage(const QString &filePath);

    /**
     * 把 OpenCV 图像转换为 QImage/QPixmap 后显示到指定标签。
     * @param image 要显示的 cv::Mat，支持 BGR 三通道或灰度单通道。
     * @param label 目标 QLabel 指针。
     */
    void showImage(const cv::Mat &image, QLabel *label);

    /**
     * 将边缘图显示在主结果区，并清空其他辅助结果区。
     * @param edgeImage 已经计算完成的单通道边缘图。
     */
    void showEdgeResult(const cv::Mat &edgeImage);

    /**
     * 统一启用或禁用全部图像处理按钮。
     * @param enabled true 允许处理，false 表示当前没有有效源图。
     */
    void setProcessingControlsEnabled(bool enabled);

    cv::Mat sourceImage;    // 当前彩色源图，OpenCV 默认以 BGR 通道顺序保存。
    cv::Mat grayscaleImage; // 从 sourceImage 转换得到的 8 位单通道灰度图。

    QLabel *sourceLabel;          // 显示当前源图。
    QLabel *primaryResultLabel;   // 显示主要处理结果或边缘图。
    QLabel *secondaryResultLabel; // 显示第二个辅助结果。
    QLabel *histogramLabel;       // 显示绘制出的灰度直方图。
    QPushButton *selectImageButton; // 打开本地图片选择框的按钮。
};

#endif // OPENCV_H：结束头文件保护。
