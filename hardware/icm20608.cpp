/*
 * ============================================================================
 * icm20608.cpp — ICM20608 6轴传感器字符设备读取实现
 * ============================================================================
 *
 * 通过 /dev/icm20608 字符设备读取原始传感器数据。
 * 驱动返回 7 个 int32 的原始值，需按量程系数转换。
 *
 * 量程与转换公式：
 *   陀螺仪量程 ±2000°/s  → raw/16.4   = °/s
 *   加速度量程 ±16g       → raw/2048   = g
 *   温度                 → (raw-25)/326.8 + 25 = ℃
 *
 * 兼容性处理：某些旧版驱动 read() 返回 0（而非 28 字节），但实际数据
 * 已写入缓冲区。代码将 bytesRead==0 也视为成功。
 */
#include "icm20608.h" // 引入 Icm20608Sensor 类声明。

#include <cerrno>  // 提供 errno 系统错误码。
#include <cstring> // 提供 std::strerror() 错误码文本转换。
#include <fcntl.h> // 提供 open()、O_RDWR。
#include <QDebug>  // 提供 qWarning() 日志。
#include <QTimer>  // 实现 500ms 周期采样。
#include <unistd.h> // 提供 read()、close()。

namespace // 匿名命名空间限制设备路径和换算常量只在本文件使用。
{
    const char SensorDevicePath[] = "/dev/icm20608"; // Linux 字符设备节点路径。
    const int PollIntervalMs = 500;                   // 轮询周期，单位毫秒。
    const float GyroscopeScale = 16.4f;               // ±2000°/s 量程下原始值到 °/s 的除数。
    const float AccelerationScale = 2048.0f;          // ±16g 量程下原始值到 g 的除数。
} // 结束文件内部匿名命名空间。

Icm20608Sensor::Icm20608Sensor(QObject *parent) // parent 是 Qt 父对象。
    : QObject(parent),                         // 初始化 QObject 基类和对象树关系。
      pollingTimer(new QTimer(this))            // 创建由当前对象自动释放的采样定时器。
{
    pollingTimer->setInterval(PollIntervalMs);                  // 设置 500ms 超时间隔。
    connect(pollingTimer, &QTimer::timeout,                     // 定时器每次 timeout 时，
            this, &Icm20608Sensor::pollSensorData);             // 执行一次字符设备读取和单位换算。
}

/*
 * 打开 /dev/icm20608 字符设备，读取 7×sizeof(int32)=28 字节，
 * 转换为人类可读的浮点数字符串，然后发射 sensorDataChanged()。
 *
 * 桌面 /dev/icm20608 不存在时静默失败，QTimer 仍然运行但无输出。
 */
void Icm20608Sensor::pollSensorData() // 读取一组七通道原始数据，转换并更新缓存。
{
    const int descriptor = open(SensorDevicePath, O_RDWR); // descriptor 保存设备文件描述符，失败为 -1。
    if (descriptor < 0)                                   // 设备不存在或权限不足时退出本次采样。
    {
        qWarning() << "Unable to open" << SensorDevicePath << ':' // 输出打不开的设备路径，
                   << std::strerror(errno);                        // 以及当前 errno 对应的原因。
        return;                                                    // 保留上一组缓存值，不发变化信号。
    }

    qint32 rawValues[7] = {}; // 保存陀螺仪 3 项、加速度 3 项和温度 1 项原始整数。
    const ssize_t bytesRead = read(descriptor, rawValues, sizeof(rawValues)); // bytesRead 保存 read() 的返回长度。
    const int savedErrno = errno; // 在 close() 前保存可能属于 read() 的错误码。
    close(descriptor);            // 释放字符设备文件描述符。

    /*
     * 某些旧版板端驱动在成功 copy_to_user 后返回 0，而规范实现返回
     * 实际字节数。两种成功形式都兼容；负数和非完整正长度视为失败。
     */
    if (bytesRead < 0 || (bytesRead > 0 && bytesRead != ssize_t(sizeof(rawValues)))) // 负值或不完整正长度都视为失败。
    {
        qWarning() << "Unable to read a complete ICM20608 sample:"       // 输出统一的采样失败提示，
                   << (bytesRead < 0 ? std::strerror(savedErrno)          // 负值时输出系统错误文本，
                                     : "unexpected byte count");        // 短读时说明字节数异常。
        return;                                                          // 不使用残缺数据，不发更新信号。
    }

    // rawValues[0..2] = 陀螺仪 X/Y/Z → 除以 GyroscopeScale
    gyroscopeXValue = QString::number(rawValues[0] / GyroscopeScale, 'f', 2); // X 轴原始角速度换算为两位小数字符串。
    gyroscopeYValue = QString::number(rawValues[1] / GyroscopeScale, 'f', 2); // Y 轴原始角速度换算为两位小数字符串。
    gyroscopeZValue = QString::number(rawValues[2] / GyroscopeScale, 'f', 2); // Z 轴原始角速度换算为两位小数字符串。
    // rawValues[3..5] = 加速度 X/Y/Z → 除以 AccelerationScale
    accelerationXValue = QString::number(rawValues[3] / AccelerationScale, 'f', 2); // X 轴原始加速度换算为 g 并保留两位小数。
    accelerationYValue = QString::number(rawValues[4] / AccelerationScale, 'f', 2); // Y 轴原始加速度换算为 g 并保留两位小数。
    accelerationZValue = QString::number(rawValues[5] / AccelerationScale, 'f', 2); // Z 轴原始加速度换算为 g 并保留两位小数。
    // rawValues[6] = 温度原始值
    temperatureValue = QString::number((rawValues[6] - 25.0f) / 326.8f + 25.0f, // 按芯片公式换算温度，
                                       'f', 2);                                  // 固定保留两位小数。
    emit sensorDataChanged();                                                    // 七项缓存全部完成后通知接收者。
}

QString Icm20608Sensor::gyroscopeX() const { return gyroscopeXValue; }       // 返回最近 X 轴角速度字符串。
QString Icm20608Sensor::gyroscopeY() const { return gyroscopeYValue; }       // 返回最近 Y 轴角速度字符串。
QString Icm20608Sensor::gyroscopeZ() const { return gyroscopeZValue; }       // 返回最近 Z 轴角速度字符串。
QString Icm20608Sensor::accelerationX() const { return accelerationXValue; } // 返回最近 X 轴加速度字符串。
QString Icm20608Sensor::accelerationY() const { return accelerationYValue; } // 返回最近 Y 轴加速度字符串。
QString Icm20608Sensor::accelerationZ() const { return accelerationZValue; } // 返回最近 Z 轴加速度字符串。
QString Icm20608Sensor::temperature() const { return temperatureValue; }     // 返回最近芯片温度字符串。

void Icm20608Sensor::setPollingEnabled(bool enabled) // enabled 决定启动还是停止传感器轮询。
{
    if (enabled) // 开启时先立即刷新一次，避免等待第一个 500ms 周期。
    {
        pollSensorData();      // 启用时立即读取一次
        pollingTimer->start(); // 然后每 500ms 自动轮询
    }
    else // 用户关闭轮询时，
        pollingTimer->stop(); // 停止 timeout 信号，但保留最近一次缓存数据。
}
