# Round Clock OS

面向微雪 ESP32-S3-Touch-AMOLED-1.75C 的开源圆屏应用系统。它不是只能显示时间的单一固件，而是一个可滑动、可返回桌面、可继续增加应用的中文微型系统：设备端负责触摸交互、显示、录音与本地内容，互联网服务端负责天气、语音识别、MiniMax 生成和跨设备数据同步。

想先看真实界面、整体架构，或从零理解硬件、软件与烧录原理，可直接打开 [GitHub Pages 交互式项目展示与原理教程](https://scott987-cmd.github.io/ESP32-Round-Clock/)；页面源码位于 [dist/index.html](dist/index.html)，使用说明见 [docs/SHOWCASE.md](docs/SHOWCASE.md)。

## 已有能力

- 圆形启动器：左右滑动切换，上滑查看全部应用，点击进入，实体键返回；长按实体键息屏。
- 18 个应用：时钟、天气、互动头像、联网语音、设置、用量、重置预警、音乐创作、AI 电台、动态壁纸、Agent 工作台、随手记、点点小星星、看图英语、作品相册、电子宠物、互动故事屋、记忆翻牌。
- 中文 UI、触摸手势、动画星空/极光、可替换壁纸、后台生成状态与持久化作品库。
- 板载麦克风录音；服务端 SenseVoice 优先、Whisper 后备；看图英语支持自动点读。
- BLE HID 键盘模式，采用 Secure Connections + MITM 和每次配对随机六位码。
- 可选 macOS 头像上传与 LLMQuota/Agent 状态同步助手。

设备：ESP32-S3、466×466 圆形 AMOLED、CST9217 触摸、ES7210 音频输入、32 MB Flash、8 MB PSRAM。固件基于 ESP-IDF 5.5.5 与 LVGL 9.5.0。

## 架构

```text
圆屏固件  ── HTTPS:8080 / mTLS + Bearer Token ── nginx
   │                                                │
   ├─ 本地 UI、触摸、音频、SPIFFS、NVS             └─ Python loopback 服务
   ├─ BLE HID                                           ├─ 天气 / Codex Reset
   └─ Wi-Fi 快速配置                                    ├─ MiniMax 图片/音乐/摘要
                                                     ├─ SenseVoice / Whisper
macOS 可选助手 ── 同一设备证书与 Token ───────────────└─ 头像、额度、Agent、作品库
```

更详细的模块边界、数据流和存储说明见 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)。

## 快速开始

### 1. 准备固件配置

安装 ESP-IDF 5.5.x，复制配置模板：

```bash
cp main/clock_secrets.example.h main/clock_secrets.h
mkdir -p main/certs
```

在 `main/clock_secrets.h` 中设置 2.4 GHz Wi-Fi、服务端 HTTPS 地址和至少 32 字符的随机设备令牌。把设备 CA、客户端证书和客户端私钥分别放到：

```text
main/certs/device-ca.pem
main/certs/device-client.pem
main/certs/device-client.key
```

这些文件已被 `.gitignore` 排除，绝不能提交。证书签发与生产部署见 [docs/DEPLOYMENT.md](docs/DEPLOYMENT.md)。

### 2. 编译和烧录

```bash
. "$IDF_PATH/export.sh"
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbmodemXXXX flash
```

烧录不会要求修改 Mac 的 Wi-Fi。设备存储布局为 8 MB 应用、8 MB 壁纸/用户数据、8 MB只读内容包；当前应用镜像约占应用分区的 63%，仍有约 37% 空间供二次开发，但 RAM、任务栈和素材体积仍需逐项测量。

### 3. 启动服务端

服务端只监听 `127.0.0.1:18080`，公网的 8080 端口必须由 nginx 终止 TLS 并验证设备客户端证书：

```bash
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
WALLPAPER_STATE_DIR=/tmp/round-clock-state \
WALLPAPER_DEFAULT_IMAGE="$PWD/wallpapers/default.png" \
WALLPAPER_TOKEN='replace-with-at-least-32-random-characters' \
.venv/bin/python server/wallpaper_service.py install-default
```

生产环境使用仓库内的 systemd、nginx 和环境变量模板。不要把 Python 服务直接绑定到公网地址。完整步骤见 [docs/DEPLOYMENT.md](docs/DEPLOYMENT.md)。

## 开发与测试

```bash
.venv/bin/python -m unittest discover -s server -p 'test_*.py'
swift test -c release --package-path macos/RoundTextBridge
.venv/bin/python tools/test_content_pack.py
.venv/bin/python tools/check_public_tree.py   # 初始化 Git 并暂存文件后运行
```

发布固件默认关闭 USB 截屏/指针注入测试桥，但不影响 USB 烧录。开发时可用 `idf.py menuconfig` 打开 `Round Clock options → Enable USB test and screenshot bridge`，完成验收后必须重新关闭。

如何增加第 19 个应用、生成中文字体、扩充离线内容、避免 DMA/内部 RAM 问题，见 [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md)。

## 安全边界

- 外网 API 同时要求设备 CA 签发的客户端证书和随机 Bearer Token。
- nginx 有请求体上限、连接限制、速率限制和超时；后端拒绝公网绑定。
- 服务器密钥和 MiniMax Key 只保存在服务端；仓库只包含模板。
- Wi-Fi 快速配置使用临时随机 WPA2 热点和短时会话；不会操作开发电脑的 Wi-Fi。
- 物理拿到未启用 Flash Encryption 的开发板仍可能提取固件内证书。这是开发模式下明确保留的风险；生产设备的不可逆加固流程见 [SECURITY.md](SECURITY.md)。

没有任何系统能被诚实地承诺“绝对无漏洞”。本项目的目标是：公开威胁模型、默认安全、可重复验证，并及时修复负责任披露的问题。

## 文档

- [架构说明](docs/ARCHITECTURE.md)
- [二次开发指南](docs/DEVELOPMENT.md)
- [服务端部署与证书](docs/DEPLOYMENT.md)
- [在线交互式项目展示与原理教程](https://scott987-cmd.github.io/ESP32-Round-Clock/)
- [展示页 HTML 源码](dist/index.html)
- [展示页使用与隐私说明](docs/SHOWCASE.md)
- [安全策略与威胁模型](SECURITY.md)
- [贡献指南](CONTRIBUTING.md)

## 许可证

项目代码使用 [Apache License 2.0](LICENSE)。`components/waveshare__esp32_s3_touch_amoled_1_75c` 保留其上游许可证；测试图片的来源与许可记录在对应 `Fixtures/README.md` 中。
