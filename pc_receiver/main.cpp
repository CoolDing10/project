/*
 * ============================================================================
 * pc_receiver/main.cpp — PC 端接收程序入口
 * ============================================================================
 *
 * 这是一个独立的 Qt Widgets 应用（独立于主项目的 .pro 文件编译）。
 * 运行在 Linux 桌面环境（x86_64），功能：
 *   - 同端口监听设备端 H.264/TCP 与 H.264/RTP/UDP 视频流
 *   - 按自定义 RTP 头重组、FFmpeg 解码并显示视频
 *   - MQTT 连接接收遥测、发送远程控制命令
 *
 * 构建方式（在 pc_receiver 目录下）：
 *   qmake pc_receiver.pro && make
 */
#include "receiver_window.h"

#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication application(argc, argv);
    ReceiverWindow window;
    window.show();
    return application.exec();
}
