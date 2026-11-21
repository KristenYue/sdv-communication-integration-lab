# 回放数据存储契约

## 目标与边界

本层把版本化JSONL回放任务规范化成可分析、可校验的列式数据，而不是把JSON文件简单改后缀。当前证据覆盖本地Parquet/Zstandard、SHA-256内容寻址文件和SQLite对象元数据事务；PostgreSQL 16.16已经验证任务调度元数据，但尚未承载远端对象生命周期。不声称已运行S3或MinIO。

```text
300个版本化ReplayTask
  ├─ tasks.parquet             300行任务索引
  ├─ frames.parquet           1300行有序车辆帧与期望返回码
  └─ expected_states.parquet   300行规范化车辆终态
          │
          ├─ Zstandard压缩
          ├─ 文件字节SHA-256
          └─ sha256/<前2位>/<完整摘要>.parquet
                         │
                         └─ SQLite逻辑名、摘要、大小、行数、Schema版本与URI
```

300个任务来自200个独立源场景：第一代覆盖200个场景，第二代重复记录前100个场景。因此帧表共1300行，而不是把重复任务表述为新的独立车辆场景。

## 数据契约

- `tasks`：任务ID、源场景ID、源数据集摘要、代次、类别、故障标志和快照时刻；
- `frames`：任务ID、任务内顺序、CAN ID、DLC、固定8字节Payload、总线时间、Trace ID和期望返回码；
- `expected_states`：速度、电量、车门、四轮胎压、BCM状态和T-Box状态等类型化列；
- 所有字段均采用显式Arrow Schema且不可为空，不依赖运行时类型猜测；
- Parquet物理文件摘要标识本次编码制品，JSONL规范化输入摘要标识逻辑数据集。两类摘要不能混用。

## 写入与读回语义

1. 表先写入对象根目录下的临时Parquet文件；
2. 计算完整文件SHA-256并原子移动到内容寻址路径；
3. 在SQLite事务中登记逻辑名称及不可变元数据；
4. 同一逻辑名称和同一元数据可幂等重放；同名不同内容拒绝覆盖；
5. 读回先检查URI未逃逸对象根目录，再校验文件摘要、物理Schema、行数及表内容。

当前未实现跨进程对象上传事务、垃圾回收、对象版本保留、远端加密和访问控制。SQLite也不是高可用元数据服务。

## 迁移接口

`migrations/0001_postgresql_replay_metadata.sql`固定了PostgreSQL表和约束，可承载数据集、不可变对象以及带Lease/Fencing的任务状态。PostgreSQL 16.16 CI已经完成数据集与任务调度元数据验证；`replay_objects`与S3/MinIO的端到端对象生命周期仍待实现。详细状态见[PostgreSQL回放后端](POSTGRES_REPLAY.md)。

本地复现：

```bash
python -m pip install -e '.[dev,data]'
python scripts/build_replay_storage.py --output artifacts/replay-storage-v1.json
```
