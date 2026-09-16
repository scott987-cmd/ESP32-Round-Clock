# 项目展示与原理教程

项目提供一份可直接在线学习的中文 HTML 教程页，不需要下载仓库、不需要安装 ESP-IDF，也不需要单独部署服务端。内容包括：

- 6 节在线学习路线和浏览器本地进度；
- 真实圆屏界面截图与成果展示；
- 设备、服务端和可选 macOS 助手的整体架构；
- ESP32-S3、Flash、PSRAM、屏幕、触摸与音频硬件原理；
- LVGL 渲染、手势、后台任务与动态壁纸的软件原理；
- 编译、ROM 下载模式、USB 传输、分区与启动的烧录原理；
- mTLS、Bearer Token、nginx 与物理设备风险的安全边界；
- 数据流切换、DMA 内存小游戏和开发者小测；
- 增加新应用的推荐流程。

在线学习：[https://scott987-cmd.github.io/ESP32-Round-Clock/](https://scott987-cmd.github.io/ESP32-Round-Clock/)

可选离线查看：

```bash
python3 -m http.server 8765 --directory dist
```

然后打开 `http://127.0.0.1:8765/`。这是备选方式，不是学习前提。页面不依赖外部字体、脚本或图片，可离线阅读。

## 隐私处理

展示页只使用经过筛选的设备帧缓冲截图，不包含真实头像、Wi-Fi 名称、服务器地址、证书、Token、额度数据或 Agent 任务名称。不要把未经检查的 `artifacts/` 调试截图直接复制到公开页面。
