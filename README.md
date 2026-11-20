# SDV Communication Integration Lab

面向智能汽车平台软件、车载通信、系统集成与仿真工具链岗位的可复现实验项目。

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

