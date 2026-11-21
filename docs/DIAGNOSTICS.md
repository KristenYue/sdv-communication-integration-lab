# 有界车辆诊断与DTC管理

## 定位

该模块面向智能汽车平台软件中的诊断状态管理：把车辆信号故障转换为具有生命周期、Freeze Frame、访问控制和重启恢复语义的DTC。实现借鉴UDS的会话与Security Access形态，但不是完整ISO 14229协议栈，也没有物理ECU、DoIP、CAN-TP或诊断仪互操作证据。

```text
车辆信号 / 故障监控器
        │ report_test_result
        ▼
DiagnosticManager
  ├─ Operation Cycle去重
  ├─ Pending / Confirmed / Aging
  ├─ 首次故障Freeze Frame
  ├─ Default / Extended Session
  ├─ Seed / Key / Lockout
  ├─ 授权清除
  ├─ CRC-32追加Journal
  └─ Trace事件
```

诊断核心已经通过`vehicle.diagnostics`适配器接入`VehicleServiceRuntime`。上层调用不直接持有`DiagnosticManager`，而是经由类型化服务契约、Principal、Permission、Request ID、Deadline与统一Trace进入以下方法：

| 方法 | 所需权限 | 语义 |
|---|---|---|
| `StartSession` | `diagnostic.session` | 为指定客户端启动Default或Extended Session |
| `RequestSecuritySeed` | `diagnostic.security` | 在会话状态机中签发一次挑战 |
| `SubmitSecurityKey` | `diagnostic.security` | 校验挑战绑定、错误计数与Lockout |
| `ReadDTCInformation` | `diagnostic.read` | 返回类型化Active DTC记录 |
| `ClearDiagnosticInformation` | `diagnostic.clear` | 执行受会话与解锁状态保护的清除 |

服务端时间由运行时适配器注入，客户端不能用请求载荷伪造会话时间。运行时先执行服务发现与权限准入，再进入诊断状态机；错误`std::variant`载荷被拒绝。所有协议交换应使用唯一Request ID；相同ID仅用于对同一个逻辑调用进行重试。尤其是Session、Seed与Key请求不得把旧Request ID复用于新的协议交换。`ClearDiagnosticInformation`的相同请求重试会返回已缓存终态，不会再次执行清除。

## DTC生命周期

- DTC必须预注册24位Code、名称和Severity，未知监控器不能动态写入状态；
- 同一Operation Cycle内重复失败不增加Occurrence，避免高频采样膨胀故障次数；
- 第一个失败周期进入Pending并捕获首次Freeze Frame；
- 连续第二个失败周期进入Confirmed，首次Freeze Frame不被后续样本覆盖；
- 两个连续通过周期清除Pending，三个连续通过周期老化Confirmed；
- Active视图只返回Failed、Pending或Confirmed记录，历史定义和Occurrence仍保留；
- Operation Cycle ID严格单调，并从Journal恢复，进程重启不能复用旧周期制造重复Occurrence。

当前确认与老化阈值属于项目策略，不声称对应某一OEM或法规标定。

## 网关故障自动桥接

`GatewayDiagnosticBridge`把`VehicleGateway`的通信契约结果与信号新鲜度转换为周期级诊断监控器。它预注册8个DTC：`INVALID_DLC`、信号越界、陈旧/冲突帧时序、未知CAN ID，以及VCU、TPMS、BCM、T-Box四组信号超时。

桥接器在一个Operation Cycle内收集任意数量的帧结果，周期结束时才对每个监控器提交一次Fail或Pass。这样，同周期内高频重复故障不会放大Occurrence；没有故障的周期会显式提交Pass，使Pending清除和Confirmed Aging能够发生。契约故障沿用原始帧Trace ID，超时故障使用周期关闭Trace；首次失败时保存网关完整快照作为Freeze Frame。

`benchmarks/windows-gateway-diagnostic-bridge-evidence-v1.json`与`benchmarks/linux-gateway-diagnostic-bridge-evidence-v1.json`记录两次故障周期：每周期包含四组有效车辆帧、四类契约故障，并在周期结束时让四组信号超过新鲜度阈值。两平台均得到8/8 DTC进入Confirmed、8/8 Freeze Frame有效、8/8 Occurrence等于2、8个来源Trace可区分；随后类型化`ReadDTCInformation`与有界SOME/IP解码均返回同一组8个Active DTC。除运行平台字段外，两份报告逐字段一致。

这些DTC Code、Severity和新鲜度阈值是项目策略。证据使用合成CAN帧与进程内SOME/IP适配器调用，不证明物理总线、电气故障、OEM诊断监控标定或量产故障内存策略。

Linux原生链路进一步在GitHub Ubuntu Runner创建内核`vcan0`，由Python虚拟ECU通过SocketCAN发送8帧，C++ `PF_CAN/CAN_RAW`进程实际接收。两周期均稳定得到`APPLIED`、`SIGNAL_OUT_OF_RANGE`、`INVALID_DLC`、`UNKNOWN_CAN_ID`各一次；缺失的TPMS、BCM和T-Box信号同时形成超时监控。最终6/6 DTC进入Confirmed、6/6 Freeze Frame有效、Occurrence均为2，并由有界SOME/IP适配器读取6个DTC。原始报告见`benchmarks/linux-socketcan-diagnostic-chain-v1.json`。

## 会话与安全状态

清除DTC必须满足：客户端处于Extended Session、会话未超时、Security Access已经解锁。三次错误Key触发10秒锁定；会话超时不会绕过尚未结束的锁定窗口。成功清除后立即撤销解锁状态。

Seed/Key算法是确定性的项目测试算法，仅用于验证会话、挑战绑定、错误计数和Lockout状态机。它不是密码学实现，不可用于量产ECU安全访问。

## 持久化语义

每次DTC状态变化在内存提交前追加并刷新完整状态记录；清除操作也先持久化再改变内存。每条记录带CRC-32。恢复按顺序重放Upsert和Clear；缺少校验、Schema错误或Checksum不匹配会拒绝启动。

这证明进程重启恢复和损坏记录失败关闭，不证明突然断电时的文件系统屏障、磨损管理、双分区NVM或量产NvM语义。

## Windows与Linux证据

`benchmarks/windows-diagnostic-evidence-v1.json`与`benchmarks/linux-diagnostic-evidence-v1.json`记录：

- 20个DTC、40个跨周期失败报告，20/20进入Confirmed；
- 20次同周期重复失败没有把Occurrence从2错误放大；
- 20/20首次Freeze Frame保留；
- 重启后20/20 Confirmed恢复；
- 30次Default Session清除全部拒绝；
- 20组错误Key全部在第三次进入Lockout；
- 授权清除成功，再次重启后Active DTC为0；
- 人工损坏Journal后恢复失败关闭；
- 全流程产生166条诊断Trace事件。

两平台得到相同的状态计数与166条Trace；平台字段保留各自原始运行环境。

## 类型化服务接入证据

`benchmarks/windows-diagnostic-service-evidence-v1.json`与`benchmarks/linux-diagnostic-service-evidence-v1.json`记录Windows本地及GitHub Ubuntu Runner接入证据：

- 服务发现返回5个诊断方法；
- 50次缺少`diagnostic.read`权限的读取全部在运行时拒绝；
- 授权读取返回10个Confirmed DTC；
- Default Session中的清除返回`SESSION_DENIED`；
- Start Session、Request Seed、Submit Key与Clear全部经由类型化运行时完成；
- 同一Clear请求重试产生1次`IDEMPOTENT_REPLAY`，没有第二次进入诊断清除；
- 清除后Active DTC为0；
- 运行时与诊断核心分别产生65和26条Trace事件。

两平台的全部语义计数一致，均产生65条运行时Trace和26条诊断Trace。该证据采用进程内类型化调用与合成DTC。它证明服务契约、权限边界、诊断状态机组合和清除重试语义，不证明远程UDS、DoIP、CAN-TP、SOME/IP诊断映射、物理ECU或诊断仪互操作。

## 有界SOME/IP跨进程映射

类型化服务进一步映射到项目定义的SOME/IP Service ID `0x1236`：五个方法分别使用Method ID `0x0001`至`0x0005`。独立C++客户端和服务端通过localhost UDP通信，Client ID映射为稳定诊断Principal，Session ID与Client ID共同形成运行时Request ID。每个新的协议交换使用新的Session ID。

传输层畸形载荷、未知服务和未知方法使用SOME/IP错误响应；诊断状态机拒绝使用成功传输响应中的一字节应用状态，避免把`SESSION_DENIED`误报成网络失败。DTC响应采用固定九字节记录：24位DTC Code、Severity、Failed/Pending/Confirmed状态位和32位Occurrence Count；名称、Freeze Frame和内部Trace不进入该有界线协议。单次最多255条记录。

`benchmarks/windows-diagnostic-someip-evidence-v1.json`与`benchmarks/linux-diagnostic-someip-evidence-v1.json`记录Windows和GitHub Ubuntu Runner上两个独立C++进程的7次请求：初始读取10个Confirmed DTC且固定字段正确；Default Session清除返回`SESSION_DENIED`；Extended Session、Seed、Key和授权清除成功；末态读取返回0个Active DTC。两平台均产生14条运行时Trace和26条诊断Trace，除运行环境与监听端口字段外语义结果一致。

该链路证明项目协议的跨进程请求关联、序列化边界和诊断状态机组合；它仍不是AUTOSAR SOME/IP一致性、Service Discovery、UDS Service/NRC、DoIP、CAN-TP、物理ECU或真实诊断仪互操作证据。
