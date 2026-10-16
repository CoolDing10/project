/*
 * imx6ulltest.cpp — 板级设备页面实现
 *
 * 页面包含四类功能：
 *   AP3216C 数值与曲线、ICM20608 数值、LED 控制、蜂鸣器控制。
 * 真实硬件初始化放在 __arm__ 条件编译中，因此桌面调试不会访问设备节点。
 */
#include "imx6ulltest.h"    // 引入 BoardDiagnosticsPage 类和成员声明。
#include "ui_imx6ulltest.h" // 引入 uic 根据 .ui 文件生成的控件访问类。

#include "ap3216c.h"  // 引入 AP3216C 传感器接口；头文件中也使用该类型。
#include <QDebug>     // 使用 qWarning() 输出文件访问错误。
#include <QVBoxLayout> // 创建让 .ui 内容跟随页面缩放的垂直布局。
#include <QtCharts>   // 使用 QChart、坐标轴、曲线、图例和抗锯齿绘制。

/**
 * 创建板级设备页面。
 * @param parent Qt 父控件；主窗口页面栈会管理本页面生命周期。
 */
BoardDiagnosticsPage::BoardDiagnosticsPage(QWidget *parent) :
    QWidget(parent),                         // 初始化 QWidget 基类并建立父子关系。
    ui(new Ui::imx6ullTest),                 // 创建 .ui 控件访问对象。
    sensorChart(nullptr),                    // 图表稍后由 initializeSensorChart() 创建。
    ambientLightSensor(nullptr),             // AP3216C 仅在 ARM 开发板上创建。
    motionSensor(nullptr),                   // ICM20608 仅在 ARM 开发板上创建。
    sampleAxis(nullptr),                     // 横轴稍后创建。
    sensorValueAxis(nullptr),                // 纵轴稍后创建。
    ambientLightSeries(nullptr),             // 环境光曲线稍后创建。
    infraredSeries(nullptr),                 // 红外曲线稍后创建。
    proximitySeries(nullptr)                 // 接近曲线稍后创建。
{
    ui->setupUi(this); // 按 imx6ulltest.ui 创建所有子控件并执行自动槽连接。

    /*
     * 原 .ui 将页面最小高度固定为 600px，嵌入带有顶部标题区的
     * 1024x600 主界面后会把底部裁掉，因此解除根页面和内部控件限制。
     */
    setMinimumSize(0, 0);                                  // 允许主布局把页面缩小到可用区域。
    setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);      // 取消根页面最大尺寸限制。
    ui->centralwidget->setMinimumSize(0, 0);               // 取消 .ui 内容容器的最小尺寸。
    ui->tabWidget->setMinimumSize(0, 0);                   // 允许页签控件随主界面缩小。
    ui->tabWidget->setMaximumSize(QWIDGETSIZE_MAX,          // 最大宽度使用 Qt 控件上限。
                                  QWIDGETSIZE_MAX);         // 最大高度也使用 Qt 控件上限。

    QVBoxLayout *contentLayout =                            // contentLayout 管理 centralwidget 内部布局。
        new QVBoxLayout(ui->centralwidget);                 // 以 centralwidget 为父对象，自动释放布局。
    contentLayout->setContentsMargins(0, 0, 0, 0);         // 去除内容容器四周空白。
    contentLayout->addWidget(ui->tabWidget);               // 让页签控件填满 centralwidget。

    QVBoxLayout *pageLayout = new QVBoxLayout(this);       // pageLayout 是页面根布局。
    pageLayout->setContentsMargins(0, 0, 0, 0);            // 去除页面根布局四周空白。
    pageLayout->addWidget(ui->centralwidget);              // 让 .ui 内容容器填满整个页面。

    initializeSensorChart(); // 创建 AP3216C 曲线和坐标轴。
    initializeBoardHardware(); // ARM 上创建传感器并准备 LED/蜂鸣器接口。

    connect(ui->pushButton, &QPushButton::clicked,          // “停止检测”按钮及其 clicked 信号。
            this, &BoardDiagnosticsPage::stopAp3216cPolling); // 点击后停止 AP3216C 轮询。
    connect(ui->btnClear, &QPushButton::clicked,            // “清除曲线”按钮及其 clicked 信号。
            this, &BoardDiagnosticsPage::clearSensorChart); // 点击后清空三条历史曲线。
    connect(ui->btnStartAndStop, &QPushButton::clicked,     // “启动检测”按钮及其 clicked 信号。
            this, &BoardDiagnosticsPage::startAp3216cPolling); // 点击后启动 AP3216C 轮询。
}

/** 释放 uic 创建的界面包装对象。 */
BoardDiagnosticsPage::~BoardDiagnosticsPage()
{
    delete ui; // ui 析构时，实际控件仍按 Qt 父子对象关系由 QWidget 负责释放。
}

/** 创建 AP3216C 的坐标轴、三条曲线和图表显示对象。 */
void BoardDiagnosticsPage::initializeSensorChart()
{
    sampleAxis = new QValueAxis();                           // 创建表示采样序号的横轴。
    sensorValueAxis = new QValueAxis();                      // 创建表示传感器数值的纵轴。
    sampleAxis->setLabelsColor(Qt::white);                   // 横轴刻度文字设为白色。
    sensorValueAxis->setLabelsColor(Qt::white);              // 纵轴刻度文字设为白色。
    sampleAxis->setLinePenColor(Qt::white);                  // 横轴线设为白色。
    sensorValueAxis->setLinePenColor(Qt::white);             // 纵轴线设为白色。
    sampleAxis->setTitleBrush(QBrush(Qt::white));            // 横轴标题画刷设为白色。
    sensorValueAxis->setTitleBrush(QBrush(Qt::white));       // 纵轴标题画刷设为白色。

    sampleAxis->setTitleText(QStringLiteral("采样序号"));    // 设置横轴含义。
    sensorValueAxis->setTitleText(QStringLiteral("传感器数值")); // 设置纵轴含义。
    sampleAxis->setRange(0, MaximumVisibleSamples);          // 初始显示第 0～40 个采样点。
    sensorValueAxis->setRange(0, MaximumSensorValue);        // 初始传感器数值范围为 0～40。

    ambientLightSeries = new QSplineSeries();                // 创建环境光平滑曲线。
    ambientLightSeries->setPointsVisible(false);             // 不单独绘制每个数据点标记。
    ambientLightSeries->setName(QStringLiteral("环境光"));   // 设置图例显示名称。

    infraredSeries = new QSplineSeries();                    // 创建红外强度平滑曲线。
    infraredSeries->setPointsVisible(false);                 // 隐藏红外数据点标记。
    infraredSeries->setName(QStringLiteral("红外"));         // 设置红外曲线图例名称。

    proximitySeries = new QSplineSeries();                   // 创建接近值平滑曲线。
    proximitySeries->setPointsVisible(false);                // 隐藏接近数据点标记。
    proximitySeries->setName(QStringLiteral("接近"));        // 设置接近曲线图例名称。

    sensorChart = new QChart();                              // 创建承载坐标轴和曲线的图表。
    sensorChart->setBackgroundBrush(QBrush(Qt::black));      // 图表背景设为黑色。
    sensorChart->setTitleBrush(QBrush(Qt::white));           // 图表标题颜色设为白色。
    sensorChart->legend()->setBrush(QBrush(Qt::white));      // 设置图例区域画刷。
    sensorChart->legend()->setLabelColor(Qt::white);         // 图例文字设为白色。

    sensorChart->addAxis(sensorValueAxis, Qt::AlignLeft);    // 把数值轴安装在图表左侧。
    sensorChart->addAxis(sampleAxis, Qt::AlignBottom);       // 把采样轴安装在图表底部。
    sensorChart->addSeries(ambientLightSeries);              // 将环境光曲线加入图表。
    sensorChart->addSeries(infraredSeries);                  // 将红外曲线加入图表。
    sensorChart->addSeries(proximitySeries);                 // 将接近曲线加入图表。

    sensorChart->setAnimationOptions(QChart::SeriesAnimations); // 数据变化时启用曲线动画。

    ambientLightSeries->attachAxis(sampleAxis);              // 环境光曲线使用采样横轴。
    ambientLightSeries->attachAxis(sensorValueAxis);         // 环境光曲线使用数值纵轴。
    infraredSeries->attachAxis(sampleAxis);                  // 红外曲线使用采样横轴。
    infraredSeries->attachAxis(sensorValueAxis);             // 红外曲线使用数值纵轴。
    proximitySeries->attachAxis(sampleAxis);                 // 接近曲线使用采样横轴。
    proximitySeries->attachAxis(sensorValueAxis);            // 接近曲线使用数值纵轴。

    ui->graphicsView->setChart(sensorChart);                 // 将图表交给 .ui 中的 QChartView 显示。
    ui->graphicsView->setRenderHint(QPainter::Antialiasing); // 启用抗锯齿，使曲线更平滑。
}

/** 仅在 ARM 开发板上创建真实硬件对象并准备 sysfs 接口。 */
void BoardDiagnosticsPage::initializeBoardHardware()
{
#if defined(__arm__) // 桌面系统没有这些设备节点，因此桌面编译时跳过硬件代码。
    ambientLightSensor = new Ap3216cSensor(this); // 创建 AP3216C 对象并交给页面管理。
    motionSensor = new Icm20608Sensor(this);      // 创建 ICM20608 对象并交给页面管理。

    connect(ambientLightSensor, &Ap3216cSensor::sensorDataChanged, // AP3216C 数据更新信号。
            this, &BoardDiagnosticsPage::updateAp3216cDisplay);    // 刷新文本和曲线。
    connect(motionSensor, &Icm20608Sensor::sensorDataChanged,      // ICM20608 数据更新信号。
            this, &BoardDiagnosticsPage::updateIcm20608Display);   // 刷新七个数值框。

    QFile ledTriggerFile(QStringLiteral(                         // 临时文件对象用于修改 LED 触发模式。
        "/sys/class/leds/sys-led/trigger"));                     // 内核 LED trigger 属性路径。
    if (ledTriggerFile.open(QIODevice::WriteOnly))               // 尝试以只写方式打开 trigger。
    {
        ledTriggerFile.write("none");                            // 禁用系统触发器，改为用户手动控制。
        ledTriggerFile.close();                                  // 写完立即释放文件描述符。
    }

    ledBrightnessFile.setFileName(                               // 配置长期复用的 LED 亮度文件对象。
        "/sys/devices/platform/leds/leds/sys-led/brightness");   // 写 1 点亮、写 0 熄灭。

    if (!ledBrightnessFile.exists())                             // 设备驱动没有导出 LED 节点。
        ui->ledBt->setText("未获取到LED设备！");                 // 在按钮上显示不可用状态。

    readLedState();                                              // 读取初始 LED 状态并刷新按钮文字。

    buzzerBrightnessFile.setFileName(                            // 配置长期复用的蜂鸣器文件对象。
        "/sys/devices/platform/leds/leds/beep/brightness");      // 写 1 开启、写 0 关闭蜂鸣器。

    if (!buzzerBrightnessFile.exists())                          // 设备驱动没有导出蜂鸣器节点。
        ui->beepBt->setText("未获取到BEEP设备！");               // 在按钮上显示不可用状态。

    readBuzzerState();                                           // 读取初始蜂鸣器状态并刷新按钮文字。
#endif // 结束 ARM 专用硬件初始化代码。
}

/** 清除 AP3216C 三条曲线并把横轴恢复为初始范围。 */
void BoardDiagnosticsPage::clearSensorChart()
{
    ambientLightSeries->clear();                    // 删除全部环境光历史点。
    infraredSeries->clear();                        // 删除全部红外历史点。
    proximitySeries->clear();                       // 删除全部接近历史点。
    sampleAxis->setRange(0, MaximumVisibleSamples); // 横轴重新显示 0～40。
    sampleIndex = 0;                                // 下一次数据从采样序号 0 开始。
}

/** ARM 平台上开启 AP3216C 定时轮询。 */
void BoardDiagnosticsPage::startAp3216cPolling()
{
#if defined(__arm__) // 只有开发板上才存在 ambientLightSensor 对象。
    ambientLightSensor->setPollingEnabled(true); // 立即读取一次并启动 500ms 定时器。
#endif // 桌面平台保持空实现。
}

/** ARM 平台上停止 AP3216C 定时轮询。 */
void BoardDiagnosticsPage::stopAp3216cPolling()
{
#if defined(__arm__) // 只有开发板上才访问真实传感器对象。
    ambientLightSensor->setPollingEnabled(false); // 停止 AP3216C 内部轮询定时器。
#endif // 结束 ARM 专用语句。
    sampleIndex = 0; // 无论平台如何，都把下一次曲线采样序号复位。
}

/** 把 ICM20608 最新七个物理量写入对应文本框。 */
void BoardDiagnosticsPage::updateIcm20608Display()
{
    ui->gxdataEdit->setText(motionSensor->gyroscopeX());    // 显示 X 轴角速度。
    ui->gydataEdit->setText(motionSensor->gyroscopeY());    // 显示 Y 轴角速度。
    ui->gzdataEdit->setText(motionSensor->gyroscopeZ());    // 显示 Z 轴角速度。
    ui->axdataEdit->setText(motionSensor->accelerationX()); // 显示 X 轴加速度。
    ui->aydataEdit->setText(motionSensor->accelerationY()); // 显示 Y 轴加速度。
    ui->azdataEdit->setText(motionSensor->accelerationZ()); // 显示 Z 轴加速度。
    ui->tempdataEdit->setText(motionSensor->temperature()); // 显示芯片温度。
}

/** 读取 AP3216C 最新值，刷新文本并追加曲线数据点。 */
void BoardDiagnosticsPage::updateAp3216cDisplay()
{
    const QString ambientLight = ambientLightSensor->ambientLight(); // 保存环境光文本。
    const QString proximity = ambientLightSensor->proximity();       // 保存接近值文本。
    const QString infrared = ambientLightSensor->infrared();         // 保存红外值文本。

    ui->alsEdit->setText(ambientLight); // 在 ALS 文本框显示环境光。
    ui->irEdit->setText(infrared);      // 在 IR 文本框显示红外值。
    ui->psEdit->setText(proximity);     // 在 PS 文本框显示接近值。

    if (sampleIndex > MaximumVisibleSamples) // 超过一屏 40 个点后移动显示窗口。
    {
        ambientLightSeries->remove(0); // 删除环境光曲线最旧的第 0 个点。
        infraredSeries->remove(0);     // 删除红外曲线最旧的第 0 个点。
        proximitySeries->remove(0);    // 删除接近曲线最旧的第 0 个点。
        sampleAxis->setRange(                         // 横轴窗口随 sampleIndex 向右移动。
            sampleIndex - MaximumVisibleSamples,      // 左边界保留最近 40 个采样。
            sampleIndex);                             // 右边界是当前采样序号。
    }
    ambientLightSeries->append(                       // 向环境光曲线追加新点。
        QPointF(sampleIndex, ambientLight.toInt()));  // X 是序号，Y 是转成整数的传感器值。
    infraredSeries->append(                           // 向红外曲线追加新点。
        QPointF(sampleIndex, infrared.toInt()));      // 使用相同采样序号保证三条曲线对齐。
    proximitySeries->append(                          // 向接近曲线追加新点。
        QPointF(sampleIndex, proximity.toInt()));     // 把文本接近值转换成整数坐标。
    ++sampleIndex;                                    // 为下一次采样准备新的横轴序号。
}

/** 根据当前 LED 状态向 brightness 写入相反值。 */
void BoardDiagnosticsPage::toggleLed()
{
    const bool currentlyOn = readLedState(); // 先读取当前状态，决定接下来写 0 还是 1。

    if (!ledBrightnessFile.exists()) // 节点不存在时不能控制 LED。
        return; // 保持页面当前状态并结束。

    if (!ledBrightnessFile.open(QIODevice::WriteOnly)) // 尝试以只写方式打开亮度节点。
    {
        qWarning() << ledBrightnessFile.errorString(); // 输出权限或设备错误。
        return; // 打开失败时不能继续写入。
    }

    ledBrightnessFile.write(currentlyOn ? "0" : "1"); // 当前亮则写 0，当前灭则写 1。
    ledBrightnessFile.close();                         // 写完后释放 sysfs 文件描述符。

    readLedState(); // 再读一次实际状态并更新按钮文字。
}

/** @return LED brightness 为 1 时返回 true，否则返回 false。 */
bool BoardDiagnosticsPage::readLedState()
{
    if (!ledBrightnessFile.exists()) // sysfs 节点不存在说明无法读取状态。
        return false; // 把不存在按关闭状态处理。

    if (!ledBrightnessFile.open(QIODevice::ReadOnly)) // 尝试只读打开 brightness。
    {
        qWarning() << ledBrightnessFile.errorString(); // 输出文件打开错误。
        return false; // 读取失败按关闭状态返回。
    }

    QTextStream input(&ledBrightnessFile); // 用文本流读取 sysfs 中的数字字符串。
    const QString value = input.readLine(); // 读取第一行，正常为 0 或 1。
    ledBrightnessFile.close();              // 读取完成后立即关闭节点。
    if (value == QLatin1String("1"))        // 字符串 1 表示 LED 已点亮。
    {
        ui->ledBt->setText("LED点亮");      // 更新按钮为点亮状态。
        return true;                        // 把实际状态返回给 toggleLed()。
    }
    else                                    // 其他值均按 LED 熄灭处理。
    {
        ui->ledBt->setText("LED熄灭");      // 更新按钮为熄灭状态。
        return false;                       // 返回关闭状态。
    }
}

/** Qt 自动槽：响应 LED 按钮点击。 */
void BoardDiagnosticsPage::on_ledBt_clicked()
{
    toggleLed(); // 执行一次 LED 状态翻转。
}

/** 根据当前蜂鸣器状态向 brightness 写入相反值。 */
void BoardDiagnosticsPage::toggleBuzzer()
{
    const bool currentlyOn = readBuzzerState(); // 先读取当前开关状态。

    if (!buzzerBrightnessFile.exists()) // sysfs 节点不存在时不能控制蜂鸣器。
        return; // 直接结束。

    if (!buzzerBrightnessFile.open(QIODevice::WriteOnly)) // 尝试以只写方式打开节点。
    {
        qWarning() << buzzerBrightnessFile.errorString(); // 输出权限或设备错误。
        return; // 打开失败，不继续写入。
    }

    buzzerBrightnessFile.write(currentlyOn ? "0" : "1"); // 当前开则写 0，当前关则写 1。
    buzzerBrightnessFile.close();                         // 写完释放文件描述符。

    readBuzzerState(); // 再次读取实际状态并更新按钮文字。
}

/** @return 蜂鸣器 brightness 为 1 时返回 true，否则返回 false。 */
bool BoardDiagnosticsPage::readBuzzerState()
{
    if (!buzzerBrightnessFile.exists()) // 节点不存在说明无法读取蜂鸣器状态。
        return false; // 按关闭状态返回。

    if (!buzzerBrightnessFile.open(QIODevice::ReadOnly)) // 尝试以只读方式打开节点。
    {
        qWarning() << buzzerBrightnessFile.errorString(); // 输出打开失败原因。
        return false; // 读取失败按关闭状态处理。
    }

    QTextStream input(&buzzerBrightnessFile); // 将 sysfs 文件包装为文本输入流。
    const QString value = input.readLine();    // 读取第一行的 0 或 1。
    buzzerBrightnessFile.close();              // 读完关闭文件。
    if (value == QLatin1String("1"))           // 1 表示蜂鸣器开启。
    {
        ui->beepBt->setText("BEEP开");         // 更新按钮为开启状态。
        return true;                           // 返回开启。
    }
    else                                       // 其他值均按关闭处理。
    {
        ui->beepBt->setText("BEEP关");         // 更新按钮为关闭状态。
        return false;                          // 返回关闭。
    }
}

/** Qt 自动槽：响应蜂鸣器按钮点击。 */
void BoardDiagnosticsPage::on_beepBt_clicked()
{
    toggleBuzzer(); // 执行一次蜂鸣器状态翻转。
}

/** Qt 自动槽：启动 ICM20608 周期读取。 */
void BoardDiagnosticsPage::on_start2060Bt_clicked()
{
#if defined(__arm__) // 桌面平台没有 motionSensor 实例。
    motionSensor->setPollingEnabled(true);      // 立即读取并启动 500ms 轮询。
    ui->start2060Bt->setText("已启动检测");     // 在按钮上显示运行状态。
#endif // 结束 ARM 专用操作。
}

/** Qt 自动槽：停止 ICM20608 读取并清空页面数值。 */
void BoardDiagnosticsPage::on_stop2060Bt_clicked()
{
#if defined(__arm__) // 只有开发板上才能访问真实传感器对象。
    motionSensor->setPollingEnabled(false);     // 停止 ICM20608 内部轮询定时器。
    ui->start2060Bt->setText("已停止检测");     // 更新启动按钮状态文字。
#endif // 结束 ARM 专用操作。

    ui->gxdataEdit->setText("无"); // 清空 X 轴角速度显示。
    ui->gydataEdit->setText("无"); // 清空 Y 轴角速度显示。
    ui->gzdataEdit->setText("无"); // 清空 Z 轴角速度显示。

    ui->axdataEdit->setText("无"); // 清空 X 轴加速度显示。
    ui->aydataEdit->setText("无"); // 清空 Y 轴加速度显示。
    ui->azdataEdit->setText("无"); // 清空 Z 轴加速度显示。

    ui->tempdataEdit->setText("无"); // 清空温度显示。
}
