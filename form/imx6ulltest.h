#ifndef IMX6ULLTEST_H
#define IMX6ULLTEST_H

#include <QChartView>
#include <QValueAxis>
#include <QSplineSeries>
#include <QWidget>
#include "ap3216c.h"
#include "icm20608.h"

/* 光添加头文件QChartView还不够，还需要引入QChart的命名空间 */
QT_CHARTS_USE_NAMESPACE

namespace Ui {
class imx6ullTest;
}

class BoardDiagnosticsPage final : public QWidget
{
    Q_OBJECT

public:
    explicit BoardDiagnosticsPage(QWidget *parent = nullptr);
    ~BoardDiagnosticsPage() override;

    void initializeSensorChart();
    void initializeBoardHardware();

private:
    Ui::imx6ullTest *ui;

    /* 图表对象 */
    QChart *sensorChart;

    /* 环境光、接近和红外传感器 */
    Ap3216cSensor *ambientLightSensor;

    /* 六轴运动传感器 */
    Icm20608Sensor *motionSensor;

    /* 横纵坐标轴对象 */
    QValueAxis *sampleAxis;
    QValueAxis *sensorValueAxis;

    /* 曲线图对象 */
    QSplineSeries *ambientLightSeries;
    QSplineSeries *infraredSeries;
    QSplineSeries *proximitySeries;

    /* 横纵坐标最大显示范围 */
    enum { MaximumVisibleSamples = 40, MaximumSensorValue = 40 };

    /* 用来记录数据点数 */
    int sampleIndex = 0;


private slots:
    void clearSensorChart();
    void startAp3216cPolling();
    void stopAp3216cPolling();

    /* 获取ap3216传感器数据 */
    void updateAp3216cDisplay();


private slots:
    /* 获取icm20608传感器数据 */
    void updateIcm20608Display();

/* led相关  */
private:
    /* 文件 */
    QFile ledBrightnessFile;

    /* 切换并读取 LED 状态 */
    void toggleLed();

    bool readLedState();

private slots:
    void on_ledBt_clicked();


/* beep相关 */
private:
    /* 文件 */
    QFile buzzerBrightnessFile;

    /* 切换并读取蜂鸣器状态 */
    void toggleBuzzer();

    bool readBuzzerState();

private slots:
    /* 槽函数 */
    void on_beepBt_clicked();
    void on_start2060Bt_clicked();
    void on_stop2060Bt_clicked();
};

#endif // IMX6ULLTEST_H
