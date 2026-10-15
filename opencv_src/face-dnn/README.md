# 轻量人脸检测模型

本目录用于边缘监控页的 OpenCV DNN 人脸检测后端：

- `deploy.prototxt`：OpenCV `samples/dnn/face_detector` 的 Caffe 网络定义。
- `res10_300x300_ssd_iter_140000.caffemodel`：OpenCV 3rdparty 的 ResNet-10 SSD 权重。

项目固定使用 OpenCV 3.4.1。该版本的 Caffe protobuf 不包含较新的
`PriorBox/DetectionOutput clip` 字段，因此 `deploy.prototxt` 删除了这些
默认值为 false 的字段。桌面 OpenCV 3.4.1 已完成一次真实 `forward()`
冒烟测试。

运行时若模型缺失或推理异常，程序会记录警告并自动回退到
`opencv_src/face-haar/haarcascade_frontalface_alt2.xml`。

模型来源：

- https://github.com/opencv/opencv/tree/3.4.1/samples/dnn/face_detector
- https://github.com/opencv/opencv_3rdparty/tree/dnn_samples_face_detector_20170830
