HEADERS += \
    $$PWD/monitoring_event.h \
    $$PWD/event_store.h \
    $$PWD/media_archive_server.h \
    $$PWD/vision_event_detector.h

SOURCES += \
    $$PWD/event_store.cpp \
    $$PWD/media_archive_server.cpp \
    $$PWD/vision_event_detector.cpp

INCLUDEPATH += $$PWD

DISTFILES += \
    $$PWD/../opencv_src/face-dnn/deploy.prototxt \
    $$PWD/../opencv_src/face-dnn/res10_300x300_ssd_iter_140000.caffemodel \
    $$PWD/../opencv_src/face-dnn/README.md
