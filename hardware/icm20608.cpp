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
#include "icm20608.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <QDebug>
#include <QTimer>
#include <unistd.h>

namespace
{
    const char SensorDevicePath[] = "/dev/icm20608";
    const int PollIntervalMs = 500;
    const float GyroscopeScale = 16.4f;
    const float AccelerationScale = 2048.0f;
}

Icm20608Sensor::Icm20608Sensor(QObject *parent)
    : QObject(parent),
      pollingTimer(new QTimer(this))
{
    pollingTimer->setInterval(PollIntervalMs);
    connect(pollingTimer, &QTimer::timeout,
            this, &Icm20608Sensor::pollSensorData);
}

/*
 * 打开 /dev/icm20608 字符设备，读取 7×sizeof(int32)=28 字节，
 * 转换为人类可读的浮点数字符串，然后发射 sensorDataChanged()。
 *
 * 桌面 /dev/icm20608 不存在时静默失败，QTimer 仍然运行但无输出。
 */
void Icm20608Sensor::pollSensorData()
{
    const int descriptor = open(SensorDevicePath, O_RDWR);
    if (descriptor < 0)
    {
        qWarning() << "Unable to open" << SensorDevicePath << ':'
                   << std::strerror(errno);
        return;
    }

    qint32 rawValues[7] = {};
    const ssize_t bytesRead = read(descriptor, rawValues, sizeof(rawValues));
    const int savedErrno = errno;
    close(descriptor);

    /*
     * 某些旧版板端驱动在成功 copy_to_user 后返回 0，而规范实现返回
     * 实际字节数。两种成功形式都兼容；负数和非完整正长度视为失败。
     */
    if (bytesRead < 0 || (bytesRead > 0 && bytesRead != ssize_t(sizeof(rawValues))))
    {
        qWarning() << "Unable to read a complete ICM20608 sample:"
                   << (bytesRead < 0 ? std::strerror(savedErrno)
                                     : "unexpected byte count");
        return;
    }

    // rawValues[0..2] = 陀螺仪 X/Y/Z → 除以 GyroscopeScale
    gyroscopeXValue = QString::number(rawValues[0] / GyroscopeScale, 'f', 2);
    gyroscopeYValue = QString::number(rawValues[1] / GyroscopeScale, 'f', 2);
    gyroscopeZValue = QString::number(rawValues[2] / GyroscopeScale, 'f', 2);
    // rawValues[3..5] = 加速度 X/Y/Z → 除以 AccelerationScale
    accelerationXValue = QString::number(rawValues[3] / AccelerationScale, 'f', 2);
    accelerationYValue = QString::number(rawValues[4] / AccelerationScale, 'f', 2);
    accelerationZValue = QString::number(rawValues[5] / AccelerationScale, 'f', 2);
    // rawValues[6] = 温度原始值
    temperatureValue = QString::number((rawValues[6] - 25.0f) / 326.8f + 25.0f,
                                       'f', 2);
    emit sensorDataChanged();
}

QString Icm20608Sensor::gyroscopeX() const { return gyroscopeXValue; }
QString Icm20608Sensor::gyroscopeY() const { return gyroscopeYValue; }
QString Icm20608Sensor::gyroscopeZ() const { return gyroscopeZValue; }
QString Icm20608Sensor::accelerationX() const { return accelerationXValue; }
QString Icm20608Sensor::accelerationY() const { return accelerationYValue; }
QString Icm20608Sensor::accelerationZ() const { return accelerationZValue; }
QString Icm20608Sensor::temperature() const { return temperatureValue; }

void Icm20608Sensor::setPollingEnabled(bool enabled)
{
    if (enabled)
    {
        pollSensorData();      // 启用时立即读取一次
        pollingTimer->start(); // 然后每 500ms 自动轮询
    }
    else
        pollingTimer->stop();
}