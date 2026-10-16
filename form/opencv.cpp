/*
 * opencv.cpp — 静态图片预处理与边缘检测页面实现
 *
 * 页面使用 cv::Mat 保存图像，使用 QLabel 显示结果。OpenCV 默认彩色顺序
 * 是 BGR，而 QImage::Format_RGB888 需要 RGB，因此显示前必须转换通道顺序。
 */
#include "opencv.h" // 引入 OpenCvDemoPage 类声明和 cv::Mat 成员。

#include <QCoreApplication> // 获取程序目录，寻找默认测试图片。
#include <QDebug>           // 保留 Qt 调试输出能力。
#include <QDir>             // 拼接和清理候选图片路径。
#include <QFileDialog>      // 打开本地图片选择窗口。
#include <QFileInfo>        // 判断候选文件是否存在并取得绝对路径。
#include <QGridLayout>      // 用 2×2 网格排列四个图像显示标签。
#include <QGroupBox>        // 将预处理按钮和边缘检测按钮分组。
#include <QHBoxLayout>      // 水平排列图像区和控制区。
#include <QImage>           // 把 OpenCV 像素包装成 Qt 图像。
#include <QLabel>           // 显示原图、处理结果、辅助结果和直方图。
#include <QMessageBox>      // 图片加载失败时显示提示框。
#include <QPushButton>      // 创建图片选择和算法按钮。
#include <QVBoxLayout>      // 垂直排列按钮和分组。

#include <opencv2/imgcodecs.hpp> // 使用 cv::imread() 解码图片。
#include <opencv2/imgproc.hpp>   // 使用颜色转换、卷积、直方图和边缘检测算法。

#include <algorithm> // 提供项目依赖的标准算法声明。
#include <array>     // 用固定长度 std::array 保存八个 Kirsch 卷积核。

namespace // 匿名命名空间中的辅助函数只在当前实现文件可见。
{
/**
 * 在开发运行和部署可能使用的几个根目录中寻找默认图片。
 * @return 找到时返回规范路径；均未找到时返回第一个候选路径供错误提示使用。
 */
QString projectImagePath()
{
    const QString relativePath = QStringLiteral("opencv_src/01.jpg"); // 默认图片相对项目根目录的位置。
    const QStringList roots = {                                      // 按优先顺序保存可能的项目根目录。
        QCoreApplication::applicationDirPath(),                      // 1. 可执行程序所在目录。
        QDir::currentPath(),                                         // 2. 启动程序时的当前工作目录。
        QDir::cleanPath(QCoreApplication::applicationDirPath() +     // 3. 从构建输出目录回到 project。
                        QStringLiteral("/../project")),
        QDir::cleanPath(QDir::currentPath() +                        // 4. 从当前目录的上级寻找 project。
                        QStringLiteral("/../project"))};

    for (const QString &root : roots)                                // 依次检查每个可能的根目录。
    {
        const QString candidate = QDir(root).filePath(relativePath); // 拼出本轮候选图片路径。
        if (QFileInfo::exists(candidate))                            // 文件真实存在时结束搜索。
            return QDir::cleanPath(candidate);                       // 返回去除多余 . 和 .. 的路径。
    }
    return QDir(roots.first()).filePath(relativePath);               // 未找到时返回默认候选路径。
}

/**
 * 把带符号的梯度矩阵转换为可显示的 8 位绝对幅值图。
 * @param gradient Sobel、Laplacian 或 filter2D 产生的 CV_16S 梯度。
 * @return CV_8U 的绝对梯度图。
 */
cv::Mat absoluteGradient(const cv::Mat &gradient)
{
    cv::Mat result;                              // 接收转换后的 8 位绝对值矩阵。
    cv::convertScaleAbs(gradient, result);       // 求绝对值、缩放并饱和转换到 8 位。
    return result;                               // cv::Mat 按值返回使用引用计数，复制成本较低。
}

/**
 * 使用水平和垂直两个卷积核计算边缘并按相同比例合并。
 * @param gray 输入的 8 位单通道灰度图。
 * @param horizontalKernel 水平方向卷积核。
 * @param verticalKernel 垂直方向卷积核。
 * @return 两个方向绝对梯度各占 0.5 的合成边缘图。
 */
cv::Mat combinedKernelEdges(const cv::Mat &gray,
                            const cv::Mat &horizontalKernel,
                            const cv::Mat &verticalKernel)
{
    cv::Mat horizontal; // 保存水平卷积产生的带符号 16 位响应。
    cv::Mat vertical;   // 保存垂直卷积产生的带符号 16 位响应。
    cv::filter2D(gray, horizontal, CV_16S, horizontalKernel); // 使用水平核对灰度图卷积。
    cv::filter2D(gray, vertical, CV_16S, verticalKernel);     // 使用垂直核对灰度图卷积。
    cv::Mat result;                                          // 保存两个方向合并后的 8 位边缘图。
    cv::addWeighted(absoluteGradient(horizontal), 0.5,        // 水平绝对梯度权重为 0.5。
                    absoluteGradient(vertical), 0.5,          // 垂直绝对梯度权重为 0.5。
                    0.0, result);                             // 不增加常量偏移，输出到 result。
    return result;                                           // 返回合成边缘图。
}
} // 结束当前文件专用辅助函数区域。

/**
 * 创建图像分析页面。
 * @param parent Qt 父控件；主窗口使用 QScrollArea 承载本页面。
 */
OpenCvDemoPage::OpenCvDemoPage(QWidget *parent)
    : QWidget(parent),                                                // 初始化 QWidget 基类。
      sourceLabel(new QLabel(QStringLiteral("原图"), this)),          // 创建源图显示标签。
      primaryResultLabel(new QLabel(QStringLiteral("处理结果"), this)), // 创建主要结果标签。
      secondaryResultLabel(new QLabel(QStringLiteral("辅助结果"), this)), // 创建辅助结果标签。
      histogramLabel(new QLabel(QStringLiteral("直方图"), this)),     // 创建直方图标签。
      selectImageButton(new QPushButton(QStringLiteral("选择图片"), this)) // 创建图片选择按钮。
{
    setObjectName(QStringLiteral("opencvPage")); // 设置对象名，供下面 QSS 精确选择本页面。
    setStyleSheet(QStringLiteral(                // 安装页面内部使用的暗色样式表。
        "#opencvPage { background:#1b2028; color:#eef2f7; }" // 页面背景和默认文字颜色。
        "QLabel { background:#10151c; color:#8fa0b3; border:1px solid #354150; border-radius:8px; }" // 图像框。
        "QGroupBox { color:#6dc7ff; border:1px solid #354150; border-radius:7px; margin-top:10px; }" // 分组框。
        "QGroupBox::title { subcontrol-origin:margin; left:7px; padding:0 3px; }" // 分组标题位置。
        "QPushButton { min-height:30px; background:#303a47; color:#eef2f7; border:none; border-radius:4px; padding:2px 6px; }" // 按钮。
        "QPushButton:hover { background:#1677b8; }"           // 鼠标悬停按钮颜色。
        "QPushButton:disabled { background:#252c35; color:#6f7c8b; }")); // 禁用按钮颜色。

    const QList<QLabel *> imageLabels = {                         // 收集需要统一设置的四个显示标签。
        sourceLabel, primaryResultLabel, secondaryResultLabel, histogramLabel}; // 标签指针列表。
    for (QLabel *label : imageLabels)                             // 逐个应用统一显示属性。
    {
        label->setAlignment(Qt::AlignCenter);                     // 无图或图片都居中显示。
        label->setMinimumSize(210, 190);                          // 保证每个结果区域可辨认。
        label->setScaledContents(true);                           // 让 QPixmap 随 QLabel 尺寸缩放。
    }

    QGridLayout *imageLayout = new QGridLayout;                   // 创建 2×2 图像结果网格。
    imageLayout->addWidget(sourceLabel, 0, 0);                    // 左上显示原图。
    imageLayout->addWidget(primaryResultLabel, 0, 1);             // 右上显示主要结果。
    imageLayout->addWidget(secondaryResultLabel, 1, 0);           // 左下显示辅助结果。
    imageLayout->addWidget(histogramLabel, 1, 1);                 // 右下显示灰度直方图。

    QGroupBox *preprocessGroup =                                 // 创建图像预处理按钮分组。
        new QGroupBox(QStringLiteral("图像预处理"), this);        // 组标题为“图像预处理”。
    QVBoxLayout *preprocessLayout =                              // 创建预处理组的垂直按钮布局。
        new QVBoxLayout(preprocessGroup);                        // 布局父对象为预处理分组。
    const QList<QPair<QString, void (OpenCvDemoPage::*)()>>      // 每项保存按钮文字和成员槽函数指针。
        preprocessActions = {
            {QStringLiteral("灰度处理"), &OpenCvDemoPage::convertToGrayscale}, // 灰度转换按钮。
            {QStringLiteral("灰度直方图"), &OpenCvDemoPage::showGrayscaleHistogram}, // 直方图按钮。
            {QStringLiteral("直方图均衡"), &OpenCvDemoPage::equalizeHistogram}, // 均衡化按钮。
            {QStringLiteral("梯度锐化"), &OpenCvDemoPage::applyGradientSharpening}, // 梯度锐化按钮。
            {QStringLiteral("Laplacian锐化"), &OpenCvDemoPage::applyLaplacianSharpening}}; // 二阶锐化按钮。
    for (const auto &action : preprocessActions)                 // 为每个预处理动作创建按钮。
    {
        QPushButton *button =                                   // button 是当前动作对应的临时按钮指针。
            new QPushButton(action.first, preprocessGroup);     // action.first 是显示文字。
        connect(button, &QPushButton::clicked,                   // 当前按钮 clicked 信号。
                this, action.second);                            // action.second 是对应成员槽。
        preprocessLayout->addWidget(button);                     // 将按钮加入预处理分组。
    }

    QGroupBox *edgeGroup =                                      // 创建边缘检测按钮分组。
        new QGroupBox(QStringLiteral("边缘检测"), this);         // 组标题为“边缘检测”。
    QVBoxLayout *edgeLayout = new QVBoxLayout(edgeGroup);       // 创建边缘检测组垂直布局。
    const QList<QPair<QString, void (OpenCvDemoPage::*)()>>      // 保存算法名称和成员槽函数指针。
        edgeActions = {
            {QStringLiteral("Roberts"), &OpenCvDemoPage::detectRobertsEdges}, // Roberts 算子。
            {QStringLiteral("Sobel"), &OpenCvDemoPage::detectSobelEdges}, // Sobel 算子。
            {QStringLiteral("Prewitt"), &OpenCvDemoPage::detectPrewittEdges}, // Prewitt 算子。
            {QStringLiteral("Laplacian"), &OpenCvDemoPage::detectLaplacianEdges}, // Laplacian 算子。
            {QStringLiteral("Canny"), &OpenCvDemoPage::detectCannyEdges}, // Canny 算法。
            {QStringLiteral("Kirsch"), &OpenCvDemoPage::detectKirschEdges}}; // Kirsch 八方向算法。
    for (const auto &action : edgeActions)                       // 为每种边缘算法创建按钮。
    {
        QPushButton *button =                                   // 当前边缘算法对应的按钮。
            new QPushButton(action.first, edgeGroup);           // 使用算法名称作为按钮文字。
        connect(button, &QPushButton::clicked,                   // 监听当前按钮点击。
                this, action.second);                            // 调用对应边缘检测成员函数。
        edgeLayout->addWidget(button);                            // 将按钮加入边缘分组布局。
    }

    connect(selectImageButton, &QPushButton::clicked,            // “选择图片”按钮 clicked 信号。
            this, &OpenCvDemoPage::selectImage);                 // 打开文件选择框。

    QVBoxLayout *controlLayout = new QVBoxLayout;                // 创建页面右侧控制区布局。
    controlLayout->addWidget(selectImageButton);                 // 顶部放图片选择按钮。
    controlLayout->addWidget(preprocessGroup);                   // 其下放预处理分组。
    controlLayout->addWidget(edgeGroup);                         // 再放边缘检测分组。
    controlLayout->addStretch();                                 // 把控件推到控制区顶部。

    QHBoxLayout *pageLayout = new QHBoxLayout(this);             // 创建页面根水平布局。
    pageLayout->addLayout(imageLayout, 1);                       // 图像区获得可伸缩空间。
    pageLayout->addLayout(controlLayout);                        // 控制区保持内容所需宽度。

    if (!loadSourceImage(projectImagePath()))                    // 启动时尝试加载默认 01.jpg。
    {
        sourceLabel->setText(QStringLiteral("默认图片加载失败，请选择图片")); // 显示失败提示。
        setProcessingControlsEnabled(false);                     // 无源图时禁止所有算法按钮。
    }
}

/**
 * 加载并准备新的源图片。
 * @param filePath 图片完整路径或可解析路径。
 * @return OpenCV 成功读取非空图像时返回 true。
 */
bool OpenCvDemoPage::loadSourceImage(const QString &filePath)
{
    sourceImage = cv::imread(filePath.toStdString(),             // 把 QString 路径转成 std::string。
                             cv::IMREAD_COLOR);                   // 强制读取为 BGR 三通道彩色图。
    if (sourceImage.empty())                                     // 解码失败或文件不存在会得到空矩阵。
        return false;                                            // 保留旧状态并通知调用者失败。

    cv::cvtColor(sourceImage, grayscaleImage, cv::COLOR_BGR2GRAY); // 生成后续算法共用的灰度图。
    showImage(sourceImage, sourceLabel);                          // 在左上标签显示新源图。
    sourceLabel->setToolTip(                                     // 鼠标悬停时显示实际图片路径。
        QFileInfo(filePath).absoluteFilePath());                 // 将输入路径转换为绝对路径。
    setProcessingControlsEnabled(true);                          // 有有效源图后启用算法按钮。
    return true;                                                 // 通知调用者加载成功。
}

/**
 * 显示 OpenCV 图像。
 * @param image 输入 cv::Mat。
 * @param label 接收 QPixmap 的 QLabel。
 */
void OpenCvDemoPage::showImage(const cv::Mat &image, QLabel *label)
{
    if (image.empty()) // 空矩阵没有有效 data、宽度和高度。
        return; // 避免构造引用无效内存的 QImage。

    cv::Mat display;                                      // 保存适合 Qt 显示的临时矩阵。
    QImage::Format format = QImage::Format_Grayscale8;    // 默认按单通道灰度解释数据。
    if (image.channels() == 3)                            // 三通道图通常是 OpenCV 的 BGR。
    {
        cv::cvtColor(image, display, cv::COLOR_BGR2RGB);  // 转成 QImage 所需 RGB 顺序。
        format = QImage::Format_RGB888;                   // 每像素使用 3×8 位 RGB。
    }
    else                                                  // 单通道结果不需要颜色转换。
    {
        display = image;                                  // cv::Mat 浅拷贝，共享原矩阵数据。
    }

    const QImage qtImage(display.data,                    // 指向 cv::Mat 像素首地址。
                         display.cols,                    // QImage 宽度等于矩阵列数。
                         display.rows,                    // QImage 高度等于矩阵行数。
                         int(display.step),               // 每行实际字节跨度，包含可能的对齐。
                         format);                         // 指定灰度或 RGB888 解释方式。
    label->setPixmap(                                     // 将 Qt 图像转换为标签可显示的像素图。
        QPixmap::fromImage(qtImage.copy()));              // copy() 脱离局部 cv::Mat 的内存生命周期。
}

/**
 * 显示边缘二值/灰度图和叠加红色边缘的彩色图。
 * @param edgeImage 与 sourceImage 尺寸相同的单通道边缘掩码。
 */
void OpenCvDemoPage::showEdgeResult(const cv::Mat &edgeImage)
{
    showImage(edgeImage, primaryResultLabel);             // 主要结果区显示纯边缘图。
    cv::Mat overlay;                                      // 保存源图副本和红色边缘叠加结果。
    sourceImage.copyTo(overlay);                          // 深拷贝源图，避免修改 sourceImage。
    overlay.setTo(cv::Scalar(0, 0, 255), edgeImage);      // edgeImage 非零处在 BGR 图上设为红色。
    showImage(overlay, secondaryResultLabel);             // 辅助结果区显示红色边缘叠加图。
}

/**
 * 统一控制算法按钮状态。
 * @param enabled 是否允许执行图像算法。
 */
void OpenCvDemoPage::setProcessingControlsEnabled(bool enabled)
{
    const QList<QPushButton *> buttons =                  // 查找页面对象树中的全部按钮。
        findChildren<QPushButton *>();                    // 返回包括选择按钮和算法按钮的列表。
    for (QPushButton *button : buttons)                   // 逐个设置相同状态。
        button->setEnabled(enabled);                      // 有图时启用，无图时禁用。
    selectImageButton->setEnabled(true);                  // 选择图片按钮必须始终可用以便恢复。
}

/** 打开文件对话框并加载用户选择的图片。 */
void OpenCvDemoPage::selectImage()
{
    const QString fileName = QFileDialog::getOpenFileName( // 显示模态文件选择框并接收选择结果。
        this,                                               // 对话框父控件为当前页面。
        QStringLiteral("选择图片"),                         // 对话框标题。
        QFileInfo(projectImagePath()).absolutePath(),       // 初始目录为默认图片所在目录。
        QStringLiteral("图片 (*.png *.jpg *.jpeg *.bmp);;所有文件 (*)")); // 文件类型筛选器。
    if (fileName.isEmpty())                                 // 用户取消时返回空字符串。
        return;                                             // 不改变当前源图。
    if (!loadSourceImage(fileName))                         // 尝试用 OpenCV 解码选择的文件。
        QMessageBox::warning(this,                          // 以当前页面作为提示框父控件。
                             QStringLiteral("图片加载失败"), // 提示框标题。
                             QStringLiteral("OpenCV 无法读取所选图片。")); // 失败说明。
}

/** 在主要结果区显示构造时已经准备好的 grayscaleImage。 */
void OpenCvDemoPage::convertToGrayscale()
{
    showImage(grayscaleImage, primaryResultLabel); // 灰度图由 loadSourceImage() 统一生成。
}

/** 计算 256 个灰度级直方图并绘制为黑线白底图。 */
void OpenCvDemoPage::showGrayscaleHistogram()
{
    int channels[] = {0};                           // 指定统计灰度图第 0 个通道。
    int histogramSize[] = {256};                    // 灰度 0～255 共 256 个柱。
    float range[] = {0.0f, 256.0f};                 // 统计区间为左闭右开 [0, 256)。
    const float *ranges[] = {range};                // calcHist() 需要每个通道的范围指针数组。
    cv::Mat histogram;                              // 接收每个灰度级的像素计数。
    cv::calcHist(&grayscaleImage,                   // 输入灰度图数组首地址。
                 1,                                 // 输入图像数量为 1。
                 channels,                          // 统计第 0 通道。
                 cv::Mat(),                         // 空掩码表示统计全部像素。
                 histogram,                         // 输出 256 项直方图。
                 1,                                 // 直方图维度为 1。
                 histogramSize,                     // 每一维的柱数量。
                 ranges);                           // 灰度统计范围。
    cv::normalize(histogram, histogram,             // 将原始像素计数归一化到绘图高度。
                  0, 180, cv::NORM_MINMAX);         // 最小映射到 0，最大映射到 180。

    cv::Mat canvas(190, 256, CV_8UC1, cv::Scalar(255)); // 创建 256×190 的白色灰度画布。
    for (int value = 0; value < 256; ++value)            // 每个灰度级绘制一条竖线。
    {
        const int height =                               // 当前灰度级在画布中的柱高。
            cvRound(histogram.at<float>(value));         // 读取浮点统计值并四舍五入。
        cv::line(canvas,                                  // 在白色画布上绘制黑色直线。
                 cv::Point(value, 189),                   // 起点位于底部对应灰度值位置。
                 cv::Point(value, 189 - height),          // 终点向上移动归一化高度。
                 cv::Scalar(0));                          // 灰度 0 表示黑色。
    }
    showImage(canvas, histogramLabel);                    // 在右下标签显示直方图画布。
}

/** 对灰度图执行直方图均衡化。 */
void OpenCvDemoPage::equalizeHistogram()
{
    cv::Mat result;                                    // 保存均衡化后的单通道结果。
    cv::equalizeHist(grayscaleImage, result);          // 重新分布灰度，使累计分布更均匀。
    showImage(result, primaryResultLabel);             // 在主要结果区显示。
}

/** 使用 Sobel 梯度作为高频细节叠加到原灰度图。 */
void OpenCvDemoPage::applyGradientSharpening()
{
    cv::Mat xGradient;                                 // 保存 X 方向带符号梯度。
    cv::Mat yGradient;                                 // 保存 Y 方向带符号梯度。
    cv::Sobel(grayscaleImage, xGradient, CV_16S, 1, 0); // 一阶 X 导数检测垂直边缘。
    cv::Sobel(grayscaleImage, yGradient, CV_16S, 0, 1); // 一阶 Y 导数检测水平边缘。
    cv::Mat gradient;                                  // 保存两个方向的合成梯度。
    cv::addWeighted(absoluteGradient(xGradient), 0.5,  // X 绝对梯度占 50%。
                    absoluteGradient(yGradient), 0.5,  // Y 绝对梯度占 50%。
                    0.0, gradient);                    // 不加偏移，输出到 gradient。
    cv::Mat result;                                    // 保存锐化后的灰度图。
    cv::addWeighted(grayscaleImage, 1.0,               // 保留完整原图。
                    gradient, 0.8,                     // 叠加 80% 梯度细节。
                    0.0, result);                      // 不加常量偏移。
    showImage(result, primaryResultLabel);             // 主要结果区显示锐化图。
    showImage(gradient, secondaryResultLabel);         // 辅助结果区显示合成梯度。
}

/** 使用 Laplacian 二阶导数增强灰度图细节。 */
void OpenCvDemoPage::applyLaplacianSharpening()
{
    cv::Mat laplacian16;                               // 保存可能包含负值的 16 位二阶响应。
    cv::Laplacian(grayscaleImage, laplacian16, CV_16S, 3); // 使用 3×3 核计算 Laplacian。
    const cv::Mat laplacian = absoluteGradient(laplacian16); // 转成可显示的 8 位绝对响应。
    cv::Mat result;                                    // 保存锐化结果。
    cv::addWeighted(grayscaleImage, 1.0,               // 原灰度图权重为 1。
                    laplacian, 0.8,                    // 二阶细节权重为 0.8。
                    0.0, result);                      // 不增加额外亮度偏移。
    showImage(result, primaryResultLabel);             // 显示 Laplacian 锐化图。
    showImage(laplacian, secondaryResultLabel);        // 显示单独的 Laplacian 响应。
}

/** 使用两个 2×2 Roberts 交叉卷积核检测对角梯度。 */
void OpenCvDemoPage::detectRobertsEdges()
{
    const cv::Mat kernelX =                            // Roberts 第一方向卷积核。
        (cv::Mat_<float>(2, 2) << 1, 0, 0, -1);       // 比较主对角线像素。
    const cv::Mat kernelY =                            // Roberts 第二方向卷积核。
        (cv::Mat_<float>(2, 2) << 0, 1, -1, 0);       // 比较副对角线像素。
    showEdgeResult(                                    // 显示纯边缘和红色叠加结果。
        combinedKernelEdges(grayscaleImage, kernelX, kernelY)); // 合并两个方向响应。
}

/** 使用 OpenCV Sobel 一阶导数检测水平和垂直边缘。 */
void OpenCvDemoPage::detectSobelEdges()
{
    cv::Mat xGradient;                                 // 保存 X 方向 Sobel 响应。
    cv::Mat yGradient;                                 // 保存 Y 方向 Sobel 响应。
    cv::Sobel(grayscaleImage, xGradient, CV_16S, 1, 0); // 计算 X 一阶导数。
    cv::Sobel(grayscaleImage, yGradient, CV_16S, 0, 1); // 计算 Y 一阶导数。
    cv::Mat result;                                    // 保存合成 Sobel 边缘。
    cv::addWeighted(absoluteGradient(xGradient), 0.5,  // X 方向绝对响应占 50%。
                    absoluteGradient(yGradient), 0.5,  // Y 方向绝对响应占 50%。
                    0.0, result);                      // 输出到 result。
    showEdgeResult(result);                            // 显示边缘图和叠加图。
}

/** 使用两个 3×3 Prewitt 卷积核检测边缘。 */
void OpenCvDemoPage::detectPrewittEdges()
{
    const cv::Mat kernelX = (cv::Mat_<float>(3, 3) << // X 方向 Prewitt 核。
        -1, 0, 1, -1, 0, 1, -1, 0, 1);              // 左列为 -1，右列为 1。
    const cv::Mat kernelY = (cv::Mat_<float>(3, 3) << // Y 方向 Prewitt 核。
        -1, -1, -1, 0, 0, 0, 1, 1, 1);              // 上行为 -1，下行为 1。
    showEdgeResult(                                    // 显示合并后的边缘结果。
        combinedKernelEdges(grayscaleImage, kernelX, kernelY)); // 两方向各占 50%。
}

/** 使用 Laplacian 二阶导数直接得到边缘图。 */
void OpenCvDemoPage::detectLaplacianEdges()
{
    cv::Mat result;                                    // 保存带符号 16 位 Laplacian 响应。
    cv::Laplacian(grayscaleImage, result, CV_16S, 3);  // 使用 3×3 核计算二阶导数。
    showEdgeResult(absoluteGradient(result));           // 转成 8 位绝对响应后显示。
}

/** 使用固定双阈值执行 Canny 边缘检测。 */
void OpenCvDemoPage::detectCannyEdges()
{
    cv::Mat result;                                    // 保存 Canny 输出的二值边缘图。
    cv::Canny(grayscaleImage, result, 60.0, 150.0);    // 低阈值 60，高阈值 150。
    showEdgeResult(result);                            // 显示二值边缘和红色叠加图。
}

/** 使用八个方向 Kirsch 核，并保留每个像素的最大方向响应。 */
void OpenCvDemoPage::detectKirschEdges()
{
    const std::array<cv::Mat, 8> kernels = {{ // 固定保存八个罗盘方向的 3×3 核。
        (cv::Mat_<float>(3, 3) << 5, 5, 5, -3, 0, -3, -3, -3, -3), // 北方向。
        (cv::Mat_<float>(3, 3) << 5, 5, -3, 5, 0, -3, -3, -3, -3), // 西北方向。
        (cv::Mat_<float>(3, 3) << 5, -3, -3, 5, 0, -3, 5, -3, -3), // 西方向。
        (cv::Mat_<float>(3, 3) << -3, -3, -3, 5, 0, -3, 5, 5, -3), // 西南方向。
        (cv::Mat_<float>(3, 3) << -3, -3, -3, -3, 0, -3, 5, 5, 5), // 南方向。
        (cv::Mat_<float>(3, 3) << -3, -3, -3, -3, 0, 5, -3, 5, 5), // 东南方向。
        (cv::Mat_<float>(3, 3) << -3, -3, 5, -3, 0, 5, -3, -3, 5), // 东方向。
        (cv::Mat_<float>(3, 3) << -3, 5, 5, -3, 0, 5, -3, -3, -3)}}; // 东北方向。

    cv::Mat maximum(grayscaleImage.size(),             // 输出尺寸与灰度源图相同。
                    CV_8UC1,                           // 每像素 8 位单通道。
                    cv::Scalar(0));                     // 初始所有最大响应为 0。
    for (const cv::Mat &kernel : kernels)              // 依次计算八个方向响应。
    {
        cv::Mat response;                              // 保存当前方向的带符号卷积结果。
        cv::filter2D(grayscaleImage, response, CV_16S, kernel); // 使用当前 Kirsch 核卷积。
        cv::max(maximum,                               // 已累计的逐像素最大值。
                absoluteGradient(response),            // 当前方向的 8 位绝对响应。
                maximum);                              // 把新的逐像素最大值写回 maximum。
    }
    showEdgeResult(maximum);                           // 显示八方向最强边缘结果。
}
