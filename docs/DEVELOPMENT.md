# 二次开发指南

## 环境

- ESP-IDF 5.5.x（目标 `esp32s3`）
- CMake/Ninja 与 IDF 自带 Python 环境
- Python 3.11+（服务端测试）
- macOS 13+ 与 Swift 6（仅 macOS 助手）

克隆后先复制 `main/clock_secrets.example.h`，准备 `main/certs/`，再运行 `idf.py build`。IDF Component Manager 会下载 `managed_components/`；仓库中的微雪 BSP 是经过项目验证的本地组件。

## 增加一个应用

1. 在独立的 `.c/.h` 文件中实现视图创建、销毁和后台状态接口。
2. 把源文件加入 `main/CMakeLists.txt`。
3. 在 `main/main.c` 的 `app_names` 和打开应用的分派逻辑中追加条目。
4. 在 `launcher.c::icon_draw` 中为新索引增加图标；不要让图标对象抢占启动器的统一拖动手势。
5. 返回桌面时释放页面对象、停止只属于页面的定时器；持久后台任务只更新线程安全状态和通知中心。
6. 增加至少一个纯逻辑测试；需要真机自动化时临时启用 USB test bridge，验收后关闭并重新构建发布固件。

全屏圆形页面按 466×466 设计。重要操作避开最外侧不可点击区域；文字不要靠拼缩小字号塞进一页，应使用滚动容器或分层详情页。中文必须使用项目字体，英文数字可用 Montserrat。

## 网络功能

简单 JSON 接口使用：

```c
size_t length = 0;
esp_err_t result = device_api("/v1/example", NULL, buffer, capacity, &length);
```

`device_api` 自动使用 mTLS、Bearer Token、超时和禁止重定向。大文件或流式音频可参考 `music_input.c`，但必须继续复用 `CLOCK_API_BASE` 与同一鉴权材料。不要在源文件中写真实 IP、Token 或 Wi-Fi 密码。

服务端新增端点时需要同时完成：

- 严格的 Content-Type、Content-Length、字段类型和标识符校验；
- 有界读取、超时、并发与持久化策略；
- `server/esp32-device.nginx.conf` 中的精确 location 和请求体上限；
- 未授权、越界、路径穿越和中断恢复测试；
- 设备端失败/重试/后台切换状态。

## 素材与字体

- `tools/build_english_assets.py` 生成英语内容。
- `tools/build_content_assets.py` 打包离线分区。
- `tools/build_fonts.cjs` 生成裁剪后的中文字体。
- 生成物必须有稳定清单和校验；避免把原始隐私照片或 API 响应放入仓库。

内容分区现在为 8 MB；应用分区也是 8 MB。增加十几个小型 UI 应用通常不会先耗尽 Flash，真正容易成为瓶颈的是同时存活的 LVGL 对象、内部 DMA RAM、任务栈和未释放的 HTTP/音频缓冲。

## 验证顺序

```bash
.venv/bin/python -m unittest discover -s server -p 'test_*.py'
.venv/bin/python tools/test_content_pack.py
.venv/bin/python tools/test_english_assets.py
.venv/bin/python tools/test_notification_store.py
swift test -c release --package-path macos/RoundTextBridge
. "$IDF_PATH/export.sh" && idf.py build
```

真机发布检查：

1. 从时钟进入桌面并遍历全部应用。
2. 验证左右滑动、上滑总览、实体键返回和长按息屏。
3. 验证重启后 Wi-Fi、语言、壁纸、头像和作品仍存在。
4. 服务端确认无证书失败、证书无 Token 为 401、双重凭据为 200。
5. 发布固件确认 `CONFIG_ROUND_CLOCK_USB_TEST_BRIDGE` 未设置。

## 不要做的事

- 不要为了“省事”关闭 TLS 校验、接受任意重定向或把服务端 Key 放入固件。
- 不要在 LVGL 锁外修改 UI。
- 不要把整屏双缓冲强行放入内部 RAM；先测量驱动的 DMA 要求和最大连续块。
- 不要在没有幂等请求 ID 的情况下自动重试付费生成。
- 不要把 `sdkconfig`、证书、运行数据、构建目录或真机截图提交到 Git。

## 宠物对话与重置解读

宠物使用 `voice_input` 采集设备麦克风，经现有 `/v1/transcribe` 识别后，由
`pet_dialogue` 请求 `/v1/pet` 的异步任务。服务端生成短句与 16 kHz 单声道
PCM；设备验证长度和 SHA-256，回到宠物页面才播放。点击回复可重听。
等待期间可切换应用；此轮对话不保证设备断电后恢复。服务端不保存原始识别文本，
只保存请求哈希、回复和音频；启动或新建对话时清理超过两天的记录，每日上限 100 次。

新增任务不要阻塞 LVGL，也不要从网络回调直接修改控件。音频使用 `audio_bus`
仲裁；PCM 在播放器结束前不得释放或改写。生成接口用随机请求 ID 去重，
网络超时不自动重新提交付费请求。

重置雷达中，来源采集时间最多允许滞后 30 分钟，预告消息本身最多保留 24 小时；
不能把两者混用。`reset_insights` 对最近公开消息做缓存语义分类，
`reset_projection` 保留时间、来源概率和非官方提示。模型不能生成概率，
也不能凭自己的判断把预告升级成“已经重置”。设备分别保存预告与确认消息的游标。

定向回归（USB 调试入口仅在开发固件中启用）：

```bash
cc -Wall -Wextra -Werror -I main tools/test_reset_notice_state.c -o /tmp/test-reset-notices
/tmp/test-reset-notices
.venv/bin/python tools/test_pet_dialogue_device.py
.venv/bin/python tools/test_pet_dialogue_device.py --acoustic
.venv/bin/python tools/test_story_entry_device.py
.venv/bin/python tools/test_reset_device.py
```

`--acoustic` 会由 Mac 播放固定中文测试句，由圆屏麦克风收音。它不修改网络或
系统音量；静音、耳机输出或设备太远时应报告条件不满足，不能把文本注入测试当作麦克风测试。
故事回归使用已有缓存作品，不创建新故事；重置回归只读取真实来源，不伪造线上预警。

桌面性能排查先测量再改动。LVGL 软件绘制中，连续改变圆形图标大小会使模糊阴影
反复计算；本项目保留渐变与轮廓，避免在滑动卡片上使用实时模糊阴影。
`device_ui_benchmark.py` 的数字是渲染加提交刷新耗时，不等于面板刷新率。
参考：[LVGL 软件阴影缓存](https://lvgl.io/docs/open/9.5/API/lv_conf_h)、
[MiniMax 同步语音合成](https://platform.minimax.io/docs/api-reference/speech-t2a-http)。
