# Security Policy

## Supported version

安全修复只保证合入默认分支的最新版本。部署者应同时更新固件、nginx 配置和 Python 服务端。

## Threat model

项目重点防护：公网未授权访问、被盗 Token 的单因素滥用、无界请求导致的资源耗尽、路径穿越、重复付费生成、附近攻击者静默配对、敏感材料进入公开仓库，以及服务进程越权访问主机。

默认控制包括 mTLS + Bearer Token、回环后端、严格请求大小/类型、超时和速率限制、systemd 沙箱、随机 BLE 配对码、关闭发布版 USB 测试桥，以及 CI 公开树扫描。

## 明确的剩余风险

- 开发默认配置没有启用 ESP32-S3 Secure Boot v2 和 Flash Encryption。物理控制设备的人可能读取或替换固件，并取得嵌入的客户端私钥。
- Wi-Fi SSID/密码和设备 Token 存在固件或 NVS 中；因此它们不能替代物理安全。
- MiniMax、天气、Codex Reset 等第三方服务的可用性与返回质量不由本项目保证；服务端会校验边界，但不能保证第三方永不宕机。
- Python 内置 HTTP 服务仅设计为 nginx 后面的单设备/家庭负载，不是通用多租户网关。

量产时应为每台设备签发独立证书，并按 Espressif 官方流程评估 Secure Boot v2、Flash Encryption Release 模式、eFuse 烧录、签名 OTA 和灾难恢复。eFuse 操作不可逆，并可能改变 USB/JTAG 调试与后续烧录能力；本仓库不会在普通开发流程中自动启用。

官方参考：[ESP32-S3 Secure Boot v2](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/security/secure-boot-v2.html)、[ESP32-S3 Flash Encryption](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/security/flash-encryption.html)。

## 报告漏洞

不要在公开 Issue 中附加密钥、证书、真实 IP、Wi-Fi 信息或可直接利用的细节。请通过 GitHub 仓库的 Private vulnerability reporting 提交：

- 受影响版本与模块；
- 最小复现步骤；
- 攻击前提和影响；
- 建议修复（如有）。

维护者确认后会协调修复、回归和披露时间。请勿在未给出合理修复窗口前扫描或攻击他人的部署。
