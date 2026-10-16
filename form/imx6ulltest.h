/*
 * imx6ulltest.h — “板级设备”页面声明
 *
 * BoardDiagnosticsPage 负责展示和控制开发板硬件：
 *   AP3216C 环境光/红外/接近传感器、ICM20608 六轴传感器、LED 和蜂鸣器。
 */
#ifndef IMX6ULLTEST_H // 防止同一编译单元重复包含本头文件。
#define IMX6ULLTEST_H // 定义头文件保护宏。

#include <QChartView>    // 提供承载 QChart 的图表视图控件。
#include <QValueAxis>    // 提供数值型 X/Y 坐标轴。
#include <QSplineSeries> // 提供平滑曲线序列，用于显示 AP3216C 历史数据。
#include <QWidget>       // BoardDiagnosticsPage 的 QWidget 基类。
#include "ap3216c.h"     // 引入 AP3216C 传感器读取类。
#include "icm20608.h"    // 引入 ICM20608 六轴传感器读取类。

QT_CHARTS_USE_NAMESPACE // 把 QtCharts 命名空间中的图表类型引入当前作用域。

namespace Ui // Qt uic 生成的界面类所在命名空间。
{
class imx6ullTest; // 前向声明由 imx6ulltest.ui 生成的 Ui::imx6ullTest。
} // 结束 Ui 命名空间。

class BoardDiagnosticsPage final : public QWidget // final 表示该页面不再作为其他类的基类。
{
    Q_OBJECT // 启用 Qt 元对象系统，使自动槽和信号槽连接可用。

public:
    /**
     * 创建板级设备页面并初始化图表和硬件接口。
     * @param parent Qt 父控件，负责页面在对象树中的生命周期。
     */
    explicit BoardDiagnosticsPage(QWidget *parent = nullptr);

    /** 释放 ui 对象；其子控件会由 Qt 父子对象机制自动释放。 */
    ~BoardDiagnosticsPage() override;

    /** 创建 AP3216C 曲线、坐标轴和图表，并安装到 UI 的 QChartView。 */
    void initializeSensorChart();

    /** ARM 平台上创建传感器对象，并准备 LED、蜂鸣器 sysfs 文件。 */
    void initializeBoardHardware();

private:
    Ui::imx6ullTest *ui; // uic 生成的控件访问入口，拥有按钮、文本框和图表视图指针。

    QChart *sensorChart; // AP3216C 三条曲线共用的图表对象。

    Ap3216cSensor *ambientLightSensor; // 读取环境光、红外和接近值的 AP3216C 对象。

    Icm20608Sensor *motionSensor; // 读取陀螺仪、加速度和温度的 ICM20608 对象。

    QValueAxis *sampleAxis;      // 横轴，表示递增的采样序号。
    QValueAxis *sensorValueAxis; // 纵轴，表示三个 AP3216C 传感器数值。

    QSplineSeries *ambientLightSeries; // 环境光强度历史曲线。
    QSplineSeries *infraredSeries;     // 红外强度历史曲线。
    QSplineSeries *proximitySeries;    // 接近传感器历史曲线。

    enum // 页面内部使用的图表固定范围常量。
    {
        MaximumVisibleSamples = 40, // 横轴同一时间最多显示 40 个采样点。
        MaximumSensorValue = 40     // 纵轴默认最大显示值为 40。
    };

    int sampleIndex = 0; // 下一次 AP3216C 数据在横轴上的采样序号。

private slots:
    /** 清空三条 AP3216C 曲线，并把采样序号复位为 0。 */
    void clearSensorChart();

    /** ARM 平台上启动 AP3216C 的 500ms 周期读取。 */
    void startAp3216cPolling();

    /** ARM 平台上停止 AP3216C 读取，并复位采样序号。 */
    void stopAp3216cPolling();

    /** 把最新 AP3216C 数值写入文本框并追加到三条曲线。 */
    void updateAp3216cDisplay();

private slots: // 保留原有槽分区；下面是 ICM20608 定时采样对应的槽函数。
    /** 把最新 ICM20608 陀螺仪、加速度和温度写入文本框。 */
    void updateIcm20608Display();

private:
    QFile ledBrightnessFile; // 对应开发板 LED brightness sysfs 节点的文件对象。

    /** 切换 LED 亮灭状态，并刷新按钮文字。 */
    void toggleLed();

    /** @return 从 sysfs 读取到的 LED 当前状态；读取失败时返回 false。 */
    bool readLedState();

private slots:
    /** Qt 自动连接槽：用户点击 LED 按钮后切换 LED 状态。 */
    void on_ledBt_clicked();

private:
    QFile buzzerBrightnessFile; // 对应开发板蜂鸣器 brightness sysfs 节点的文件对象。

    /** 切换蜂鸣器开关状态，并刷新按钮文字。 */
    void toggleBuzzer();

    /** @return 从 sysfs 读取到的蜂鸣器当前状态；读取失败时返回 false。 */
    bool readBuzzerState();

private slots:
    /** Qt 自动连接槽：用户点击 BEEP 按钮后切换蜂鸣器状态。 */
    void on_beepBt_clicked();

    /** Qt 自动连接槽：用户点击开启按钮后启动 ICM20608 周期读取。 */
    void on_start2060Bt_clicked();

    /** Qt 自动连接槽：用户点击关闭按钮后停止 ICM20608 周期读取。 */
    void on_stop2060Bt_clicked();
};

#endif // IMX6ULLTEST_H：结束头文件保护。
