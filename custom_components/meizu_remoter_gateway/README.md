# 魅族蓝牙网关：Home Assistant 集成

开发版 **1.5.0** 新增 [ESPHome BLE 组件连接方式](https://github.com/hallucigenia/esp32_meizu_remoter_gateway/blob/feature/esphome-component/components/meizu_ble/README.md)，保留原生学习向导、命名按键库与遥控动作。该方式的传感器和固件更新由标准 ESPHome 集成提供，魅族集成只增加遥控实体。已有 TCP 方式继续支持。

以下是已发布 **1.4.0** 的 TCP 使用说明。稳定版安装包尚不包含 ESPHome 适配；测试新方式请使用本开发分支的集成源码。两种方式均不依赖 HA 主机蓝牙、`bluepy`、MQTT 或原厂 App。

## 安装与绑定

从 [HA 发布页](https://github.com/hallucigenia/esp32_meizu_remoter_gateway/releases/tag/ha-v1.4.0) 下载集成 ZIP，将 `custom_components/meizu_remoter_gateway/` 放到 HA `/config/custom_components/` 中。已有安装先备份代码；保留 HA 的配置项和 `.storage`，以保留绑定入口及已学习的按键。重启 HA 后，在「设置 → 设备与服务 → 添加集成」搜索「魅族蓝牙网关」，填写网关 IP 与端口，默认端口 `8266`。

通过开发者工具的动作页面执行：

```yaml
action: meizu_remoter_gateway.bind
data:
  serial_no: ABC123
```

`ABC123` 是示例序列号，请替换为自己的网关编号。把一只有电的小圆饼放近网关完成绑定。建议温湿度轮询间隔为 5–10 分钟；`set_update_interval` 支持 1–30 分钟，并等待网关回读确认。设备初始列表只是缓存；真正的测量推送到达后，传感器才开始可用。断连、测量失败或样本超过两个轮询周期加 90 秒会标记不可用。

## 原生红外学习

需要支持 `learn_v1` 的网关固件（0.6.0 及以上）。在集成的「配置」中选择小圆饼，填写设备名和按键名。让原遥控器发射头对准小圆饼顶部中心，距离小于 1 厘米；提交后等待连接，收到就绪提示才短按按键。

也可使用 HA 标准动作，实体 ID 请按自己的安装替换：

```yaml
action: remote.learn_command
target:
  entity_id: remote.meizu_puck
data:
  device: tv
  command: power
  command_type: ir
  timeout: 15
```

只有设备返回 `0x07` 才开始 1–15 秒的学习窗口。完整接收、Python 转码、传输长度检查和持久化成功后才保存按键。同名按键需要显式允许覆盖；失败保留旧码。向导提供学习下一键、试发、重新学习和结束，关闭向导会取消当前学习。标准动作可通过以下服务取消：

```yaml
action: meizu_remoter_gateway.cancel_learning
data:
  serial_no: ABC123
```

学习和保存不预装也不发射红外。试发会实际操作电器；学习期间新的发送和绑定请求返回忙，结束后恢复原轮询。断网、取消和解绑均有终止路径。

## 发送与删除

```yaml
action: remote.send_command
target:
  entity_id: remote.meizu_puck
data:
  device: tv
  command: power
```

使用 `remote.delete_command` 可删除对应 `device` 和 `command`。按键保存在 HA `.storage/meizu_remoter_gateway.<entry_id>.commands`；其中含原始捕获、完整 `ir_code`、缓存 `key`、格式版本、载频、SHA-256 和学习时间。这个文件是用户运行数据，不属于可公开的源码或发布附件。

增强固件等待缓存发送的 `0x04` 或完整上传后的 `0x02` 应答，核对请求编号、设备地址和结果后才标记 `confirmed: true`、`bridge_acknowledged: true`。超时不会自动重发。`last_ir_result` 保存最近结果；蓝牙确认不等于被控电器状态确认。

`send_ir` 支持直接发送 `serial_no`、`address`、`key` 与可选 `ir_code`，并提供动作响应；`store_command` 使用同样的码值字段，再加 `device` 和 `command`，只保存而不发射。`key` 是九字节发射参数头，不是设备云端标识。当前 TCP 请求限 511 字节，含关联编号；过长或格式无效的码会在发送前拒绝。

## 固件更新

需要 OTA 固件 0.7.3+；当前稳定固件为 0.7.4。首次迁移旧固件须通过 USB 安装匹配的 OTA 基础版及回滚 bootloader，并核对实际分区、备份整片 Flash。详细步骤见 [OTA 文档](https://github.com/hallucigenia/esp32_meizu_remoter_gateway/blob/master/docs/ota.md)。

通过私有 USB 串口输入 `ota-key` 读取该网关独立密钥，再在集成菜单「重新配置」中录入；留空保留旧值。表单会验证密钥，提交表单不会升级或发射。密钥存于设备 NVS 与 HA 配置项，不能发布。

网关设备下的「固件」更新实体每六小时检查稳定版。安装由网关直接下载固定版本的 HTTPS 镜像，校验型号、布局、大小、版本和 SHA-256 后写备用槽；稳定启动确认后，HA 才结束安装。HA 重启或断线后按原事务查询，不自动重复刷写。

升级期间暂停遥控和采样。正常模式下载失败或取消时会重启原固件恢复蓝牙；连续启动失败进入仅网络与 OTA 的恢复模式。已有绑定、轮询间隔及 HA 按键库保留。回滚能力须通过实际 bootloader 验证，不能只根据双分区推断。

## 兼容性与测试

旧 0.3.2 固件可采样与发码，但发送只能报告未确认，学习仅有串口日志。该版本存在 21 字节 BLE 长码写入问题；增强固件使用 15 字节码分片、最多 20 字节完整 BLE 帧，并支持缓存丢失后补装完整码。

`codec.py` 无第三方依赖，不需要 APK、JNI 或模拟器。九组 R16 捕获与原厂转换结果逐字节一致；编码最多支持八组时序对。测试向量只包含波形与通用按键名，不含设备身份或凭据。空调长报文尚未完成实机验证。

仓库根目录执行离线回归：

```sh
python3 -B -m unittest discover -s tests/meizu_remoter_gateway -v
```

测试无需安装 Home Assistant，使用模拟服务与运行层替身；这些离线检查不等于对所有 HA 版本和设备型号的实机认证。集成保留 [MIT 许可证](LICENSE) 与原作者署名。
