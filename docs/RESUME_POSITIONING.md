# 智能汽车平台软件岗位简历定位

## 唯一项目定位

项目在简历上的主语是：**研发C++车载服务运行时与通信诊断集成平台**。

它服务于以下同一人才画像，不拆成多个互不相关的项目：

- 智能汽车平台软件研发；
- 车载通信与中间件研发；
- 车辆诊断、系统集成与仿真工具链研发。

不把项目包装成测试平台、通用Agent、完整AUTOSAR栈或量产UDS协议栈。

## JD关键词与项目证据

| JD高频要求 | 仓库中的直接证据 |
|---|---|
| C++、Linux、系统编程 | C++20核心数据面；Linux `PF_CAN/CAN_RAW`；GCC/MSVC与ASan/UBSan CI |
| CAN、SocketCAN、车载以太网 | 四类车辆CAN消息；内核`vcan0`真实收发；有界SOME/IP/UDP跨进程调用 |
| 服务化、中间件、平台架构 | 类型化服务注册/发现/调用；Principal/Permission；Deadline、Cancel与Trace |
| 诊断与系统集成 | Operation Cycle、DTC、Freeze Frame、Session、Seed/Key、Lockout；网关故障自动桥接 |
| 可靠性与异常恢复 | 请求幂等、冲突拒绝、持久化Journal、结果未知、并发In-flight去重与背压 |
| 仿真、回放和数据闭环 | 200个多节点场景、100次故障注入、300个回放任务、Parquet与PostgreSQL调度 |

## 一页简历只保留三条

仓库继续完整保留所有核心模块、工程实现、原始报告和技术亮点；这里的取舍只针对一页简历。以下表述必须根据投递版面继续压缩，但不要再增加第四条简历技术主线。

1. **平台内核：** 使用C++20研发车载服务运行时，完成类型化服务注册/发现、权限、Deadline/Cancel、Trace及有副作用请求的幂等与重启恢复；通过有界SOME/IP/UDP实现跨进程车辆状态、车身控制与诊断服务调用。
2. **通信诊断闭环：** 构建CAN/SocketCAN车辆网关并接入周期级诊断监控，将DLC、范围、时序、未知ID及四节点超时自动映射为DTC/Freeze Frame；Linux `vcan0`原生链路两周期确认6/6 DTC，并由SOME/IP接口读取一致结果。
3. **系统集成证据：** 建立VCU、TPMS、BCM、T-Box多进程仿真与确定性回放，执行200个冻结系统场景、100次故障注入和300个回放任务；在Windows/Linux、Sanitizer与PostgreSQL CI中保存可复现原始报告。

## 面试时再展开的内容

- 60组幂等、重启恢复和网络结果未知实验；
- 并发Follower去重、Request ID冲突和背压语义；
- 27格性能矩阵及测量边界；
- DTC确认、老化、Freeze Frame和Security Lockout状态机；
- SQLite/PostgreSQL租约、Fencing与Parquet内容寻址数据。

这些内容证明技术深度，但不同时塞进简历正文，避免画像变成“什么都做过但没有主线”。

## 禁止写入简历的越界表述

- 完整AUTOSAR或量产SOME/IP协议栈；
- 完整ISO 14229 UDS、DoIP或CAN-TP实现；
- 真实车辆、物理CAN总线或量产ECU适配；
- 功能安全、网络安全认证或硬实时保证；
- 分布式Exactly Once、数据库高可用或多机生产部署。

仓库证据支持的是Linux虚拟CAN、项目有界SOME/IP、合成车辆节点和可复现系统软件实验。
