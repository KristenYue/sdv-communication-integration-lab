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
```

这不是量产 ECU，也不声称支持完整 DBC、AUTOSAR 或真实车辆控制。当前边界是：

- C++20 实现确定性的虚拟车辆网关和故障注入器；
- Python 实现项目约束内的 DBC 解码与 Linux SocketCAN 验收；
- CI 同时验证 Windows/Linux 构建，并在 Linux 创建真实 `vcan0` 接口；
- Python场景控制面通过进程管道驱动独立C++网关，冻结集与结果均保存SHA-256；
- 所有性能、可靠性和恢复结论必须在冻结场景执行后才能写入简历。

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

