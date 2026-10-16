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
#ifndef AP3216C_H // 如果尚未包含过本头文件，才编译下面内容。
#define AP3216C_H // 定义头文件保护宏，避免类被重复声明。

#include <QObject> // Ap3216cSensor 的基类，提供对象树和信号槽机制。
class QTimer;      // 前向声明轮询定时器，减少头文件依赖。

class Ap3216cSensor final : public QObject // final 表示该传感器封装类不再被继承。
{
    Q_OBJECT // 启用 Qt 元对象、信号槽和 Q_PROPERTY 功能。
    Q_PROPERTY(QString ambientLight READ ambientLight NOTIFY sensorDataChanged) // 暴露只读环境光属性，变化信号为 sensorDataChanged。
    Q_PROPERTY(QString proximity READ proximity NOTIFY sensorDataChanged)       // 暴露只读接近值属性。
    Q_PROPERTY(QString infrared READ infrared NOTIFY sensorDataChanged)         // 暴露只读红外值属性。

public: // 对页面和其他业务对象开放的接口。
    /** @param parent Qt 父对象；非空时由父对象负责本实例的生命周期。 */
    explicit Ap3216cSensor(QObject *parent = nullptr); // 创建 500ms 轮询定时器并连接超时槽。
    ~Ap3216cSensor() override = default;               // 使用 QObject 默认析构，自动释放子定时器。

    /** @param enabled true 立即采样并启动定时器，false 停止定时轮询。 */
    Q_INVOKABLE void setPollingEnabled(bool enabled); // Q_INVOKABLE 允许通过 Qt 元对象系统调用此接口。

    QString ambientLight() const; // 返回最近一次采样的环境光字符串，读取操作不修改对象。
    QString proximity() const;    // 返回最近一次采样的接近距离字符串。
    QString infrared() const;     // 返回最近一次采样的红外强度字符串。

private: // 仅供类内部使用的读取工具和缓存状态。
    /*
     * 使用 POSIX open/read/close 直接读取 sysfs 节点。
     * 用 static 是因为它不依赖实例状态，只是纯工具函数。
     */
    /** @param devicePath 要读取的 sysfs 文件路径；返回去除换行的文本，失败返回 "N/A"。 */
    static QString readTextValue(const char *devicePath); // static 表示读取过程不依赖某个对象实例。

    QTimer *pollingTimer;      // 500ms 轮询定时器
    QString ambientLightValue; // 缓存光强值
    QString proximityValue;    // 缓存接近值
    QString infraredValue;     // 缓存红外值

private slots: // 仅由内部轮询定时器触发的槽函数。
    void pollSensorData(); // 依次读取三个 sysfs 节点，更新缓存并发出变化信号。

signals: // 对外通知传感器数据已完成一次刷新。
    void sensorDataChanged(); // 三项缓存更新后触发，接收者随后调用 getter 读取新值。
};

#endif // AP3216C_H：结束头文件保护。
