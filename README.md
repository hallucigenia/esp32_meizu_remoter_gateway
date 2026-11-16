# 魅族小圆饼 BLE 网关与 Home Assistant 集成

**开发分支：**新增 [ESPHome 魅族 BLE 外部组件](components/meizu_ble/README.md) `0.1.0`，以及 HA 集成 `1.5.0` 的 ESPHome 适配。已提供配置、主机回归和固件编译验证；ESPHome 固件尚待实机验收。下方 `v0.7.4` / `ha-v1.4.0` 是既有稳定方案。

通过 ESP32 连接魅族 R16 小圆饼，在局域网读取温湿度、电量和蓝牙信号，并学习、保存和发送红外指令。日常运行不需要魅家 App、云服务、APK、Mac 或 Home Assistant 主机蓝牙。

本项目基于 [George Zhao 的 ESP32 网关](https://github.com/georgezhao2010/esp32_meizu_remoter_gateway)，增加可靠发码回执、离线恢复、原生学习与 OTA；HA 集成参考其 [原集成](https://github.com/georgezhao2010/meizu_remoter_gateway)。源码许可范围见 [NOTICE](NOTICE.md)。

| 部分 | 当前版本 | 入口 |
| --- | --- | --- |
| ESP32 固件 | 0.7.4 | [稳定发布](https://github.com/hallucigenia/esp32_meizu_remoter_gateway/releases/tag/v0.7.4)、[固件与协议](docs/firmware.md)、[安装与 OTA](docs/ota.md) |
| Home Assistant 集成 | 1.4.0 | [安装包](https://github.com/hallucigenia/esp32_meizu_remoter_gateway/releases/tag/ha-v1.4.0)、[安装与使用](custom_components/meizu_remoter_gateway/README.md) |

## 安装

1. 准备 ESP32 与魅族小圆饼。已有网关升级前，先备份整片 Flash 并核对实际分区；首次安装及旧 bootloader 迁移按 [OTA 说明](docs/ota.md) 操作。
2. 从 HA 集成发布页下载 `meizu_remoter_gateway-1.4.0.zip`，将其中的 `custom_components/meizu_remoter_gateway/` 复制到 HA 的 `/config/custom_components/`。先备份已有同名集成目录，再替换代码；保留现有 HA 配置与 `.storage`。
3. 重启 HA，在「设置 → 设备与服务 → 添加集成」搜索「魅族蓝牙网关」，填写网关地址和 TCP 端口 `8266`。
4. 调用集成的 `bind` 动作，把一只有电的小圆饼放近网关完成绑定。等待真实采样后，温湿度等传感器开始可用。
5. 在集成的「配置」中逐键学习，或调用标准 `remote.learn_command`。收到就绪提示后再短按原遥控器；保存不发射，试发需单独操作。

OTA 首次配置需要通过 USB 串口读取每台网关的独立密钥，再填入集成「重新配置」表单。后续可在 HA 的固件更新实体中安装稳定版。

## 主要能力

- 每只小圆饼提供温度、湿度、电量、电池电压、RSSI、最近测量和红外遥控实体。
- 发码等待小圆饼的实际蓝牙应答；缓存丢失时可重新上传完整码。失败和超时明确报错，不自动重发。
- 原生逐键学习支持就绪提示、取消、超时、重学和显式覆盖；接收或保存失败保留原按键。
- Python ET4007 编解码器不依赖原厂二进制库；按键与原始捕获保存在 HA 本地。
- 设备离线后持续退避探测；网络恢复保留真实测量年龄，缓存不冒充新采样。
- 固件支持认证 HTTPS OTA、启动确认、回滚和最小恢复模式；升级前仍需核对硬件与分区兼容性。

蓝牙回执表示小圆饼确认发送，无法证明被控电器已经切换输入或改变电源状态。独立网关的 TCP 输入限 511 字节；空调长报文尚待实机验证。ESPHome 组件使用标准原生 API，接口与迁移边界见其单独说明。

## 源码与离线验证

| 路径 | 内容 |
| --- | --- |
| `main/` | ESP32 BLE、发码、学习、调度与 OTA |
| `custom_components/meizu_remoter_gateway/` | HA 集成、原生配置流、ET4007 编解码器 |
| `tests/meizu_remoter_gateway/` | HA 协议、学习、持久化和 OTA 回归 |
| `tests/*_test.cpp` | 固件主机协议与恢复回归 |
| `tools/ir_compat.py` | 无设备 I/O 的旧版 NEC 校验与分包兼容工具 |
| `tools/package-ha-release.py` | 仅打包集成文件的可复现 ZIP 生成器 |

HA 离线测试使用 Python 3.11+，只连接测试创建的回环 TCP 服务，不访问家庭网络或操作设备：

```sh
python3 -B -m unittest discover -s tests/meizu_remoter_gateway -v
```

另有真实 HA 事件总线回归：在安装 `homeassistant==2026.11.1` 的 Python 3.14 环境中运行 `python -B -m unittest discover -s tests/ha_core -v`。测试启用 asyncio debug，检查接入握手、发码回执和实体状态更新的事件循环上下文；使用临时 HA 实例，不连接设备。

固件使用 ESP-IDF 4.3.7 构建。主机 sanitizer 测试：

```sh
MEIZU_CJSON_DIR=/path/to/esp-idf/components/json/cJSON tests/run-host.sh
```

发布说明及资料范围见 [维护与发布](docs/publishing.md)。HA 安装包使用 `ha-v*` 标签，固件使用 `v*` 标签；HA 包不会替换固件更新清单所指向的最新稳定版。
