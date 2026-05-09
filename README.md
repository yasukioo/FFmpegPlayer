# FFmpegPlayer

一个基于 Qt 6 + FFmpeg 的 RTSP/QML 播放组件库。

## 简介

`FFmpegPlayer` 提供一个可直接在 QML 中使用的显示组件，用于拉取 RTSP 视频流并渲染当前帧。适合快速集成摄像头预览、视频墙、监控画面。

## 特性

- 支持 Qt 6 QML 插件形式集成
- 基于 FFmpeg 解码 RTSP 视频流
- 自动重连
- 支持常见低延迟参数配置
- 提供 `url`、`rtspTransport`、`timeoutUs` 等可调属性

## 依赖

- Qt 6 Quick
- FFmpeg

## 目录

- `src/FFmpegPlayer/`：核心库源码

## QML 用法
ssss
```qml
import FFmpegPlayer 1.0

FFmpegPlayer {
    width: 1280
    height: 720
    url: "rtsp://127.0.0.1:8554/live"
    rtspTransport: "tcp"
}
```

## 构建

```bash
cmake -S . -B build
cmake --build build
```
