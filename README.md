# SDV Communication Integration Lab

面向智能汽车平台软件、车载服务运行时、通信中间件与系统集成岗位的可复现研发项目。项目主体是C++车辆通信与服务软件；仿真、故障注入和回放用于证明这些核心模块的正确性与可靠性，不作为测试执行型项目定位。

最终形态是一个 **Vehicle Service Runtime & Integration Platform（车载服务运行时与通信集成平台）**：底层接入CAN与车载服务通信，中间层提供车辆服务注册、发现、调用、超时、取消、权限和状态管理，上层可按岗位选择接入诊断工具、车云任务或受控AI Agent，而不把通用Agent框架作为唯一主线。

当前第一阶段已经建立一条不依赖物理车辆的最小真实链路：

```text
车辆状态 / 胎压帧
  -> C++20 车辆网关
  -> DLC、时间戳、重复帧与冲突校验
  -> 信号解码和新鲜度状态
  -> Trace 事件

Linux vcan0
  -> Python虚拟ECU发送进程
  -> C++ SocketCAN网关
  -> 项目DBC语义与车辆状态
  -> 可审计 JSON 证据

车辆信号状态
  -> C++ Vehicle Service Runtime
  -> 服务注册与发现
  -> 类型化请求/响应
  -> 权限、Deadline、Cancel与Trace
  -> 诊断工具 / 车云任务 / 可选Agent应用
```

这不是量产 ECU，也不声称支持完整 DBC、AUTOSAR 或真实车辆控制。当前边界是：

- C++20 实现确定性的虚拟车辆网关和故障注入器；
- Python 实现项目约束内的 DBC 解码与 Linux SocketCAN 验收；
- CI 同时验证 Windows/Linux 构建，并在 Linux 创建真实 `vcan0` 接口；
- Python场景控制面通过进程管道驱动独立C++网关，冻结集与结果均保存SHA-256；
- 所有性能、可靠性和恢复结论必须在冻结场景执行后才能写入简历。

## 车辆服务运行时

`VehicleServiceRuntime`把底层车辆信号从“可以读取的C++对象”提升为有明确契约的车载服务：

- 以服务名、语义版本和方法描述符完成注册与发现；
- 使用C++ `std::variant`承载类型化请求和响应，拒绝错误载荷；
- 在运行时统一执行Principal、Permission、Deadline和Cancel准入检查；
- Trace ID与Request ID贯穿派发、拒绝和完成事件；
- 区分“派发前超时/取消”和“派发后结果不确定”：后者返回`COMPLETION_UNKNOWN`，不虚假声称有副作用的操作已经回滚；
- 首个`vehicle.state/GetSnapshot`服务把现有CAN网关状态以只读权限暴露给上层消费者。

本阶段运行时内核仍采用同步执行；其只读车辆状态方法已经接入下面的跨进程有界SOME/IP链路，但尚未声称具备动态服务发现、生产SOME/IP/DDS互操作、持久化幂等或实时调度能力。Windows/MSVC可执行演示与C++测试均已纳入构建脚本和CMake/CTest。

### 请求幂等与冲突拒绝

运行时保存`request_id -> (语义指纹, 终态响应)`：同一Principal、Service、Method和类型化Payload的重试直接返回原始响应，不再次调用Handler；同一Request ID携带不同语义内容时返回`REQUEST_ID_CONFLICT`。Trace ID和Deadline允许在重试时变化，不参与语义指纹。派发后超时、取消或Handler异常形成的`COMPLETION_UNKNOWN`也会被缓存，避免不确定操作被盲目重放。

Windows本地证据包含60组状态变更指令、共180次调用：60次首次执行、60次相同内容重试和60次冲突内容复用；Handler实际执行60次、幂等重放60次、冲突拒绝60次、重复最终动作0次。原始结果见`benchmarks/windows-runtime-idempotency-v1.json`。

运行时默认使用单进程内存缓存；状态变更方法可启用下面的持久化Journal。当前调用仍为顺序执行，不证明并发重复请求协调或调度背压。

### 进程重启恢复Journal

对标记为`state_changing`的方法，运行时在调用Handler前追加并刷新`BEGIN(request_id, fingerprint)`，在Handler返回后追加并刷新带类型化响应的`COMPLETE`。重启恢复时：完整记录重放原始响应；只有`BEGIN`的请求恢复为`COMPLETION_UNKNOWN`并禁止盲目重执行；CRC-32或记录Schema损坏则拒绝启动。

Windows本地执行60个状态变更请求后重启服务进程：第二进程重放60个响应、Handler执行0次、重复最终动作0次，并拒绝60个冲突内容。另一次实验在Handler内部以退出码73直接终止进程，重启后得到`COMPLETION_UNKNOWN`且Handler执行0次。人工追加损坏记录后，进程以退出码74失败关闭。结果见`benchmarks/windows-runtime-restart-recovery-v1.json`，状态机说明见[恢复语义](docs/RECOVERY_SEMANTICS.md)。

该证据针对进程退出/重启，不代表突然断电下的存储屏障、跨多进程写入、分布式Exactly Once或真实执行器事务。

### 并发In-flight协调与背压

运行时以Request ID和语义指纹协调并发调用：首个请求获得执行权；相同内容的Follower等待终态并重放响应；相同ID的冲突内容立即拒绝。`max_in_flight`限制活跃Handler数量，容量耗尽时，无Deadline请求返回`QUEUE_FULL`，有Deadline请求等待槽位并在到期后返回`QUEUE_TIMEOUT`。Trace通过加锁快照读取，Journal写入独立串行化。

Windows本地实测：32个线程同时提交同一状态变更，32个调用全部成功、Handler只执行1次、31次等待后重放；首个请求执行期间的16个冲突请求全部拒绝。并发上限设为2并占满后，无Deadline请求返回`QUEUE_FULL`，50 ms Deadline请求返回`QUEUE_TIMEOUT`，两者均未进入Handler。原始结果见`benchmarks/windows-runtime-concurrency-v1.json`，详细语义见[并发与背压说明](docs/CONCURRENCY_SEMANTICS.md)。

当前属于单进程多线程准入控制，Handler仍运行在调用方线程；不声称具备内部线程池、优先级调度、公平性、实时性或跨进程并发仲裁。

### 网络结果未知与重启重试

SOME/IP有界链路新增`vehicle.body/SetDoorLocks`状态变更方法。故障实验让第一服务进程正常执行指令并持久化终态，但在UDP层故意丢弃响应；客户端只能观察到超时。随后服务端携带同一Journal重启，客户端使用相同Client ID、Session ID和Payload重试，运行时重放原始响应而不再次进入Handler。

Windows localhost实测30个独立指令：第一服务进程Handler执行30次、故意丢弃30个响应、客户端超时30次；重启后30次重试全部成功、30个请求身份全部关联、第二进程Handler执行0次、重复最终动作0次。原始报告见`benchmarks/windows-network-recovery-v1.json`。

该结果验证的是合成车辆服务上的“响应丢失—客户端超时—服务重启—同ID重试”组合语义，不代表物理车载以太网、真实门锁执行器、断电恢复或量产SOME/IP栈。

## C++运行时性能矩阵

类型化`vehicle.telemetry/Upload`服务用于测量运行时数据路径。矩阵包含100/500/1000 Hz三档聚合频率、64/256/1024 B三档载荷和1/4/8个并发节点，共27个Release构建单元；每个单元创建独立运行时，避免历史幂等缓存影响后续结果。

Windows 11 / MSVC `/O2`本地实测：3600/3600调用成功，最低吞吐达成率95.63%；所有单元中最差p50/p95/p99分别为181.9/326.5/389.0微秒，最高进程CPU 85.56%，最高常驻内存7.58 MiB。最差p99来自1000 Hz、1024 B、8节点单元。原始27格数据见`benchmarks/windows-runtime-performance-matrix-v1.json`，方法见[性能矩阵说明](docs/PERFORMANCE_MATRIX.md)。

这些数字仅覆盖进程内C++运行时的请求构造、语义指纹、准入、Handler调用、终态缓存和Trace，不代表CAN、SocketCAN、UDP、SOME/IP、ECU或真实车辆网络时延，也不构成硬实时保证。

## 版本化记录与确定性回放

`data/replay/tasks_v1.jsonl`冻结300个唯一任务ID，来源为200个独立四节点系统场景：第一代包含全部200个场景，第二代将前100个场景作为独立任务再次记录。每个任务内嵌完整输入、源场景ID、源数据集SHA-256和代次；因此项目明确表述为“300个任务、200个独立源场景”，不包装成300个不同车辆场景。

基线阶段和回放阶段分别启动全新的C++网关进程，记录有序返回码、接受状态、CAN ID、Trace ID和完整终态快照，并生成每任务SHA-256。Windows实测两阶段共提交2600帧，600/600次预期契约校验通过，300/300任务摘要一致、差异0项，基线与回放聚合SHA-256均为`385722a0d868c5014f2ee0f7326c5e5e547eac9569981d9a05dfc9c7a5c700b1`。原始报告见`benchmarks/windows-replay-v1.json`，方法见[回放管线说明](docs/REPLAY_PIPELINE.md)。

当前是JSONL任务集与JSON报告，还没有MCAP/Parquet、对象存储、PostgreSQL元数据或分布式Worker；不能描述成量产自动驾驶数据闭环平台。

### 租约Worker、心跳与Fencing

本地调度原型使用SQLite WAL保存任务状态、Lease Owner、过期时间、Attempt、单调递增Fencing Token和结果摘要。4个Python Worker分别持有独立C++网关进程，通过原子事务领取任务，执行前后续租，并以Token提交结果；过期Worker的迟到提交不能覆盖新Owner。

Windows实测300个任务全部完成、300次预期契约全部通过，最终状态为300 completed、0 leased、0 pending；4个Worker分别提交68/78/85/69个任务，聚合输出SHA-256与单Worker基线保持一致。另有60组租约控制实验：60次心跳成功、60次提前接管被阻止、60次过期接管提交成功、60次旧Token提交被Fence、60次同结果提交幂等重放、60次不同结果冲突拒绝。原始报告见`benchmarks/windows-scheduled-replay-v1.json`，协议见[回放调度说明](docs/REPLAY_SCHEDULER.md)。

SQLite和单机线程协调器只是可复现元数据原型，不代表PostgreSQL高可用、多主调度、跨主机时钟或网络分区能力。

## SOME/IP有界跨进程链路

第二条通信链路使用两个独立C++进程，在localhost UDP上完成`vehicle.state/GetSnapshot`请求/响应。实现遵循AUTOSAR SOME/IP的16字节基础头布局，覆盖Service/Method ID、Length、Client/Session ID、协议与接口版本、消息类型和返回码；同时校验精确报文长度、4 KiB项目载荷上限、请求身份关联和错误服务/方法。

Windows本地实测中，服务端收到请求后实际进入`VehicleServiceRuntime`，读取由VCU、TPMS、BCM和T-Box状态组成的车辆快照，再向客户端返回123.45 km/h、75%电量和四组信号新鲜状态。原始报告见`benchmarks/windows-someip-loopback-v1.json`。

这不是完整AUTOSAR SOME/IP栈：当前没有Service Discovery、事件组、订阅、SOME/IP-TP、E2E Profile、安全机制或vsomeip互操作验证。报告中的总耗时还包含进程启动和Python编排，不能作为SOME/IP网络时延。完整边界和载荷Schema见[有界SOME/IP说明](docs/SOMEIP_SUBSET.md)。

## 快速运行

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure

python -m venv .venv
python -m pip install -e '.[dev]'
python -m pytest
```

本机Visual Studio工具链也可以直接执行：

```powershell
.\scripts\build_windows.ps1
$env:PYTHONPATH = "python"
python scripts\run_frozen_scenarios.py `
  --gateway-bin .runtime\windows-build\sdv_gateway_cli.exe `
  --output artifacts\windows-scenario-report.json
```

该脚本同时运行`sdv_service_runtime_demo.exe`和车辆服务运行时测试。

跨进程SOME/IP有界链路：

```powershell
$env:PYTHONPATH = "python"
python scripts\run_someip_loopback.py `
  --server .runtime\windows-build\sdv_someip_vehicle_server.exe `
  --client .runtime\windows-build\sdv_someip_vehicle_client.exe `
  --output artifacts\someip-loopback.json
```

## 冻结场景v1

`data/scenarios/v1.jsonl`固定包含220个场景、300次帧提交，覆盖正常状态、正常胎压、错误DLC、未知ID、重复帧、陈旧帧、同时间戳冲突和越界信号。它验证的是“Python控制面 -> 本地进程管道 -> C++网关”的系统契约；报告中的延迟明确不是CAN总线时延。

Windows 11 / MSVC本地实测：220/220场景通过、300次帧提交、失败0次；进程管道加C++校验的p50/p95/p99分别为0.0354/0.0760/0.0959 ms。该数字不代表CAN、ECU或真实车辆网络时延。原始结果见 `benchmarks/windows-scenario-v1.json`。

首次端到端运行发现并修复了`QUIT`确认与子进程退出之间的清理竞争，见 [工程日志](docs/ENGINEERING_LOG.md)。

## 四节点系统集成

VCU、TPMS、BCM和T-Box分别作为独立Python进程运行，C++网关作为第五个进程持续接收。Windows本地实验采用2代节点、每代每节点30帧：

- 240/240帧被C++网关接收；
- 每个节点60帧，代际分布120/120；
- 第二代节点产生4次进程重启/重新接入；
- 240条Trace ID全部唯一；
- 最终VCU、TPMS、BCM、T-Box四组状态均未过期；
- p50/p95/p99控制路径延迟为0.0388/0.0863/0.1442 ms。

该实验的多进程队列是仿真控制通道，不是CAN或车载以太网；真实Linux传输由单独的SocketCAN链路验证。原始结果见 `benchmarks/windows-multinode-v1.json`，结构和边界见 [架构说明](docs/ARCHITECTURE.md)。

### 四节点冻结系统场景v1

`data/scenarios/system_v1.jsonl`固定包含200个跨节点系统场景和900次帧提交：100个健康场景，另100个分别注入VCU重复帧、TPMS陈旧帧、BCM错误DLC、T-Box信号越界、未知CAN ID和BCM同时间戳冲突。每个场景均先建立VCU、TPMS、BCM和T-Box四组基线状态，再验证故障返回码、四组信号新鲜度以及拒绝故障后基线状态不被污染。

Windows 11 / MSVC本地实测：200/200场景通过、900次帧提交、失败0次、200/200终态保持验证通过。网关返回800次`APPLIED`，并按预期拒绝20次重复帧、20次陈旧帧、15次错误DLC、15次越界信号、15次时间戳冲突和15次未知ID。控制路径p50/p95/p99为0.0284/0.0698/0.1048 ms；该数字仅覆盖本地进程管道与C++网关处理，不代表CAN、ECU或真实车辆网络时延。冻结集SHA-256为`5ba71dc14acd662810eee9fab9a85afa4e0a98a13b013da3b947701175100e82`，原始结果见 `benchmarks/windows-system-scenarios-v1.json`。

首次执行该冻结集时出现40个假失败：C++ JSON中的十进制速度与Python计算所得二进制浮点值被直接做精确相等比较。修复后浮点信号采用`1e-9`绝对误差，离散状态仍保持精确比较；数据集、期望返回码和网关实现均未改动。完整分析见 [工程日志](docs/ENGINEERING_LOG.md)。

Linux SocketCAN 验收：

```bash
sudo modprobe vcan
sudo ip link add dev vcan0 type vcan
sudo ip link set vcan0 up
python scripts/verify_vcan.py --interface vcan0 --output artifacts/linux-vcan.json
```

## 从现有资产复用了什么

复用的是本人已有项目中已经验证过的工程方法，而不是复制仓库改名：

- `edge-device-verification-lab`：C++资源生命周期、状态边界、有界输入和坏路径优先设计；
- `telecom-network-incident-agent`：Linux SocketCAN验收方法、受限DBC解析、新鲜度和迟到数据语义；
- `agent-runtime-quality-platform`：后续场景Worker的lease、heartbeat、fencing、幂等和Trace方法。

汽车项目重新定义车辆帧、网关状态、故障模型、场景集、指标和验收结论。完整竞争力门槛见 [PROJECT_ACCEPTANCE.md](PROJECT_ACCEPTANCE.md)。

