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
#include "receiver_window.h" // 引入 PC 接收端主窗口类声明。

#include <QApplication> // 管理 Qt Widgets 应用生命周期和主事件循环。

int main(int argc, char *argv[]) // argc 是参数数量，argv 保存程序名和命令行参数字符串。
{
    QApplication application(argc, argv); // 创建 GUI 应用对象并解析 Qt 支持的命令行参数。
    ReceiverWindow window;                // 在栈上创建接收端主窗口，main 结束时自动析构。
    window.show();                        // 请求窗口显示；实际绘制由事件循环完成。
    return application.exec();            // 启动事件循环，并把退出码返回给操作系统。
}
