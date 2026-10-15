#include "opencv.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <array>

namespace
{
QString projectImagePath()
{
    const QString relativePath = QStringLiteral("opencv_src/01.jpg");
    const QStringList roots = {
        QCoreApplication::applicationDirPath(),
        QDir::currentPath(),
        QDir::cleanPath(QCoreApplication::applicationDirPath() +
                        QStringLiteral("/../project")),
        QDir::cleanPath(QDir::currentPath() + QStringLiteral("/../project"))};

    for (const QString &root : roots)
    {
        const QString candidate = QDir(root).filePath(relativePath);
        if (QFileInfo::exists(candidate))
            return QDir::cleanPath(candidate);
    }
    return QDir(roots.first()).filePath(relativePath);
}

cv::Mat absoluteGradient(const cv::Mat &gradient)
{
    cv::Mat result;
    cv::convertScaleAbs(gradient, result);
    return result;
}

cv::Mat combinedKernelEdges(const cv::Mat &gray,
                            const cv::Mat &horizontalKernel,
                            const cv::Mat &verticalKernel)
{
    cv::Mat horizontal;
    cv::Mat vertical;
    cv::filter2D(gray, horizontal, CV_16S, horizontalKernel);
    cv::filter2D(gray, vertical, CV_16S, verticalKernel);
    cv::Mat result;
    cv::addWeighted(absoluteGradient(horizontal), 0.5,
                    absoluteGradient(vertical), 0.5, 0.0, result);
    return result;
}
}

OpenCvDemoPage::OpenCvDemoPage(QWidget *parent)
    : QWidget(parent),
      sourceLabel(new QLabel(QStringLiteral("原图"), this)),
      primaryResultLabel(new QLabel(QStringLiteral("处理结果"), this)),
      secondaryResultLabel(new QLabel(QStringLiteral("辅助结果"), this)),
      histogramLabel(new QLabel(QStringLiteral("直方图"), this)),
      selectImageButton(new QPushButton(QStringLiteral("选择图片"), this))
{
    setObjectName(QStringLiteral("opencvPage"));
    setStyleSheet(QStringLiteral(
        "#opencvPage { background:#1b2028; color:#eef2f7; }"
        "QLabel { background:#10151c; color:#8fa0b3; border:1px solid #354150; border-radius:8px; }"
        "QGroupBox { color:#6dc7ff; border:1px solid #354150; border-radius:7px; margin-top:10px; }"
        "QGroupBox::title { subcontrol-origin:margin; left:7px; padding:0 3px; }"
        "QPushButton { min-height:30px; background:#303a47; color:#eef2f7; border:none; border-radius:4px; padding:2px 6px; }"
        "QPushButton:hover { background:#1677b8; }"
        "QPushButton:disabled { background:#252c35; color:#6f7c8b; }"));

    const QList<QLabel *> imageLabels = {
        sourceLabel, primaryResultLabel, secondaryResultLabel, histogramLabel};
    for (QLabel *label : imageLabels)
    {
        label->setAlignment(Qt::AlignCenter);
        label->setMinimumSize(210, 190);
        label->setScaledContents(true);
    }

    QGridLayout *imageLayout = new QGridLayout;
    imageLayout->addWidget(sourceLabel, 0, 0);
    imageLayout->addWidget(primaryResultLabel, 0, 1);
    imageLayout->addWidget(secondaryResultLabel, 1, 0);
    imageLayout->addWidget(histogramLabel, 1, 1);

    QGroupBox *preprocessGroup = new QGroupBox(QStringLiteral("图像预处理"), this);
    QVBoxLayout *preprocessLayout = new QVBoxLayout(preprocessGroup);
    const QList<QPair<QString, void (OpenCvDemoPage::*)()>> preprocessActions = {
        {QStringLiteral("灰度处理"), &OpenCvDemoPage::convertToGrayscale},
        {QStringLiteral("灰度直方图"), &OpenCvDemoPage::showGrayscaleHistogram},
        {QStringLiteral("直方图均衡"), &OpenCvDemoPage::equalizeHistogram},
        {QStringLiteral("梯度锐化"), &OpenCvDemoPage::applyGradientSharpening},
        {QStringLiteral("Laplacian锐化"), &OpenCvDemoPage::applyLaplacianSharpening}};
    for (const auto &action : preprocessActions)
    {
        QPushButton *button = new QPushButton(action.first, preprocessGroup);
        connect(button, &QPushButton::clicked, this, action.second);
        preprocessLayout->addWidget(button);
    }

    QGroupBox *edgeGroup = new QGroupBox(QStringLiteral("边缘检测"), this);
    QVBoxLayout *edgeLayout = new QVBoxLayout(edgeGroup);
    const QList<QPair<QString, void (OpenCvDemoPage::*)()>> edgeActions = {
        {QStringLiteral("Roberts"), &OpenCvDemoPage::detectRobertsEdges},
        {QStringLiteral("Sobel"), &OpenCvDemoPage::detectSobelEdges},
        {QStringLiteral("Prewitt"), &OpenCvDemoPage::detectPrewittEdges},
        {QStringLiteral("Laplacian"), &OpenCvDemoPage::detectLaplacianEdges},
        {QStringLiteral("Canny"), &OpenCvDemoPage::detectCannyEdges},
        {QStringLiteral("Kirsch"), &OpenCvDemoPage::detectKirschEdges}};
    for (const auto &action : edgeActions)
    {
        QPushButton *button = new QPushButton(action.first, edgeGroup);
        connect(button, &QPushButton::clicked, this, action.second);
        edgeLayout->addWidget(button);
    }

    connect(selectImageButton, &QPushButton::clicked,
            this, &OpenCvDemoPage::selectImage);

    QVBoxLayout *controlLayout = new QVBoxLayout;
    controlLayout->addWidget(selectImageButton);
    controlLayout->addWidget(preprocessGroup);
    controlLayout->addWidget(edgeGroup);
    controlLayout->addStretch();

    QHBoxLayout *pageLayout = new QHBoxLayout(this);
    pageLayout->addLayout(imageLayout, 1);
    pageLayout->addLayout(controlLayout);

    if (!loadSourceImage(projectImagePath()))
    {
        sourceLabel->setText(QStringLiteral("默认图片加载失败，请选择图片"));
        setProcessingControlsEnabled(false);
    }
}

bool OpenCvDemoPage::loadSourceImage(const QString &filePath)
{
    sourceImage = cv::imread(filePath.toStdString(), cv::IMREAD_COLOR);
    if (sourceImage.empty())
        return false;

    cv::cvtColor(sourceImage, grayscaleImage, cv::COLOR_BGR2GRAY);
    showImage(sourceImage, sourceLabel);
    sourceLabel->setToolTip(QFileInfo(filePath).absoluteFilePath());
    setProcessingControlsEnabled(true);
    return true;
}

void OpenCvDemoPage::showImage(const cv::Mat &image, QLabel *label)
{
    if (image.empty())
        return;

    cv::Mat display;
    QImage::Format format = QImage::Format_Grayscale8;
    if (image.channels() == 3)
    {
        cv::cvtColor(image, display, cv::COLOR_BGR2RGB);
        format = QImage::Format_RGB888;
    }
    else
    {
        display = image;
    }

    const QImage qtImage(display.data, display.cols, display.rows,
                         int(display.step), format);
    label->setPixmap(QPixmap::fromImage(qtImage.copy()));
}

void OpenCvDemoPage::showEdgeResult(const cv::Mat &edgeImage)
{
    showImage(edgeImage, primaryResultLabel);
    cv::Mat overlay;
    sourceImage.copyTo(overlay);
    overlay.setTo(cv::Scalar(0, 0, 255), edgeImage);
    showImage(overlay, secondaryResultLabel);
}

void OpenCvDemoPage::setProcessingControlsEnabled(bool enabled)
{
    const QList<QPushButton *> buttons = findChildren<QPushButton *>();
    for (QPushButton *button : buttons)
        button->setEnabled(enabled);
    selectImageButton->setEnabled(true);
}

void OpenCvDemoPage::selectImage()
{
    const QString fileName = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择图片"), QFileInfo(projectImagePath()).absolutePath(),
        QStringLiteral("图片 (*.png *.jpg *.jpeg *.bmp);;所有文件 (*)"));
    if (fileName.isEmpty())
        return;
    if (!loadSourceImage(fileName))
        QMessageBox::warning(this, QStringLiteral("图片加载失败"),
                             QStringLiteral("OpenCV 无法读取所选图片。"));
}

void OpenCvDemoPage::convertToGrayscale()
{
    showImage(grayscaleImage, primaryResultLabel);
}

void OpenCvDemoPage::showGrayscaleHistogram()
{
    int channels[] = {0};
    int histogramSize[] = {256};
    float range[] = {0.0f, 256.0f};
    const float *ranges[] = {range};
    cv::Mat histogram;
    cv::calcHist(&grayscaleImage, 1, channels, cv::Mat(), histogram,
                 1, histogramSize, ranges);
    cv::normalize(histogram, histogram, 0, 180, cv::NORM_MINMAX);

    cv::Mat canvas(190, 256, CV_8UC1, cv::Scalar(255));
    for (int value = 0; value < 256; ++value)
    {
        const int height = cvRound(histogram.at<float>(value));
        cv::line(canvas, cv::Point(value, 189),
                 cv::Point(value, 189 - height), cv::Scalar(0));
    }
    showImage(canvas, histogramLabel);
}

void OpenCvDemoPage::equalizeHistogram()
{
    cv::Mat result;
    cv::equalizeHist(grayscaleImage, result);
    showImage(result, primaryResultLabel);
}

void OpenCvDemoPage::applyGradientSharpening()
{
    cv::Mat xGradient;
    cv::Mat yGradient;
    cv::Sobel(grayscaleImage, xGradient, CV_16S, 1, 0);
    cv::Sobel(grayscaleImage, yGradient, CV_16S, 0, 1);
    cv::Mat gradient;
    cv::addWeighted(absoluteGradient(xGradient), 0.5,
                    absoluteGradient(yGradient), 0.5, 0.0, gradient);
    cv::Mat result;
    cv::addWeighted(grayscaleImage, 1.0, gradient, 0.8, 0.0, result);
    showImage(result, primaryResultLabel);
    showImage(gradient, secondaryResultLabel);
}

void OpenCvDemoPage::applyLaplacianSharpening()
{
    cv::Mat laplacian16;
    cv::Laplacian(grayscaleImage, laplacian16, CV_16S, 3);
    const cv::Mat laplacian = absoluteGradient(laplacian16);
    cv::Mat result;
    cv::addWeighted(grayscaleImage, 1.0, laplacian, 0.8, 0.0, result);
    showImage(result, primaryResultLabel);
    showImage(laplacian, secondaryResultLabel);
}

void OpenCvDemoPage::detectRobertsEdges()
{
    const cv::Mat kernelX = (cv::Mat_<float>(2, 2) << 1, 0, 0, -1);
    const cv::Mat kernelY = (cv::Mat_<float>(2, 2) << 0, 1, -1, 0);
    showEdgeResult(combinedKernelEdges(grayscaleImage, kernelX, kernelY));
}

void OpenCvDemoPage::detectSobelEdges()
{
    cv::Mat xGradient;
    cv::Mat yGradient;
    cv::Sobel(grayscaleImage, xGradient, CV_16S, 1, 0);
    cv::Sobel(grayscaleImage, yGradient, CV_16S, 0, 1);
    cv::Mat result;
    cv::addWeighted(absoluteGradient(xGradient), 0.5,
                    absoluteGradient(yGradient), 0.5, 0.0, result);
    showEdgeResult(result);
}

void OpenCvDemoPage::detectPrewittEdges()
{
    const cv::Mat kernelX = (cv::Mat_<float>(3, 3) <<
        -1, 0, 1, -1, 0, 1, -1, 0, 1);
    const cv::Mat kernelY = (cv::Mat_<float>(3, 3) <<
        -1, -1, -1, 0, 0, 0, 1, 1, 1);
    showEdgeResult(combinedKernelEdges(grayscaleImage, kernelX, kernelY));
}

void OpenCvDemoPage::detectLaplacianEdges()
{
    cv::Mat result;
    cv::Laplacian(grayscaleImage, result, CV_16S, 3);
    showEdgeResult(absoluteGradient(result));
}

void OpenCvDemoPage::detectCannyEdges()
{
    cv::Mat result;
    cv::Canny(grayscaleImage, result, 60.0, 150.0);
    showEdgeResult(result);
}

void OpenCvDemoPage::detectKirschEdges()
{
    const std::array<cv::Mat, 8> kernels = {{
        (cv::Mat_<float>(3, 3) << 5, 5, 5, -3, 0, -3, -3, -3, -3),
        (cv::Mat_<float>(3, 3) << 5, 5, -3, 5, 0, -3, -3, -3, -3),
        (cv::Mat_<float>(3, 3) << 5, -3, -3, 5, 0, -3, 5, -3, -3),
        (cv::Mat_<float>(3, 3) << -3, -3, -3, 5, 0, -3, 5, 5, -3),
        (cv::Mat_<float>(3, 3) << -3, -3, -3, -3, 0, -3, 5, 5, 5),
        (cv::Mat_<float>(3, 3) << -3, -3, -3, -3, 0, 5, -3, 5, 5),
        (cv::Mat_<float>(3, 3) << -3, -3, 5, -3, 0, 5, -3, -3, 5),
        (cv::Mat_<float>(3, 3) << -3, 5, 5, -3, 0, 5, -3, -3, -3)}};

    cv::Mat maximum(grayscaleImage.size(), CV_8UC1, cv::Scalar(0));
    for (const cv::Mat &kernel : kernels)
    {
        cv::Mat response;
        cv::filter2D(grayscaleImage, response, CV_16S, kernel);
        cv::max(maximum, absoluteGradient(response), maximum);
    }
    showEdgeResult(maximum);
}
