# 服务端部署

以下示例假设公网只开放 nginx 的 TCP 8080，Python 后端固定监听 `127.0.0.1:18080`。SSH 应限制来源并在部署完成后按运维策略关闭公网入口。

## 1. 签发独立证书

为每台设备签发独立客户端证书；不要在多台设备之间复制同一个私钥。服务器证书的 SAN 必须覆盖 `CLOCK_API_BASE` 中的域名或 IP。

设备构建需要：

```text
main/certs/device-ca.pem
main/certs/device-client.pem
main/certs/device-client.key
```

服务器需要：

```text
/etc/esp32-device-tls/server.pem
/etc/esp32-device-tls/server.key
/etc/esp32-device-tls/device-ca.pem
```

目录设为 `0700`，私钥设为 `0600`。CA 私钥应离线保存，不要放到应用服务器或仓库。

## 2. 安装应用

```bash
sudo useradd --system --home /var/lib/esp32-wallpaper --shell /usr/sbin/nologin espclock
sudo install -d -o root -g root -m 0755 /opt/esp32-wallpaper
sudo install -d -o espclock -g espclock -m 0700 /var/lib/esp32-wallpaper
sudo install -o root -g root -m 0644 server/*.py /opt/esp32-wallpaper/
sudo install -o root -g root -m 0644 wallpapers/default.png /opt/esp32-wallpaper/default.png
sudo install -o root -g root -m 0644 server/esp32-wallpaper.service /etc/systemd/system/
```

安装 Pillow 与 OpenCC。可以使用发行版包，也可以创建只供 `espclock` 服务使用的固定版本虚拟环境，并相应修改 `ExecStart`。

复制 `server/esp32-wallpaper.env.example` 到 `/etc/esp32-wallpaper.env`，填入 MiniMax Key 与随机 Token，权限必须为 `0600`。生产环境保持：

```text
REQUIRE_WALLPAPER_TOKEN=1
```

同一个 Token 写入被忽略的 `main/clock_secrets.h`。已有主机可使用 `tools/provision_device_token.py` 原子轮换本地、macOS 助手与远程环境文件；脚本不会打印 Token，也不会把它放进进程参数。

## 3. nginx

复制 `server/esp32-device.nginx.conf` 到站点配置，替换示例域名但保留：

- `ssl_verify_client on`
- TLS 1.2/1.3
- 精确路径代理和各接口请求体上限
- 连接/请求速率限制与超时
- 仅代理到 `127.0.0.1:18080`

相关 nginx 官方说明：[客户端证书验证](https://nginx.org/en/docs/http/ngx_http_ssl_module.html#ssl_verify_client)、[请求速率限制](https://nginx.org/en/docs/http/ngx_http_limit_req_module.html)、[连接限制](https://nginx.org/en/docs/http/ngx_http_limit_conn_module.html)。

```bash
sudo nginx -t
sudo systemctl daemon-reload
sudo systemctl enable --now esp32-wallpaper
sudo systemctl reload nginx
```

## 4. 验证

依次验证三种情况，使用受保护的临时 Header 文件，避免 Token 出现在 shell 历史和 `ps`：

```bash
# 无客户端证书：TLS/HTTP 拒绝
curl --cacert device-ca.pem https://device-api.example.com:8080/v1/weather

# 有证书、无 Token：401
curl --cacert device-ca.pem --cert device-client.pem --key device-client.key \
  https://device-api.example.com:8080/v1/weather

# 两种凭据都有：200
curl --cacert device-ca.pem --cert device-client.pem --key device-client.key \
  --header @authorization-header \
  https://device-api.example.com:8080/v1/weather
```

再检查：

```bash
systemctl is-active esp32-wallpaper nginx
systemd-analyze security esp32-wallpaper.service
journalctl -u esp32-wallpaper --since today
```

## 5. 轮换与撤销

- Token 泄漏：先生成新 Token 并烧录设备，再重启服务端启用新值。
- 单台设备丢失：从 nginx 信任链或吊销策略中撤销该客户端证书；每设备证书使撤销不会影响其他设备。
- 服务端私钥泄漏：更换服务器证书和私钥；若 CA 私钥也泄漏，创建新 CA 并重新签发全部证书。
- MiniMax Key 泄漏：在提供商控制台撤销并更换，只更新服务端环境文件。

不要在日志、截图、问题单或 GitHub Actions Secret 之外粘贴任何真实凭据。
