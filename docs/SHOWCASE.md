# 项目展示与原理教程

项目提供一份可直接在浏览器打开的中文 HTML 展示页，内容包括：

- 真实圆屏界面截图与成果展示；
- 设备、服务端和可选 macOS 助手的整体架构；
- ESP32-S3、Flash、PSRAM、屏幕、触摸与音频硬件原理；
- LVGL 渲染、手势、后台任务与动态壁纸的软件原理；
- 编译、ROM 下载模式、USB 传输、分区与启动的烧录原理；
- mTLS、Bearer Token、nginx 与物理设备风险的安全边界；
- 增加新应用的推荐流程和入门学习路线。

在线浏览：[https://scott987-cmd.github.io/ESP32-Round-Clock/](https://scott987-cmd.github.io/ESP32-Round-Clock/)

本地查看：

```bash
python3 -m http.server 8765 --directory dist
```

然后打开 `http://127.0.0.1:8765/`。页面不依赖外部字体、脚本或图片，可离线阅读。

## 隐私处理

展示页只使用经过筛选的设备帧缓冲截图，不包含真实头像、Wi-Fi 名称、服务器地址、证书、Token、额度数据或 Agent 任务名称。不要把未经检查的 `artifacts/` 调试截图直接复制到公开页面。
