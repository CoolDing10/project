/*
 * ============================================================================
 * icm20608.h — ICM20608 6轴惯性传感器驱动（3轴陀螺仪 + 3轴加速度计 + 温度）
 * ============================================================================
 *
 * 通过 /dev/icm20608 字符设备读取原始数据。
 * 仅 ARM 平台可用；桌面环境下无此设备节点，轮询无效果。
 *
 * 原始值含义（每次 read 返回 7 个 int32）：
 *   rawValues[0..2] = 陀螺仪 X/Y/Z 原始值  → 除以 16.4 得到 °/s
 *   rawValues[3..5] = 加速度 X/Y/Z 原始值  → 除以 2048 得到 g
 *   rawValues[6]    = 温度原始值           → (val - 25) / 326.8 + 25 得到 ℃
 *
 * 轮询间隔：500ms
 *
 * 安全规则（在 CameraStreamingPage 中使用）：
 *   - 合加速度 |mag-1.0| >= 0.55g  → 震动告警（Warning，15s 冷却）
 *   - 温度 >= 60℃                 → 温度告警（Critical，60s 冷却）
 */
#ifndef ICM20608_H
#define ICM20608_H

#include <QObject>
class QTimer;

class Icm20608Sensor final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString gyroscopeX READ gyroscopeX NOTIFY sensorDataChanged)
    Q_PROPERTY(QString gyroscopeY READ gyroscopeY NOTIFY sensorDataChanged)
    Q_PROPERTY(QString gyroscopeZ READ gyroscopeZ NOTIFY sensorDataChanged)
    Q_PROPERTY(QString accelerationX READ accelerationX NOTIFY sensorDataChanged)
    Q_PROPERTY(QString accelerationY READ accelerationY NOTIFY sensorDataChanged)
    Q_PROPERTY(QString accelerationZ READ accelerationZ NOTIFY sensorDataChanged)
    Q_PROPERTY(QString temperature READ temperature NOTIFY sensorDataChanged)

public:
    explicit Icm20608Sensor(QObject *parent = nullptr);
    ~Icm20608Sensor() override = default;

    Q_INVOKABLE void setPollingEnabled(bool enabled); // 启停 500ms 轮询

    // 7 个传感器读数（已转换为人类可读的字符串格式）
    QString gyroscopeX() const;    // 陀螺仪 X (°/s)
    QString gyroscopeY() const;    // 陀螺仪 Y (°/s)
    QString gyroscopeZ() const;    // 陀螺仪 Z (°/s)
    QString accelerationX() const; // 加速度 X (g)
    QString accelerationY() const; // 加速度 Y (g)
    QString accelerationZ() const; // 加速度 Z (g)
    QString temperature() const;   // 芯片温度 (℃)

private slots:
    void pollSensorData(); // 打开 /dev/icm20608，读取 28 字节，转换后发射信号

private:
    QTimer *pollingTimer;       // 500ms 轮询定时器
    QString gyroscopeXValue;    // 陀螺仪 X 缓存
    QString gyroscopeYValue;    // 陀螺仪 Y 缓存
    QString gyroscopeZValue;    // 陀螺仪 Z 缓存
    QString accelerationXValue; // 加速度 X 缓存
    QString accelerationYValue; // 加速度 Y 缓存
    QString accelerationZValue; // 加速度 Z 缓存
    QString temperatureValue;   // 温度缓存

signals:
    void sensorDataChanged(); // 任一值变化时发射
};

#endif // ICM20608_H