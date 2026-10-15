/*
 * ============================================================================
 * ap3216c.h — AP3216C 环境光/接近/红外传感器驱动
 * ============================================================================
 *
 * 通过 sysfs 接口读取传感器数据（Linux 驱动已将寄存器映射为文件节点）。
 * 仅 ARM 平台可用；桌面环境使用空实现（构造函数不报错，polling 无效果）。
 *
 * 读取路径：
 *   /sys/class/misc/ap3216c/als  — 环境光强度 (Ambient Light Sensor)
 *   /sys/class/misc/ap3216c/ps   — 接近距离 (Proximity Sensor)
 *   /sys/class/misc/ap3216c/ir   — 红外强度 (Infrared)
 *
 * 轮询机制：每 500ms 通过 QTimer 触发 pollSensorData()，
 * 读取三个 sysfs 节点并发射 sensorDataChanged() 信号。
 */
#ifndef AP3216C_H
#define AP3216C_H

#include <QObject>
class QTimer;

class Ap3216cSensor final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString ambientLight READ ambientLight NOTIFY sensorDataChanged)
    Q_PROPERTY(QString proximity READ proximity NOTIFY sensorDataChanged)
    Q_PROPERTY(QString infrared READ infrared NOTIFY sensorDataChanged)

public:
    explicit Ap3216cSensor(QObject *parent = nullptr);
    ~Ap3216cSensor() override = default;

    Q_INVOKABLE void setPollingEnabled(bool enabled); // 启停 500ms 轮询

    QString ambientLight() const; // 环境光强度值
    QString proximity() const;    // 接近距离值
    QString infrared() const;     // 红外强度值

private:
    /*
     * 使用 POSIX open/read/close 直接读取 sysfs 节点。
     * 用 static 是因为它不依赖实例状态，只是纯工具函数。
     */
    static QString readTextValue(const char *devicePath);

    QTimer *pollingTimer;      // 500ms 轮询定时器
    QString ambientLightValue; // 缓存光强值
    QString proximityValue;    // 缓存接近值
    QString infraredValue;     // 缓存红外值

private slots:
    void pollSensorData(); // 执行一次三个 sysfs 节点的读取

signals:
    void sensorDataChanged(); // 任一值发生变化时触发
};

#endif // AP3216C_H