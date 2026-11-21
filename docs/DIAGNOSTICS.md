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

`benchmarks/windows-diagnostic-service-evidence-v1.json`记录当前Windows本地接入证据：

- 服务发现返回5个诊断方法；
- 50次缺少`diagnostic.read`权限的读取全部在运行时拒绝；
- 授权读取返回10个Confirmed DTC；
- Default Session中的清除返回`SESSION_DENIED`；
- Start Session、Request Seed、Submit Key与Clear全部经由类型化运行时完成；
- 同一Clear请求重试产生1次`IDEMPOTENT_REPLAY`，没有第二次进入诊断清除；
- 清除后Active DTC为0；
- 运行时与诊断核心分别产生65和26条Trace事件。

该证据采用进程内类型化调用与合成DTC。它证明服务契约、权限边界、诊断状态机组合和清除重试语义，不证明远程UDS、DoIP、CAN-TP、SOME/IP诊断映射、物理ECU或诊断仪互操作。Linux对应报告由CI在原生GCC环境生成；只有将其下载并纳入仓库后，才作为Linux原始证据引用。
