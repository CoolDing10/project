/*
 * ============================================================================
 * ap3216c.cpp — AP3216C 传感器 sysfs 读取实现
 * ============================================================================
 *
 * 通过 POSIX 文件 I/O 读取 Linux 驱动导出的 sysfs 属性文件。
 * 每 500ms 轮询一次（QTimer），读取三个传感器值。
 *
 * 桌面环境兼容：sysfs 节点不存在时返回 "N/A"，通过 errno 记录。
 */
#include "ap3216c.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <QDebug>
#include <QTimer>
#include <unistd.h>

namespace
{
    const char AmbientLightPath[] = "/sys/class/misc/ap3216c/als"; // 环境光强度
    const char ProximityPath[] = "/sys/class/misc/ap3216c/ps";     // 接近距离
    const char InfraredPath[] = "/sys/class/misc/ap3216c/ir";      // 红外强度
    const int PollIntervalMs = 500;
}

Ap3216cSensor::Ap3216cSensor(QObject *parent)
    : QObject(parent),
      pollingTimer(new QTimer(this))
{
    pollingTimer->setInterval(PollIntervalMs);
    connect(pollingTimer, &QTimer::timeout,
            this, &Ap3216cSensor::pollSensorData);
}

/*
 * 使用 POSIX open/read/close 从 sysfs 文件读取最多 31 字节文本。
 * sysfs 文件通常只包含一个数字+换行符，32 字节缓冲区足够。
 * 桌面环境下设备节点不存在会返回 "N/A"。
 */
QString Ap3216cSensor::readTextValue(const char *devicePath)
{
    const int descriptor = open(devicePath, O_RDONLY);
    if (descriptor < 0)
    {
        qWarning() << "Unable to open" << devicePath << ':' << std::strerror(errno);
        return QStringLiteral("N/A");
    }

    char buffer[32] = {};
    const ssize_t bytesRead = read(descriptor, buffer, sizeof(buffer) - 1);
    const int savedErrno = errno;
    close(descriptor);

    if (bytesRead < 0)
    {
        qWarning() << "Unable to read" << devicePath << ':'
                   << std::strerror(savedErrno);
        return QStringLiteral("N/A");
    }

    return QString::fromLocal8Bit(buffer, int(bytesRead)).trimmed();
}

/*
 * 执行一次完整的传感器轮询——依次读取三个 sysfs 节点。
 * 读取完成后发射 sensorDataChanged() 信号，
 * 由 CameraStreamingPage 响应，更新 UI 并做阈值判断。
 */
void Ap3216cSensor::pollSensorData()
{
    ambientLightValue = readTextValue(AmbientLightPath);
    proximityValue = readTextValue(ProximityPath);
    infraredValue = readTextValue(InfraredPath);
    emit sensorDataChanged();
}

QString Ap3216cSensor::ambientLight() const
{
    return ambientLightValue;
}

QString Ap3216cSensor::proximity() const
{
    return proximityValue;
}

QString Ap3216cSensor::infrared() const
{
    return infraredValue;
}

void Ap3216cSensor::setPollingEnabled(bool enabled)
{
    if (enabled)
    {
        pollSensorData();      // 启动时立即读取一次
        pollingTimer->start(); // 然后每 500ms 自动轮询
    }
    else
        pollingTimer->stop();
}