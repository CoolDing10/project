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
#ifndef ICM20608_H // 如果本头文件尚未被包含，才编译其内容。
#define ICM20608_H // 定义保护宏，防止类和元对象属性重复声明。

#include <QObject> // Icm20608Sensor 的基类，提供对象树和信号槽。
class QTimer;      // 前向声明 500ms 轮询定时器。

class Icm20608Sensor final : public QObject // final 表示该硬件封装不作为其他类的基类。
{
    Q_OBJECT // 启用 Qt 元对象、信号槽和属性系统。
    Q_PROPERTY(QString gyroscopeX READ gyroscopeX NOTIFY sensorDataChanged)       // 对外暴露只读 X 轴角速度属性。
    Q_PROPERTY(QString gyroscopeY READ gyroscopeY NOTIFY sensorDataChanged)       // 对外暴露只读 Y 轴角速度属性。
    Q_PROPERTY(QString gyroscopeZ READ gyroscopeZ NOTIFY sensorDataChanged)       // 对外暴露只读 Z 轴角速度属性。
    Q_PROPERTY(QString accelerationX READ accelerationX NOTIFY sensorDataChanged) // 对外暴露只读 X 轴加速度属性。
    Q_PROPERTY(QString accelerationY READ accelerationY NOTIFY sensorDataChanged) // 对外暴露只读 Y 轴加速度属性。
    Q_PROPERTY(QString accelerationZ READ accelerationZ NOTIFY sensorDataChanged) // 对外暴露只读 Z 轴加速度属性。
    Q_PROPERTY(QString temperature READ temperature NOTIFY sensorDataChanged)     // 对外暴露只读芯片温度属性。

public: // 供页面调用的构造、轮询控制和读数访问接口。
    /** @param parent Qt 父对象；非空时由父对象负责本实例的生命周期。 */
    explicit Icm20608Sensor(QObject *parent = nullptr); // 创建并连接内部轮询定时器。
    ~Icm20608Sensor() override = default;               // QObject 默认析构会自动释放子定时器。

    /** @param enabled true 立即读取并启动轮询，false 停止轮询。 */
    Q_INVOKABLE void setPollingEnabled(bool enabled); // 可通过 Qt 元对象系统跨组件调用。

    // 7 个传感器读数（已转换为人类可读的字符串格式）
    QString gyroscopeX() const;    // 陀螺仪 X (°/s)
    QString gyroscopeY() const;    // 陀螺仪 Y (°/s)
    QString gyroscopeZ() const;    // 陀螺仪 Z (°/s)
    QString accelerationX() const; // 加速度 X (g)
    QString accelerationY() const; // 加速度 Y (g)
    QString accelerationZ() const; // 加速度 Z (g)
    QString temperature() const;   // 芯片温度 (℃)

private slots: // 只供类内部定时器连接使用的槽。
    void pollSensorData(); // 打开设备、读取 7 个 qint32、换算单位并发出更新信号。

private: // 内部定时器与最近一次采样结果缓存。
    QTimer *pollingTimer;       // 500ms 轮询定时器
    QString gyroscopeXValue;    // 陀螺仪 X 缓存
    QString gyroscopeYValue;    // 陀螺仪 Y 缓存
    QString gyroscopeZValue;    // 陀螺仪 Z 缓存
    QString accelerationXValue; // 加速度 X 缓存
    QString accelerationYValue; // 加速度 Y 缓存
    QString accelerationZValue; // 加速度 Z 缓存
    QString temperatureValue;   // 温度缓存

signals: // 供 UI 或业务层订阅的数据刷新通知。
    void sensorDataChanged(); // 七项缓存完成一次更新后发射。
};

#endif // ICM20608_H：结束头文件保护。
