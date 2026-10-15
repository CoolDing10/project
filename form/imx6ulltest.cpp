#include "imx6ulltest.h"
#include "ui_imx6ulltest.h"

#include "ap3216c.h"
#include <QDebug>
#include <QVBoxLayout>
#include <QtCharts>

BoardDiagnosticsPage::BoardDiagnosticsPage(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::imx6ullTest),
    sensorChart(nullptr),
    ambientLightSensor(nullptr),
    motionSensor(nullptr),
    sampleAxis(nullptr),
    sensorValueAxis(nullptr),
    ambientLightSeries(nullptr),
    infraredSeries(nullptr),
    proximitySeries(nullptr)
{
    ui->setupUi(this);

    /*
     * 原 .ui 将页面最小高度固定为 600px，嵌入带有顶部标题区的
     * 1024x600 主界面后会把底部裁掉。实际页签内容最高约 491px，
     * 因此解除根页面限制，交由主界面布局分配约 548px 的可用高度。
     */
    setMinimumSize(0, 0);
    setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
    ui->centralwidget->setMinimumSize(0, 0);
    ui->tabWidget->setMinimumSize(0, 0);
    ui->tabWidget->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);

    /* .ui 中保留 centralwidget 作为内容容器，并用布局让页面跟随主窗口缩放。 */
    QVBoxLayout *contentLayout = new QVBoxLayout(ui->centralwidget);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->addWidget(ui->tabWidget);

    QVBoxLayout *pageLayout = new QVBoxLayout(this);
    pageLayout->setContentsMargins(0, 0, 0, 0);
    pageLayout->addWidget(ui->centralwidget);

    initializeSensorChart();
    initializeBoardHardware();

    connect(ui->pushButton, &QPushButton::clicked,
            this, &BoardDiagnosticsPage::stopAp3216cPolling);
    connect(ui->btnClear, &QPushButton::clicked,
            this, &BoardDiagnosticsPage::clearSensorChart);
    connect(ui->btnStartAndStop, &QPushButton::clicked,
            this, &BoardDiagnosticsPage::startAp3216cPolling);
}

BoardDiagnosticsPage::~BoardDiagnosticsPage()
{
    delete ui;
}


void BoardDiagnosticsPage::initializeSensorChart()
{
    // 创建横纵坐标轴并设置显示范围
    sampleAxis = new QValueAxis();
    sensorValueAxis = new QValueAxis();
    sampleAxis->setLabelsColor(Qt::white);
    sensorValueAxis->setLabelsColor(Qt::white);
    sampleAxis->setLinePenColor(Qt::white);
    sensorValueAxis->setLinePenColor(Qt::white);
    sampleAxis->setTitleBrush(QBrush(Qt::white));
    sensorValueAxis->setTitleBrush(QBrush(Qt::white));

    sampleAxis->setTitleText(QStringLiteral("采样序号"));
    sensorValueAxis->setTitleText(QStringLiteral("传感器数值"));
    sampleAxis->setRange(0, MaximumVisibleSamples);
    sensorValueAxis->setRange(0, MaximumSensorValue);

    ambientLightSeries = new QSplineSeries();
    ambientLightSeries->setPointsVisible(false);
    ambientLightSeries->setName(QStringLiteral("环境光"));

    //创建ir曲线对象
    infraredSeries = new QSplineSeries();
    infraredSeries->setPointsVisible(false);
    infraredSeries->setName(QStringLiteral("红外"));

    //创建ps曲线对象
    proximitySeries = new QSplineSeries();
    proximitySeries->setPointsVisible(false);
    proximitySeries->setName(QStringLiteral("接近"));

    sensorChart = new QChart();
    sensorChart->setBackgroundBrush(QBrush(Qt::black));
    sensorChart->setTitleBrush(QBrush(Qt::white));
    sensorChart->legend()->setBrush(QBrush(Qt::white));
    sensorChart->legend()->setLabelColor(Qt::white);

    sensorChart->addAxis(sensorValueAxis, Qt::AlignLeft);
    sensorChart->addAxis(sampleAxis, Qt::AlignBottom);
    sensorChart->addSeries(ambientLightSeries);
    sensorChart->addSeries(infraredSeries);
    sensorChart->addSeries(proximitySeries);

    sensorChart->setAnimationOptions(QChart::SeriesAnimations);

    ambientLightSeries->attachAxis(sampleAxis);
    ambientLightSeries->attachAxis(sensorValueAxis);

    //ir曲线关联到X，Y轴
    infraredSeries->attachAxis(sampleAxis);
    infraredSeries->attachAxis(sensorValueAxis);

    //ps曲线关联到X，Y轴
    proximitySeries->attachAxis(sampleAxis);
    proximitySeries->attachAxis(sensorValueAxis);

    ui->graphicsView->setChart(sensorChart);
    ui->graphicsView->setRenderHint(QPainter::Antialiasing);
}
void BoardDiagnosticsPage::initializeBoardHardware()
{  
#if defined(__arm__)
    ambientLightSensor = new Ap3216cSensor(this);
    motionSensor = new Icm20608Sensor(this);
    /* 只能在开发板上开启获取数据，Ubuntu上是没有ap3216c传感器的 */

    connect(ambientLightSensor, &Ap3216cSensor::sensorDataChanged,
            this, &BoardDiagnosticsPage::updateAp3216cDisplay);
    connect(motionSensor, &Icm20608Sensor::sensorDataChanged,
            this, &BoardDiagnosticsPage::updateIcm20608Display);

    /* led相关------------------------------------------------------------ */
    QFile ledTriggerFile(QStringLiteral("/sys/class/leds/sys-led/trigger"));
    if (ledTriggerFile.open(QIODevice::WriteOnly)) {
        ledTriggerFile.write("none");
        ledTriggerFile.close();
    }

    /* 开发板的LED控制接口 */
    ledBrightnessFile.setFileName("/sys/devices/platform/leds/leds/sys-led/brightness");

    if (!ledBrightnessFile.exists())
        /* 设置按钮的初始化文本 */
        ui->ledBt->setText("未获取到LED设备！");

    /* 获取LED的状态 */
    readLedState();
    /* --------------------------------------------------------------------- */

    /* beep相关------------------------------------------------------------- */
    /* 开发板的蜂鸣器控制接口 */
    buzzerBrightnessFile.setFileName("/sys/devices/platform/leds/leds/beep/brightness");

    if (!buzzerBrightnessFile.exists())
        /* 设置按钮的初始化文本 */
        ui->beepBt->setText("未获取到BEEP设备！");

    /* 获取BEEP的状态 */
    readBuzzerState();
#endif
}

void BoardDiagnosticsPage::clearSensorChart()
{
    ambientLightSeries->clear();
    infraredSeries->clear();
    proximitySeries->clear();
    sampleAxis->setRange(0, MaximumVisibleSamples);
    sampleIndex = 0;
}

void BoardDiagnosticsPage::startAp3216cPolling()
{
#if defined(__arm__)
    ambientLightSensor->setPollingEnabled(true);
#endif
}

void BoardDiagnosticsPage::stopAp3216cPolling()
{
#if defined(__arm__)
    ambientLightSensor->setPollingEnabled(false);
#endif
    sampleIndex = 0;
}

void BoardDiagnosticsPage::updateIcm20608Display()
{
    ui->gxdataEdit->setText(motionSensor->gyroscopeX());
    ui->gydataEdit->setText(motionSensor->gyroscopeY());
    ui->gzdataEdit->setText(motionSensor->gyroscopeZ());
    ui->axdataEdit->setText(motionSensor->accelerationX());
    ui->aydataEdit->setText(motionSensor->accelerationY());
    ui->azdataEdit->setText(motionSensor->accelerationZ());
    ui->tempdataEdit->setText(motionSensor->temperature());
}

void BoardDiagnosticsPage::updateAp3216cDisplay()
{
    const QString ambientLight = ambientLightSensor->ambientLight();
    const QString proximity = ambientLightSensor->proximity();
    const QString infrared = ambientLightSensor->infrared();

    ui->alsEdit->setText(ambientLight);
    ui->irEdit->setText(infrared);
    ui->psEdit->setText(proximity);

    //画曲线代码
    if (sampleIndex > MaximumVisibleSamples) {
        ambientLightSeries->remove(0);
        infraredSeries->remove(0);
        proximitySeries->remove(0);
        sampleAxis->setRange(sampleIndex - MaximumVisibleSamples, sampleIndex);
    }
    ambientLightSeries->append(QPointF(sampleIndex, ambientLight.toInt()));
    infraredSeries->append(QPointF(sampleIndex, infrared.toInt()));
    proximitySeries->append(QPointF(sampleIndex, proximity.toInt()));
    ++sampleIndex;
}


/* led相关 -------------------------------------------*/

void BoardDiagnosticsPage::toggleLed()
{
    const bool currentlyOn = readLedState();

    if (!ledBrightnessFile.exists())
        return;

    if (!ledBrightnessFile.open(QIODevice::WriteOnly)) {
        qWarning() << ledBrightnessFile.errorString();
        return;
    }

    ledBrightnessFile.write(currentlyOn ? "0" : "1");
    ledBrightnessFile.close();

    readLedState();
}

bool BoardDiagnosticsPage::readLedState()
{
    if (!ledBrightnessFile.exists())
        return false;

    if (!ledBrightnessFile.open(QIODevice::ReadOnly)) {
        qWarning() << ledBrightnessFile.errorString();
        return false;
    }

    QTextStream input(&ledBrightnessFile);
    const QString value = input.readLine();
    ledBrightnessFile.close();
    if (value == QLatin1String("1")) {
        ui->ledBt->setText("LED点亮");
        return true;
    } else {
        ui->ledBt->setText("LED熄灭");
        return false;
    }
}



void BoardDiagnosticsPage::on_ledBt_clicked()
{
    toggleLed();
}


/* beep相关函数定义 -------------------------------*/
void BoardDiagnosticsPage::toggleBuzzer()
{
    const bool currentlyOn = readBuzzerState();

    if (!buzzerBrightnessFile.exists())
        return;

    if (!buzzerBrightnessFile.open(QIODevice::WriteOnly)) {
        qWarning() << buzzerBrightnessFile.errorString();
        return;
    }

    buzzerBrightnessFile.write(currentlyOn ? "0" : "1");
    buzzerBrightnessFile.close();

    readBuzzerState();
}

bool BoardDiagnosticsPage::readBuzzerState()
{
    if (!buzzerBrightnessFile.exists())
        return false;

    if (!buzzerBrightnessFile.open(QIODevice::ReadOnly)) {
        qWarning() << buzzerBrightnessFile.errorString();
        return false;
    }

    QTextStream input(&buzzerBrightnessFile);
    const QString value = input.readLine();
    buzzerBrightnessFile.close();
    if (value == QLatin1String("1")) {
        ui->beepBt->setText("BEEP开");
        return true;
    } else {
        ui->beepBt->setText("BEEP关");
        return false;
    }
}



void BoardDiagnosticsPage::on_beepBt_clicked()
{
    toggleBuzzer();
}

void BoardDiagnosticsPage::on_start2060Bt_clicked()
{
#if defined(__arm__)
    motionSensor->setPollingEnabled(true);
    ui->start2060Bt->setText("已启动检测");
#endif
}

void BoardDiagnosticsPage::on_stop2060Bt_clicked()
{
#if defined(__arm__)
    motionSensor->setPollingEnabled(false);
    ui->start2060Bt->setText("已停止检测");
#endif

    ui->gxdataEdit->setText("无");
    ui->gydataEdit->setText("无");
    ui->gzdataEdit->setText("无");

    ui->axdataEdit->setText("无");
    ui->aydataEdit->setText("无");
    ui->azdataEdit->setText("无");

    ui->tempdataEdit->setText("无");
}
