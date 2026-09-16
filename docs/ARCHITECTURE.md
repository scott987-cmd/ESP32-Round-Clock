# 架构说明

## 设计目标

Round Clock OS 把资源受限的圆屏终端与可扩展的互联网服务分开：即时触摸反馈、时钟、游戏和已下载素材留在设备；需要模型、公开数据或大存储的能力放在服务端。增加应用时不应绕过公共网络层、音频总线或启动器。

## 固件

`main/main.c` 负责系统启动、屏幕、电源、Wi-Fi、时间同步、全局手势与主要视图。功能较完整的应用被拆在独立模块：

| 模块 | 责任 |
| --- | --- |
| `launcher.c` | 单应用轮播、相邻应用预览、全部应用页 |
| `wifi_setup.c` | 临时 WPA2 配网热点、会话校验、NVS 凭据 |
| `voice_input.c` / `audio_bus.c` | 板载麦克风录音和独占音频资源 |
| `device_api.c` | 通用小型 JSON API 客户端 |
| `avatar_store.c` / `avatar_face.c` | 头像同步、双副本提交、互动动画 |
| `music_input.c` / `wallpaper_input.c` | 后台生成任务、播放或安装结果 |
| `companion_apps.c` | Agent 工作台与随手记 |
| `library_app.c` | 服务端作品库浏览、收藏、应用和回收站 |
| `kids_apps.c` / `story_app.c` / `english_app.c` / `star_game.c` | 儿童应用与离线内容 |
| `ble_remote.c` | 有限时配对窗口的 BLE HID 键盘 |
| `notification_center.c` | 后台任务完成/失败通知 |

所有 HTTPS 请求使用 `service_config.h`，实际地址与 Token 来自被忽略的 `clock_secrets.h`。CA、设备证书和设备私钥在构建时嵌入固件。

### 存储

- NVS：Wi-Fi、语言、音量、头像元数据等小型设置。
- `wallpaper` SPIFFS：当前壁纸、头像双副本和少量终端缓存。
- `content` 自定义分区：英语图片/音频和内置故事音频，由 `tools/build_content_assets.py` 生成。
- 服务端状态目录：作品、故事、笔记、配额快照和生成结果；写入使用临时文件 + `os.replace`。

### 内存与 DMA

大图片、JSON 响应、音频和头像缓冲优先分配到 PSRAM。显示驱动的 DMA 队列和内部 RAM 保留由 `sdkconfig.defaults` 与 BSP 控制，不应通过盲目增加内部缓冲解决卡顿。新增应用需要观察内部堆最大连续块，而不仅是总剩余内存；后台任务必须在退出页面后安全完成，且不能直接操作已销毁的 LVGL 对象。

## 服务端

公网入口是 nginx 8080：

1. 验证 TLS 服务器证书和设备客户端证书（mTLS）。
2. 限制连接数、请求速率、请求体大小和超时。
3. 只代理明确列出的 `/v1/*` 路径到 `127.0.0.1:18080`。

`wallpaper_service.py` 使用有界线程 HTTP 服务，强制至少 32 字符的 Bearer Token，并拒绝非回环监听。MiniMax、天气和 Codex Reset 请求只允许 HTTPS，模型响应大小有上限。服务端模块不会把 MiniMax Key 发到设备。

主要 API：

| 端点 | 用途 |
| --- | --- |
| `/v1/weather` | 缓存后的天气 |
| `/v1/transcribe` | 16 kHz PCM 语音识别 |
| `/v1/wallpaper`, `/v1/wallpaper/generate` | 壁纸读取与生成 |
| `/v1/music`, `/v1/music/latest` | 音乐生成与播放数据 |
| `/v1/avatars` | 头像清单、读取与上传 |
| `/v1/quota`, `/v1/agents` | 外部助手上传的只读投影 |
| `/v1/notes` | 语音笔记与服务端摘要 |
| `/v1/library` | 作品元数据与二进制内容 |
| `/v1/stories` | 有日限额、可恢复的故事任务 |
| `/v1/codex-reset` | 外部信号的保守投影 |

## macOS 助手

`macos/RoundTextBridge` 是 Swift Package。当前保留头像裁剪/人脸关键点、BLE 数据包和本地识别组件；头像上传通过系统 `curl` 使用客户端证书和权限为 0600 的 Header 文件，Token 不出现在进程命令行。

`macos/QuotaSync/quota_sync.py` 只上传 LLMQuota 已生成的展示投影以及任务板白名单字段，不重新计算额度，也不上传提示词、凭据或任意本地路径。

## 扩展原则

- 新应用注册到统一启动器并提供可销毁的独立视图。
- 网络请求只经过 `device_api` 或现有专用客户端，不新建明文 HTTP 通道。
- 服务端先校验长度、类型、标识符，再做文件或模型操作。
- 付费模型任务使用请求 ID、状态持久化和并发锁，避免重试造成重复扣费。
- 二进制协议必须包含版本/长度/校验和；写入采用临时文件和原子替换。
