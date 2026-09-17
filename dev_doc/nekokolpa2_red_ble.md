# NekokoLPA2 RED BLE v1 兼容层

## 1. 平台与选择

- MCU：ESP32-C3 SuperMini
- 框架：Arduino ESP32 Core 3.3.10
- BLE：NimBLE-Arduino 2.3.7，外设模式
- eUICC：连接到 ML307A 的 UICC 接口，ESP32 通过 UART AT 命令访问
- UICC 协议：T=0/T=1、供电、PPS 由 ML307A 管理，ESP32 不直接驱动 ISO 7816
- 现有 GATT：加密 WiFi 配网服务
- 可用 RAM：约 327 KB；RED 层使用三个约 4 KB 的静态缓冲区

采用 RED BLE v1。工程没有 USB CCID，现有 GATT 也不是 Nordic UART；RED v1 只需
长度帧和原始 APDU，改动最小。不能使用名称 `ESTKme RED`，否则 NekokoLPA2 会选择
RED BLE v2。

## 2. 广播与 GATT

未连接时，设备名每 1.5 秒在 `ESTKme-XXXXXX` 和 `SMS-XXXXXX` 之间轮换，后缀来自
ESP32 MAC。前者兼容 NekokoLPA2 的名称识别，后者保留原配网页体验。两套 GATT 服务
始终同时存在，短服务 UUID `0x4553` 始终广播。建立任意连接后停止切名，断线后恢复
轮换和可连接广播。

| 用途 | UUID | 属性 | 单次属性长度 |
|---|---|---|---|
| RED 服务 | `00004553-0000-1000-8000-00805F9B34FB` | Primary Service | — |
| NekokoLPA2 写入 TX | `00006D65-0000-1000-8000-00805F9B34FB` | Write、Write Without Response | 244 |
| 设备通知 RX | `0000544B-0000-1000-8000-00805F9B34FB` | Notify、CCCD | 244 |

配网服务与 RED 服务共用一个 NimBLE Server。配网特征保持加密权限，RED 特征不要求
配对。Web 配网页同时按 `ESTKme-` 名称发现设备。

## 3. 收包状态机和缓冲区

每次 GATT Write 仅追加到 4099 字节流缓冲区，BLE 回调不执行 AT 命令。主循环按下列
状态机取出一条命令：

1. 少于 3 字节：等待后续 Write。
2. 读取 `CMD` 和 `LEN_L | LEN_H << 8`。
3. 长度超过 4096：清空半包；APDU 命令返回 `6F00`，其他命令返回空负载。
4. 当前字节少于 `3 + LEN`：继续等待。
5. 复制负载到 4096 字节命令缓冲区。
6. 从流缓冲区移除这一帧；粘在后面的下一帧保留。
7. 在 Arduino 主循环串行执行命令，一次只处理一条 APDU。

静态缓冲区：

- BLE 字节流：8198 字节，可同时容纳两个最大逻辑帧；
- 当前命令：4096 字节；
- 响应/Notify 帧：4099 字节；
- `AT+CSIM` 文本使用临时 Arduino `String`，最大二进制响应限制 4096 字节。

连接建立、断开或接收溢出时清空半包。断开还清除连接句柄、MTU 和原始 UICC 会话
就绪标志。

## 4. UICC 接口

### Power On

ML307A 控制 UICC 供电、复位、PPS 和 T=0/T=1。`0x03` 执行以下检查与读取：

1. `AT+CSIM=?` 必须返回 `OK`；
2. 查询 `AT+CPIN?` 供诊断；未返回 `READY` 时记录日志，但不阻断配置下载；
3. 执行 ASR 私有命令 `AT*GATR`；
4. 从当前 ML307A 格式 `*GATR:<ATR_HEX>` 解析 ATR；解析不依赖同一缓冲区内的 `OK`；
5. 校验 ATR 以 `3B/3F` 开头，且接口字节、历史字节、可选 TCK 长度和校验和完整；
6. 返回 `03 | ATR长度LE16 | ATR`。

解析器宽松接受 ATR 字节之间的空格和额外换行；另外兼容旧 ASR 文档中的
`*GATR:<data_len>,<ATR_HEX>` 变体，其中长度可按二进制字节或 HEX 字符计数。若当前
ML307A 固件不支持 `AT*GATR` 或返回值校验失败，则返回标准空 ATR 帧 `03 00 00`，不会
伪造 ATR。NekokoLPA2 当前 RED v1 适配器不会校验 ATR 内容，因此降级不会阻塞后续 APDU。

蜂窝网络状态与 eUICC 管理访问独立：`CGACT` 失败或网络注册超时仍保留蜂窝限制模式，
但不跳过启动时的 eSIM 能力检测和 EID 读取，也不拒绝 BLE Power On。
能力检测通过不代表已检测到卡；缺卡、锁卡或访问失败以实际 EID/APDU 操作的结果为准。
此流程不会启用 PDP 数据连接。

### 原始 APDU

使用 ML307A 的 3GPP `AT+CSIM=<hex字符数>,"<APDU HEX>"` 基本通道接口。响应
`+CSIM: <hex字符数>,<RAPDU HEX>` 被还原为二进制并完整返回。兼容层不会：

- 打开或关闭逻辑通道；
- 修改 CLA；
- 自动执行 GET RESPONSE；
- 解释 `61xx`、`6Cxx` 或业务状态字；
- 删除 `SW1 SW2`。

这些工作由 NekokoLPA2 完成。原有网页 eSIM 管理继续使用 `AT+CCHO/CGLA`，不应在
NekokoLPA2 会话期间同时操作网页 eSIM 管理接口。

## 5. 命令和错误

| 请求 | 行为 | 正常响应 |
|---|---|---|
| `0x02` Claim | 校验负载是否为 ASCII `ESTKme` | `02 00 00` |
| `0x03` Power On | 检查 `CSIM` 和 UICC READY | `03 00 00`（空 ATR） |
| `0x04` APDU | 通过 `AT+CSIM` 原样转发 | `04 LEN_LE RAPDU` |

RED v1 没有传输错误字段。非法短 APDU返回 `6700`；AT 超时、UICC 不可用、HEX 解析
错误或响应溢出返回 `6F00`。收到未知命令时回显命令号并返回空负载，保证客户端帧同步。

## 6. Notify 分包和流控

逻辑响应先编码为 `CMD | LEN_LE16 | PAYLOAD`，再按 `MTU - 3` 分成连续 Notify。
本机最大 MTU 设为 247，因此单个 Notify 最大 244 字节。每片发送失败最多重试 10 次，
每次间隔 8 ms；多片之间间隔 4 ms。客户端按 RED 长度头重新拼接。

| 环境 | ATT MTU | Notify 数据上限 | 4099 字节最大帧片数 |
|---|---:|---:|---:|
| 默认/申请失败 | 23 | 20 | 205 |
| 原生客户端申请成功 | 247 | 244 | 17 |
| Web Bluetooth | 通常 23 | 20 | 205 |

TX 端同样是字节流，所以客户端按 20、240 或 244 字节写入都能处理；一次 Write 不需要
对应一条完整 RED 帧。

## 7. 超时和恢复

- `AT+CSIM` 等待上限 14 秒，低于 NekokoLPA2 RED v1 当前 15 秒等待窗口；
- BLE 回调只复制字节，长 AT 操作在主循环执行；
- 一次只处理一条命令，后续粘包保留在流缓冲区；
- 断线立即丢弃半包和待处理传输状态，并要求下次连接重新 Power On；
- `advertiseOnDisconnect(true)` 负责恢复广播；
- ML307A 不提供 card-only reset，断线只能清除 ESP32 会话状态，无法强制冷复位 UICC。

## 8. 测试向量

### Claim

请求：

```text
02 06 00 45 53 54 4B 6D 65
```

预期通知字节流：

```text
02 00 00
```

分别以 1+2+6 字节、每字节一包、完整 9 字节写入，响应应相同。

### Power On

请求：

```text
03 02 00 01 01
```

ML307A 和 UICC 就绪且 `AT*GATR` 返回例如 `3B 00` 时预期：

```text
03 02 00 3B 00
```

不支持 `AT*GATR` 时预期降级响应为 `03 00 00`。

### APDU

以 SELECT MF 为链路冒烟测试，实际 RAPDU 取决于卡：

```text
TX: 04 07 00 00 A4 00 0C 02 3F 00
AT: AT+CSIM=14,"00A4000C023F00"
RX: 04 <RAPDU长度LE16> <完整RAPDU，末尾含SW1 SW2>
```

若 UICC 返回 `9000`，RED 响应为：

```text
04 02 00 90 00
```

### 粘包

一次或连续 Write 输入：

```text
02 06 00 45 53 54 4B 6D 65 03 02 00 01 01
```

应依次产生 Claim 和 Power On 两个逻辑响应，不丢失第二帧。

### 长度保护

输入头 `04 01 10` 声明 4097 字节，应清空半包并返回：

```text
04 02 00 6F 00
```

## 9. NekokoLPA2 联调

1. 编译并烧录固件，串口应显示 `BLE GATT check v1, build=...`，核对构建时间；
   随后应有 `BLE GATT verified: provisioning + RED 4553, address=...` 和
   `BLE 已开启: ESTKme-/SMS-...`。服务和特征的 `handle` 均应非零，否则不会启动广播。
2. 使用 nRF Connect 确认 `0x4553` 服务、可写 `0x6D65` 和可通知 `0x544B`。
3. 订阅 RX，依次发送上述 Claim、Power On、SELECT MF 测试向量。
4. 在 NekokoLPA2 扫描；设备类型应为 `red_ble`，不能是 `red_ble2`。
5. 连接后检查日志中依次出现 Claim、Power On 和 `eSIM CSIM TX/RX`。
6. 先执行 EID/配置列表读取，再进行下载；下载期间不要同时使用网页 eSIM 操作。
7. 分别在 Android/iOS 原生端（目标 MTU 247）和 Web Bluetooth（20 字节写入）测试。
8. 在 APDU 中途断开蓝牙，确认设备恢复广播；重新连接必须再次经过 Power On。

若仍报 `RED BLE service not found`，保留完整的 `BLE GATT` 启动日志，并记录客户端平台、
连接设备地址及 `discoverServices()` 实际返回的全部 UUID。广播中包含 `4553` 不等于服务
注册成功；本地句柄校验通过也不等于客户端已获得相同的服务列表。此错误发生在服务发现
阶段，与后续 eSIM 初始化、Claim、Power On 或 APDU 返回值无关。

## 10. 已知兼容边界

- NekokoLPA2 Power On 显示值会包含 RED 三字节帧头；这是当前客户端行为。
- NekokoLPA2 RED v1 单命令等待约 15 秒，因此 ML307A 超过 14 秒的 APDU会被固件终止并
  返回 `6F00`。若实际下载中出现合法 APDU 超过该时间，需要同时放宽客户端和固件超时。
- ML307A AT 固件必须支持 `AT+CSIM`；ML307A 支持，部分 ML307S 固件不支持。
- ATR 依赖 ASR 私有命令 `AT*GATR`；冷复位和直接 T=0/T=1 控制仍由 ML307A 持有。
- BLE 和 HTTP 最终共享同一 UART。当前 Arduino 主循环使它们串行，但用户仍应避免两端
  同时发起 eUICC 操作，以免改变基本/逻辑通道状态。
