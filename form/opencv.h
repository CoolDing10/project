/*
 * opencv.h — 图像预处理与边缘检测页面
 */
#ifndef OPENCV_H
#define OPENCV_H

#include <QWidget>

#include <opencv2/core.hpp>

class QLabel;
class QPushButton;

class OpenCvDemoPage final : public QWidget
{
    Q_OBJECT

public:
    explicit OpenCvDemoPage(QWidget *parent = nullptr);
    ~OpenCvDemoPage() override = default;

private slots:
    void selectImage();
    void convertToGrayscale();
    void showGrayscaleHistogram();
    void equalizeHistogram();
    void applyGradientSharpening();
    void applyLaplacianSharpening();
    void detectRobertsEdges();
    void detectSobelEdges();
    void detectPrewittEdges();
    void detectLaplacianEdges();
    void detectCannyEdges();
    void detectKirschEdges();

private:
    bool loadSourceImage(const QString &filePath);
    void showImage(const cv::Mat &image, QLabel *label);
    void showEdgeResult(const cv::Mat &edgeImage);
    void setProcessingControlsEnabled(bool enabled);

    cv::Mat sourceImage;
    cv::Mat grayscaleImage;

    QLabel *sourceLabel;
    QLabel *primaryResultLabel;
    QLabel *secondaryResultLabel;
    QLabel *histogramLabel;
    QPushButton *selectImageButton;
};

#endif // OPENCV_H
