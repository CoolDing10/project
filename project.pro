QT += core gui network multimedia multimediawidgets charts sql

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

CONFIG += c++11

# FFmpeg H.264 编码。桌面通常由 pkg-config 提供；交叉编译时可通过
# qmake "FFMPEG_ROOT=/path/to/target/ffmpeg" 指定目标端开发包。
isEmpty(FFMPEG_ROOT) {
    CONFIG += link_pkgconfig
    PKGCONFIG += libavcodec libavutil libswscale
} else {
    INCLUDEPATH += $$FFMPEG_ROOT/include
    LIBS += -L$$FFMPEG_ROOT/lib -lavcodec -lavutil -lswscale
    QMAKE_RPATHDIR += $$FFMPEG_ROOT/lib
}

DEFINES += QT_DEPRECATED_WARNINGS


# ============================================================
# 主程序
# ============================================================

SOURCES += \
    main.cpp \
    mainwidget.cpp

HEADERS += \
    mainwidget.h

# ============================================================
# 打印当前 Qt Kit 的目标架构
#
# Desktop Qt 5.12.9:
#     QT_ARCH 应该为 x86_64
#
# ATK-I.MX6U:
#     QT_ARCH 后面应该为 arm
# ============================================================

message("============================================")
message("QT_ARCH = $$QT_ARCH")
message("PWD     = $$PWD")
message("============================================")


# ============================================================
# OpenCV 3.4.1 公共链接库
#
# 当前图像分析只使用图片读写和基础图像处理。
# ============================================================

OPENCV_LIBS = \
    -lopencv_imgcodecs \
    -lopencv_imgproc \
    -lopencv_core


# ============================================================
# Desktop Qt 5.12.9 GCC 64bit
# ============================================================

contains(QT_ARCH, x86_64) {

    message("Build target: Desktop x86_64")

    # --------------------------------------------------------
    # Desktop OpenCV 3.4.1
    # --------------------------------------------------------

    OPENCV_ROOT = /home/dwm/linux/opencv-3.4.1/x86_opencv

    INCLUDEPATH += $$OPENCV_ROOT/include

    LIBS += -L$$OPENCV_ROOT/lib
    LIBS += $$OPENCV_LIBS


    # --------------------------------------------------------
    # Desktop QtMqtt 5.12.9
    #
    # video_monitor.cpp:
    #     #include <mqtt/qmqttclient.h>
    #
    # 因此 INCLUDEPATH 必须到 src 这一层。
    # --------------------------------------------------------

    MQTT_ROOT = $$PWD/lib/x86_qtmqtt

    INCLUDEPATH += $$MQTT_ROOT/src

    DEPENDPATH += $$MQTT_ROOT/src

    LIBS += $$MQTT_ROOT/lib/libQt5Mqtt.so.5.12.9


    # --------------------------------------------------------
    # Desktop运行时动态库搜索路径
    #
    # 防止程序运行时出现：
    #
    # libQt5Mqtt.so.5 => not found
    #
    # 或 OpenCV 动态库找不到。
    # --------------------------------------------------------

    QMAKE_RPATHDIR += $$OPENCV_ROOT/lib
    QMAKE_RPATHDIR += $$MQTT_ROOT/lib
}


# ============================================================
# ATK-I.MX6U / ARM
# ============================================================

contains(QT_ARCH, arm) {

    message("Build target: ATK-I.MX6U ARM")

    # --------------------------------------------------------
    # ARM OpenCV 3.4.1
    # --------------------------------------------------------

    OPENCV_ROOT = $$PWD/lib/open_lib/opencv_3.4.1/arm_opencv

    INCLUDEPATH += $$OPENCV_ROOT/include

    # ARM 包中只有 libopencv_*.so.3.4.1，没有供链接器使用的
    # libopencv_*.so 开发链接名。若继续使用 -lopencv_xxx，链接器会
    # 错误选中 SDK sysroot 中的 OpenCV 3.1，造成 3.4.1 头文件与
    # 3.1 运行库 ABI 混用。这里显式指定项目随附的 3.4.1 完整文件。
    LIBS += \
        $$OPENCV_ROOT/lib/libopencv_imgcodecs.so.3.4.1 \
        $$OPENCV_ROOT/lib/libopencv_imgproc.so.3.4.1 \
        $$OPENCV_ROOT/lib/libopencv_core.so.3.4.1


    # --------------------------------------------------------
    # ARM QtMqtt 5.12.9
    #
    # ARM目录没有 libQt5Mqtt.so，
    # 只有 libQt5Mqtt.so.5.12.9，
    # 所以这里直接指定完整文件。
    # --------------------------------------------------------

    MQTT_ROOT = $$PWD/lib/arm_qtmqtt

    INCLUDEPATH += $$MQTT_ROOT/src

    DEPENDPATH += $$MQTT_ROOT/src

    LIBS += $$MQTT_ROOT/lib/libQt5Mqtt.so.5.12.9

    # 开发板固定部署布局：
    #   /opt/project/bin/project
    #   /opt/project/lib/libopencv_*.so.3.4
    # 把运行库目录写入 ELF RUNPATH，部署完整后可直接 ./project，
    # 不需要每次手动设置 LD_LIBRARY_PATH。
    QMAKE_RPATHDIR += /opt/project/lib
}


# ============================================================
# Qt Resource
# ============================================================

RESOURCES += \
    res.qrc


# ============================================================
# 子模块
# ============================================================

include(./form/form.pri)
include(./hardware/hardware.pri)
include(./edge/edge.pri)


# form/ 和 hardware/ 头文件搜索路径
INCLUDEPATH += $$PWD/form
INCLUDEPATH += $$PWD/hardware
INCLUDEPATH += $$PWD/edge


# ============================================================
# 默认部署规则
# ============================================================

qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin

!isEmpty(target.path): INSTALLS += target
