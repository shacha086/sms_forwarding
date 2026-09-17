# REST API 与 BLE 配网

## 托管管理页

管理页是 [`web/`](../web/README.md) 下的 React + TypeScript + Vite 工程。执行
`npm install` 和 `npm run build`，将生成的 `web/dist` 目录部署到
`https://sms.ctree.site/`。访问设备根路径时，固件会跳转到：

```text
https://sms.ctree.site/#192.168.x.x
```

页面从 hash 解析设备 IP，并请求 `http://设备IP/api/v1/*`。首次访问时，现代浏览器会
请求“本地网络访问”权限，必须选择允许。页面不自动连接，确认顶部账号和密码后点击
“连接”；默认账号密码为 `admin / admin123`，修改过密码的设备需填写新密码。
连接过程中可取消或使用新凭据重新连接，连接失败后不自动重试。

设备 REST API 的 CORS 白名单严格限制为：

```text
https://sms.ctree.site
http://sms.ctree.site
```

允许 `GET, POST, OPTIONS`，允许请求头 `Authorization, Content-Type`，并在预检响应中
返回 `Access-Control-Allow-Private-Network: true`。不要将允许来源改成 `*`，因为管理
接口携带 Basic Auth 凭据。

固件不再提供内嵌 HTML 管理页。所有 HTTP 接口都使用 HTTP Basic Auth，默认账号为
`admin` / `admin123`，响应格式为 JSON。

## REST API

| 方法 | 路径 | 参数 | 用途 |
|---|---|---|---|
| GET | `/api/v1/status` | 无 | 设备、WiFi、BLE、堆内存和模组状态 |
| GET | `/api/v1/config` | 无 | 获取脱敏配置 |
| POST | `/api/v1/config` | form-urlencoded | 更新系统配置 |
| POST | `/api/v1/sms` | `phone`, `content` | 发送短信 |
| POST | `/api/v1/ping` | 与原 `/ping` 相同 | 模组 Ping |
| POST | `/api/v1/wifi` | 见下文 | 切换 WiFi 或开启 BLE 配网 |
| GET | `/api/v1/logs` | 无 | 最近 60 行日志 |

示例：

```bash
curl -u admin:admin123 http://DEVICE_IP/api/v1/status

curl -u admin:admin123 -X POST http://DEVICE_IP/api/v1/wifi \
  -d "action=connect" -d "ssid=YOUR_SSID" -d "password=YOUR_PASSWORD"

curl -u admin:admin123 -X POST http://DEVICE_IP/api/v1/wifi \
  -d "action=enable_ble"
```

旧接口路径暂时保留，以免现有自动化脚本立即失效；它们现在也只返回 JSON。

## BLE 配网

设备开机后开启 BLE。由于同一个 NimBLE Server 还提供 NekokoLPA2 RED BLE v1
读卡服务，WiFi 连接后 BLE 会保持运行；断开连接后自动恢复广播。也可以通过 REST API
的 `enable_ble` 动作重新开启。

- BLE 名称：未连接时每 1.5 秒在 `ESTKme-XXXXXX` 与 `SMS-XXXXXX` 间轮换；
  连接后停止切换（任何名称都不能包含 `ESTKme RED`）
- 配对 PIN：`123456`（可通过 `BLE_PROVISIONING_PASSKEY` 编译宏覆盖）
- Service UUID：`7d2ea28a-f7bd-485a-bd9d-92ad6ecfe93e`

| Characteristic | UUID | 权限 | 内容 |
|---|---|---|---|
| SSID | `7d2ea28b-f7bd-485a-bd9d-92ad6ecfe93e` | 加密写 | WiFi 名称，最长 32 字节 |
| Password | `7d2ea28c-f7bd-485a-bd9d-92ad6ecfe93e` | 加密写 | WiFi 密码，最长 63 字节 |
| Command | `7d2ea28d-f7bd-485a-bd9d-92ad6ecfe93e` | 加密写 | 写入 `connect` 开始切换 |
| Status | `7d2ea28e-f7bd-485a-bd9d-92ad6ecfe93e` | 加密读、Notify | JSON 状态和设备 IP |

可使用 nRF Connect、LightBlue 等通用 BLE 工具测试：依次写入 SSID、Password、
`connect`，然后读取或订阅 Status。凭据会保存到 ESP32 NVS，重启后继续使用。
首次启动或 NVS 中没有 WiFi 凭据时，设备等待 BLE 配网，不使用硬编码的默认网络。
已联网设备仍可通过 REST API 更新凭据。

## NekokoLPA2 RED BLE v1

同一设备还广播短服务 UUID `0x4553`。RED 服务本身不要求加密配对；配网特征仍要求
PIN 和加密连接。完整协议、缓冲区和测试方法见
[`nekokolpa2_red_ble.md`](nekokolpa2_red_ble.md)。
