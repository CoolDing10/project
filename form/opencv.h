/*
 * ============================================================================
 * opencv.h — OpenCV 计算机视觉演示页面
 * ============================================================================
 *
 * OpenCvDemoPage 是"OpenCV 视觉"导航页的实现。当前学习版只开放
 * 第 1、2 类；其余算法源码暂时保留但不提供 UI 入口：
 *
 *   1. 图像预处理：灰度转换、直方图、均衡化、梯度锐化、拉普拉斯锐化
 *   2. 边缘检测：Roberts、Sobel、Prewitt、Laplacian、Canny、Kirsch
 *   3. 噪声与滤波：椒盐/高斯噪声、均值/中值/高斯/自定义滤波
 *   4. 图像变换：仿射变换、透视变换
 *   5. 背景处理：阈值分割、Otsu、Kittler、帧差法、高斯混合模型
 *   6. 特征提取：LBP、SIFT、ORB、目标检测、颜色拟合、Gabor 滤波
 *   7. 机器学习：SVM 分类、文字识别
 *   8. 物体检测：Haar 级联检测、Stereo 相机标定
 *
 * 自动抓拍刷新机制：
 *   snapRefreshTimer 定时扫描 SNAP_IMAGE_PATH 目录，
 *   当检测到新的抓拍图片时自动加载并处理。
 *   lastObservedSnapshotPath 用于防止重复处理。
 *
 * 当前导航页只需要重点学习第 1、2 类。
 */
#ifndef OPENCV_H
#define OPENCV_H

#include <QWidget>
#include <opencv2/opencv.hpp>
#include <opencv2/imgproc/types_c.h>
#include <opencv2/imgproc/imgproc.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/core.hpp>
#include <opencv2/objdetect.hpp>
#include <opencv2/ml.hpp>
#include <QFileDialog>
#include <QVector>
#include <iostream>
#include <algorithm>
#include <limits>
#include <qmath.h>
#include <vector>
#include <opencv2/calib3d/calib3d.hpp>
#include "opencv2/calib3d.hpp"
#include <opencv2/features2d/features2d.hpp>
#include <stdio.h>
#include <fstream>
#include <opencv2/highgui/highgui_c.h>
#include <opencv2/highgui.hpp>
using namespace cv;
using namespace std;

namespace Ui
{
    class opencv;
}

class QTimer;

/*
 * OpenCvDemoPage — OpenCV 视觉算法演示页面
 *
 * 当前提供图像预处理和边缘检测的交互式演示。
 * 用户可以选择本地图片或使用自动抓拍的图片进行处理。
 */
class OpenCvDemoPage final : public QWidget
{
    Q_OBJECT

public:
    explicit OpenCvDemoPage(QWidget *parent = nullptr);
    ~OpenCvDemoPage() override;

private:
    Ui::opencv *ui;
    Mat sourceImage;          // 源图像（从文件加载的原图）
    Mat grayscaleImage;       // 灰度图像（灰度转换结果，供后续处理使用）
    Mat noisyImage;           // 加噪图像（椒盐/高斯噪声添加后的结果）
    QString loadedSourcePath; // 当前已加载的源图像文件路径

    /*
     * 最近一次已经自动处理过的抓拍路径。
     * 它不能和 loadedSourcePath 共用：用户手动选图后 loadedSourcePath
     * 会改变，但 QSettings 中仍是上一次抓拍，若直接比较两者，定时器
     * 会在下一秒把手动图片错误地覆盖回旧抓拍。
     */
    QString lastObservedSnapshotPath;
    QTimer *snapshotRefreshTimer; // 自动抓拍刷新定时器

    /* 加载源图像文件 */
    bool loadSourceImage(const QString &filePath);
    /* 设置图像处理控件的启用/禁用状态 */
    void setProcessingControlsEnabled(bool enabled);
    /* 刷新最新抓拍图片 */
    void refreshLatestSnapshot();

private slots:
    // ===== 第 1 类：图像预处理 =====
    void selectImage();              // 选择本地图片文件
    void convertToGrayscale();       // RGB → 灰度（加权平均法）
    void showGrayscaleHistogram();   // 显示灰度直方图
    void equalizeHistogram();        // 直方图均衡化（增强对比度）
    void applyGradientSharpening();  // 梯度锐化（增强边缘）
    void applyLaplacianSharpening(); // 拉普拉斯锐化

    // ===== 第 2 类：边缘检测 =====
    void detectRobertsEdges();   // Roberts 交叉算子
    void detectSobelEdges();     // Sobel 算子（一阶梯度）
    void detectPrewittEdges();   // Prewitt 算子
    void detectLaplacianEdges(); // Laplacian 算子（二阶梯度）
    void detectCannyEdges();     // Canny 边缘检测（最优边缘检测器）
    void detectKirschEdges();    // Kirsch 方向模板算子

    // ===== 第 3 类：噪声 =====
    void addSaltNoise();     // 添加椒盐噪声（随机黑白点）
    void addGaussianNoise(); // 添加高斯噪声（服从正态分布）

    // ===== 第 4 类：滤波 =====
    void applyAverageFilter();  // 均值滤波（简单平均去噪）
    void applyMedianFilter();   // 中值滤波（对椒盐噪声效果好）
    void applyWindowFilter();   // 窗口滤波
    void applyGaussianFilter(); // 高斯滤波（权重符合高斯分布）
    void applyCustomFilter();   // 自定义卷积核滤波

    // ===== 第 5 类：图像变换 =====
    void applyAffineTransform();      // 仿射变换（旋转、缩放、平移）
    void applyPerspectiveTransform(); // 透视变换（投影映射）

    // ===== 第 6 类：背景处理 / 分割 =====
    void applyThresholdSegmentation(); // 固定阈值分割
    void applyOtsuThreshold();         // Otsu 自动阈值（最大类间方差法）
    void applyKittlerThreshold();      // Kittler 最小误差阈值法
    void runFrameDifference();         // 帧差法（运动检测基础算法）
    void runGaussianMixture();         // 高斯混合模型（背景建模）

    // ===== 第 7 类：特征提取 =====
    void extractCircularLbp(); // 圆形 LBP（局部二值模式）特征
    void runTargetDetection(); // 目标检测
    void runModelCheck();      // 模型校验
    void runCloakingDemo();    // 伪装（图像隐藏）演示
    void runColorFitting();    // 颜色拟合
    void applyGaborFilter();   // Gabor 滤波器（纹理分析）

    // ===== 第 8 类：高级特征提取和机器学习 =====
    void extractSiftFeatures();    // SIFT 特征点提取（尺度不变特征变换）
    void extractOrbFeatures();     // ORB 特征点提取（快速二进制描述子）
    void runSvmTest();             // SVM 支持向量机测试
    void runWordRecognitionTest(); // 文字识别测试
    void runHaarDetection();       // Haar 级联检测（人脸/物体）
    void runSecondHaarDetection(); // 第二种 Haar 检测器

    // ===== 第 9 类：相机标定 =====
    void runStereoCameraCalibration(); // 双目相机标定
    void runCameraDemo();              // 相机演示
};

#endif // OPENCV_H
