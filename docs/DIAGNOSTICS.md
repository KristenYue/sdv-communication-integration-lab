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

## Windows证据

`benchmarks/windows-diagnostic-evidence-v1.json`记录：

- 20个DTC、40个跨周期失败报告，20/20进入Confirmed；
- 20次同周期重复失败没有把Occurrence从2错误放大；
- 20/20首次Freeze Frame保留；
- 重启后20/20 Confirmed恢复；
- 30次Default Session清除全部拒绝；
- 20组错误Key全部在第三次进入Lockout；
- 授权清除成功，再次重启后Active DTC为0；
- 人工损坏Journal后恢复失败关闭；
- 全流程产生166条诊断Trace事件。

CI同时在Windows和Linux重新运行相同证据程序。
