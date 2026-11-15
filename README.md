# 魅族遥控器蓝牙网关

`0.7.1` 新增 HA 原生 OTA 升级：认证 HTTPS 下载、双槽首启回滚与最小恢复模式。
首次需要 USB 引导安装和每机密钥配置；分区、部署、发布及恢复说明见 [OTA 维护](docs/ota.md)。

本 fork 的 `0.4.0-ack1` 增加红外发送应答，并修复 R16 的分包长度（每片 15 字节、完整 BLE 帧最多 20 字节）和整倍数尾包丢失。构建使用 ESP-IDF 4.3.7，协议测试见 `tests/ir_protocol_test.cpp`；CI 构建产物中的 `.bin` 是应用镜像。仓库原有 `build/` 内的旧镜像未更新。

`config_info.data.capabilities` 包含 `ir_ack_v1`。客户端可在 `irsend.data` 加入最多 64 字节的 `request_id`，网关只向发起连接返回：

```json
{"type":"irsend_result","data":{"request_id":"unique-id","device":"ab:2d:12:34:56:ef","success":true,"confirmed":true,"cached":true,"status":"ok","ack":4,"elapsed_ms":320}}
```

`ack:4` 表示小圆饼确认缓存发送，`ack:2` 表示上传新码后收到最终确认。缺少缓存且未提供完整码返回 `cache_miss`；蓝牙连接、订阅、写入失败或超时分别返回错误状态，绝不把 TCP 写入当成发码成功。`confirmed` 仅代表小圆饼的蓝牙应答，无法确认被控电器的实际状态。超时后不自动重发；同一次上传内仅响应小圆饼的缺包请求。TCP 输入支持拆包和连续 JSON 对象，单个对象仍限 511 字节。

已有网关升级前应备份整片 Flash，并读取实际分区表与 OTA 选择信息。确认当前应用槽和大小后，仅向该槽写入应用镜像；保留原 bootloader、分区表、NVS、OTA 数据以及其它分区。不要用仓库内的分区表覆盖已配置网关。
## 稳定性版本 0.5.0-stability1

- 设备状态改用值对象和 `std::string`，JSON 使用 cJSON 动态构建，缺失信息不会解引用空指针。厂家、型号、固件分别补读；只有完整温湿度和电压读取成功才更新测量时间。
- TCP 在首次蓝牙探测前启动。单个 BLE worker 优先执行控制队列，后台每次只探测一只设备。队列上限 8，排队启动期限 15 秒，过期请求不会稍后发射；已经开始的请求超时不自动重发。正在执行的单次 BLE 操作不会被强制中断。
- 不再在五次失败后停止轮询。失败后按 5、10、20 秒等指数退避，最多 15 分钟，持续探测直到恢复。成功后回到原温湿度轮询间隔。`devices` 始终包含全部绑定，离线设备标记 `available:0`，并报告 `failures`、`next_probe_in_ms`、真实样本的 `measurement_age_ms`。
- 绑定、解绑与测量提交使用短锁及绑定代次，正在工作的状态副本不会因解绑被释放，也不会在重新绑定后写回旧结果。每次绑定窗口仅接受一只设备。
- Wi-Fi 重试使用定时器，1 秒起退避至约 30 秒并附少量抖动，断网不会阻塞默认事件循环，也不会因累计重试而循环重启。配网流程和现有 NVS 保持兼容。

新增只读 `{"type":"diagnostics"}`，返回运行时长、重启原因码、空闲堆内存、BLE 工作与排队状态、Wi-Fi 断连原因码和重试计数。`{"type":"refresh","data":{"device":"ab:2d:12:34:56:ef"}}` 可排入一次普通探测，不发射红外。维护操作 `{"type":"wifi_reconnect"}` 在返回后安排一次 Wi-Fi 重连，使用同一自动恢复流程，不清除配置。

`config_info` 声明 `recovery_v1`、`measurement_age_v1`、`diagnostics_v1`。支持样本年龄的 HA 客户端可在网络恢复后还原真实测量时间；不能把缓存列表的到达时间当作测量时间。串口打开可能复位开发板，长期故障诊断应保持串口连接或使用网络诊断计数。

主机回归覆盖原发码协议、缺失及长设备信息、内存所有权、超过五次失败后的恢复、排队取消/过期，以及正常 Wi-Fi 回调的非阻塞重试、退避上限、错误码和主动重连。`MEIZU_CJSON_DIR` 指向 ESP-IDF 4.3.7 的 `components/json/cJSON` 后运行 `tests/run-host.sh`；CI 在 AddressSanitizer/UndefinedBehaviorSanitizer 下运行相同测试。

## 原生学习版本 0.6.0-learn1

`config_info` 新增能力 `learn_v1`。学习事务在 BLE worker 中执行，TCP 接收线程立即返回，期间心跳、诊断和取消仍可处理。一个网关同时保留一个学习会话，学习期间新发码或绑定返回忙；已开始的 BLE 操作先结束。原五分钟等轮询配置不变，结束后自动恢复探测。

```json
{"type":"learn_start","data":{"request_id":"unique-id","device":"ab:2d:12:34:56:ef","timeout":15}}
{"type":"learn_cancel","data":{"request_id":"unique-id","device":"ab:2d:12:34:56:ef"}}
```

网关仅向发起连接推送 `learn_event`，所有事件包含原 `request_id` 和 `device`：

- `phase:queued` / `connecting`：等待工作或连接中。
- `phase:ready,timeout_ms:15000`：实际收到设备 `0x07` 后开始学习窗口。`timeout` 可为 1–15 秒。
- `phase:receiving,received:<bytes>,length:<bytes>`：已收到头部或数据，提示用户停止按键。
- `phase:complete,status:ok,length:<bytes>,raw:<hex>`：完整重组 10–1500 字节原始数据并完成 `0x0B` 确认。
- `phase:failed,status:<reason>` 或 `phase:cancelled,status:cancelled`：未成功。

完成事件不是可发射码；HA 端须先将 ET4007 捕获转换成发送格式、校验后保存。固件只接收，不预装也不发射。每包最多 15 字节，缺包通过 `0x0A` 请求；未就绪时最多等待 8 秒，数据接收有总期限。所有退出路径发送 `0x06` 停止并断开 BLE。发起 TCP 连接断开、显式取消或绑定代次改变都会终止学习。使用递增连接 ID，避免旧事务结果发送给复用内存的新连接。

输入 JSON 仍限 511 字节；学习结果是输出消息，最大约 3.2 KB，客户端须支持流式 JSON 解析。旧 `irrecv` 保留串口诊断，新的 HA 客户端使用 `learn_start`。协议离线测试见 `tests/learn_protocol_test.cpp`，覆盖就绪、错误序号、超时、取消、断连、缺包和接收上限；测试断言整个学习过程没有发射指令。

[![Stable](https://img.shields.io/github/v/release/georgezhao2010/esp32_meizu_remoter_gateway)](https://github.com/georgezhao2010/esp32_meizu_remoter_gateway/releases/latest)

一个ESP32固件，用于魅族遥控器的网关，可以将魅族遥控器的温湿度数据通过局域网进行推送，同时还支持通过局域网控制魅族遥控器发送和接收红外数据。

魅族遥控器如下图：

![remoter](https://user-images.githubusercontent.com/27534713/131553205-0a2ce801-df07-4943-ba96-bc730ce44635.jpg)

# 特点
- 零代码/零配置文件/零命令行操作
- 手机App配网，并支持重新配置，修改Wi-Fi不需要重新烧录固件
- 与遥控器智能绑定，可以随时向网关添加遥控器绑定或者移除遥控器，增减遥控器不需要烧录固件
- 通过通讯传送需要发送的红外码，改变红外码不需要烧录固件
- 可以绑定多个遥控器，轮询查询各遥控器的温湿度数据并通过网络推送
- 网络中可以存在多个蓝牙网关，每个绑定不同遥控器，实现全屋覆盖
- 固件基于[ESP-IDF](https://github.com/espressif/esp-idf)编写，TTL日志输出，方便定位问题

# 下载和烧录

克隆本仓库，或者下载最新的Release，在build目录下有一个esp32_meizu_remoter_gateway.bin文件，这就是ESP32的固件，请下载并使用[esphome-flasher](https://github.com/esphome/esphome-flasher/releases/latest)烧录进ESP32中。

由于烧录过程中，esphome-flasher可能会访问网络获取分区表和bootloder，如果是中国国内网络，可能会显示网络超时错误，关掉esphome-flasher并重开，多尝试几次即可。

# 基本概念定义
图中是比较常见的ESP32开发板NodeMCU-32S。
![esp32](https://user-images.githubusercontent.com/27534713/131534909-ef80cda9-1f67-4032-b2b1-15576f4d1030.jpg)
## 功能键
上方红圈是GPIO0的下拉微动开关，按住可以下拉GPIO0的电平，我们在下文中称为功能键，用于网关的操作。
## 状态灯
下方红圈的蓝色LED接在GPIO2，我们在下文中称为状态灯，用来表示网关的状态。状态灯有以下状态。
- **常亮**, 状态灯一直亮的状态，表示此时无线网络连接正常。
- **常灭**, 如果电源指示灯(红色LED)正常点亮，而状态灯一直为灭的状态，表示此时无线网无法连接，可以检查无线路由器或者重启网关。
- **慢闪**, 此时状态灯2秒钟闪烁一次，我们在下文中称为慢闪，具体含义下文会提及。
- **快闪**, 此时状态灯每秒钟闪烁5次，我们在下文中称为快闪，具体含义下文会提及。

*注意:使用非NodeMCU-32S的其它ESP32模块，也许需自行处理GPIO0/GPIO2的输入输出*


# 配网
魅族遥控器网关需要Espressif官方的无线网络配网工具，iOS手机请在App Store中搜索ESP SoftAP Provisioning，Android手机请搜索ESP SoftAP Provisioning并下载安装。

## 初次配网
- 刚烧录好固件并启动后，状态灯为慢闪状态，此时用手机搜索ESP_XXXXXX的无线网并加入，密码为"ESP32xAP"。
- 打开ESP SoftAP Provisioning手机App，点按"Provision Device"按钮。
- 在二维码配网步骤，直接点按"I don't have a QR code"
- PIN码输入界面，输入PIN码"p1d32a94"
- 在选择无线网界面，直接选择无线网，并输入密码。如果无法搜索无线网，点击"Join Other Network"，并在弹出的窗口输入需要连接的无线网SSID及密码，并点击"Connect"
- 如果前述步骤没有错误，网关状态灯进入快闪状态，此时网关设备将重启，重新启动后，状态灯如果成功连接上无线网，会显示为常亮状态。

## 重新配网
在任意状态下，按住功能键10秒钟，状态灯进入快闪状态，网关的配网信息将被清除，重启后又将进入配网状态，配置的步骤与初次配网操作相同。

*注意:按住功能键10秒，是初始化所有设置，除无线网配置信息被清除以外，绑定的全部遥控器也将都被移除。*


# 绑定遥控器
在网关工作时，需要知道采集哪些遥控器的信息，可以通过绑定与解绑操作，告知网关需要采集的遥控器的蓝牙地址集合。

## 绑定
- 在网关进入正常工作模式(非配网模式)时，按住功能键2秒钟，网关进入绑定模式，此时状态灯为慢闪状态。
- 拿起一个遥控器并尽量靠近网关，网关如果发现这个设备，状态灯会快闪2秒，并退出绑定模式，该遥控器绑定成功。

或 在[魅族遥控器网关集成](https://github.com/georgezhao2010/meizu_remoter_gateway)中调用bind服务指定进入绑定状态

## 解绑单个遥控器
此部分操作需要在[魅族遥控器网关集成](https://github.com/georgezhao2010/meizu_remoter_gateway)中进行操作，见[解除绑定](https://github.com/georgezhao2010/meizu_remoter_gateway#%E8%A7%A3%E9%99%A4%E7%BB%91%E5%AE%9A)部分的描述。

## 解绑全部遥控器
与重新配网相同，按住功能键10秒钟，状态灯进入快闪状态，所有绑定的遥控器均会被清除。

*注意:按住功能键10秒，是初始化所有设置，除绑定的全部遥控器会被移除外，无线网配置信息也将被清除。*
*注意:原则上不推荐不同网关绑定同一个遥控器*

# 数据轮询与更新

网关开始工作后，按照设定的时间间隔轮询所有绑定的遥控器设备，并将轮询的结果数据推送到连接的所有客户端。默认的轮询时间为5分钟，这一间隔可以在[魅族遥控器网关集成](https://github.com/georgezhao2010/meizu_remoter_gateway)中进行修改，见[选项](https://github.com/georgezhao2010/meizu_remoter_gateway#%E9%80%89%E9%A1%B9)部分的描述。

原版固件在连续五次失败后停止轮询；本 fork 的稳定性版本改为持续退避探测。遥控器恢复供电或回到覆盖范围后，无需重新绑定。

# 红外发送与接收
## 发送
支持按照传送的key与ircode,让遥控器发送指定红外信号，当遥控器发送完成后，网关会快闪1秒
## 接收(Beta)
可以使网关进入20秒的红外接收状态，收到的红外数据将以蓝牙Notify的形式输出在TTL。据观察，遥控器似乎并不能准确接收红外数据，时灵时不灵，接收到的数据也奇形怪状，有待验证。

# 网络服务与通讯
## mDNS
魅族遥控器网关支持mDNS服务,服务名称为`_meizu_remoter_gateway._tcp.local.`,使用zeroconf可获得以下格式信息
```
type='_meizu_remoter_gateway._tcp.local.', name='1C4BD9._meizu_remoter_gateway._tcp.local.', addresses=[b'\xc0\xa8\x01\x87'], port=8266, weight=0, priority=0, server='1C4BD9.local.', properties={b'version': b'0.1.0', b'serialno': b'1C4BD9'}
```

## TCP
魅族遥控器网关开放并监听8266端口，支持多用户连接，payload形式为JSON。

### 网关接收消息
- 获取网关配置信息
```
{
  "type": "config_info"
}
```
- 订阅消息推送，只有向网关发送该消息，才会收到消息推送
```
{
  "type": "subscribe"
}
```
- 使网关进入遥控器绑定状态(最长30秒)
```
{
  "type": "bind"
}
```
- 调整绑定操作信号阈值
```
{
  "type": "bindthrld",
  "data":{
    "threshold":-35
  }
}
```
- 移除已经绑定的遥控器
```
{
  "type":"removebind",
  "data":{
    "device":"ab:6d:12:34:56:ef"
  }
}
```
- 心跳, 如3分钟内无心跳或其它消息，客户端将被断开
```
{
  "type":"heartbeat"
}
```
- 设置更新间隔(分钟)
```
{
  "type":"setinterval",
  "data":{
    "update_interval":10
  }
}
```
- 发射红外
```
{
  "type":"irsend",
  "data":{
    "device":"ab:2d:12:34:56:ef",
    "key":"65001C63C68D8000C8",
    "ircode":"0123456789abcdef..."
  }
}
```
- 接收红外
```
{
  "type":"irrecv",
  "data":{
    "device":"ab:2d:12:34:56:ef"
  }
}
```

### 网关回应消息
- 获取网关配置信息回应
```
{
  "type":"config_info",
  "data":{
    "version":"0.3.0",
    "serialno":"1B923A",
    "update_interval":5
  }
}
```
- 绑定状态回应
```
{
  "type": "bind",
  "data":{
    "status":1
  }
}
```
- 心跳回应
```
{
  "type":"heartbeat"
}
```

### 网关推送消息
- 设备状态更新
```
{
  "type":"update",
  "data":{
    "device":"12:62:33:b5:9f",
    "available":1,
    "status":{
      "manufacturer":"Meizu",
      "model":"R16",
      "fireware":"v_3.0.10.23",
      "temperature":32.62,
      "humidity":75.33,
      "battery":90,
      "voltage":2.9,
      "rssi":-47
    }
  }
}
```
- 更新间隔变化推送
```
{
  "type":"setinterval",
  "data":{
    "update_interval":10
  }
}
```
- 设备移除推送
```
{
  "type":"removebind",
  "data":{
    "device":"ab:6d:12:34:56:ef"
  }
}
```

关于网关与网关集成的更多操作，请参阅[魅族遥控器网关集成](https://github.com/georgezhao2010/meizu_remoter_gateway/README.md)的说明。
