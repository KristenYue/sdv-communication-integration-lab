# PostgreSQL回放调度后端

## 当前状态

仓库已经实现PostgreSQL后端与GitHub Actions service-container集成任务。本机没有Docker、`psql`或PostgreSQL服务，因此本页区分两种状态：

- 已在本机验证：Python接口、输入约束、SQLite兼容回归、300任务原有调度链路；
- 等待远端CI产物验证：PostgreSQL migration、8个并发连接/Worker、300个真实C++网关回放任务和60组真实服务端时钟租约实验。

在远端报告产生前，不把PostgreSQL写入已完成的简历数字。

## 一致性设计

PostgreSQL实现不接收Worker提供的租约时间作为事实来源。领取、续租、过期判断和提交均使用数据库`clock_timestamp()`，避免不同Worker主机时钟漂移直接决定任务所有权。

领取语句在一个事务内完成：

```sql
SELECT ...
FROM replay_jobs
WHERE status = 'pending' OR lease_expires_at <= clock_timestamp()
ORDER BY task_id
FOR UPDATE SKIP LOCKED
LIMIT 1;
```

同一事务随后写入Owner、Lease Expiry，并递增Attempt和Fencing Token。`SKIP LOCKED`允许多个数据库连接并行领取不同行，不把全表任务领取串行化。

提交前对任务行执行`FOR UPDATE`，并依次校验：

1. Dataset与Task存在；
2. Owner和Fencing Token仍属于提交者；
3. 已完成任务只能进行同摘要幂等重放，不同摘要返回冲突；
4. 未完成任务的租约必须仍由数据库时钟判定为有效。

旧Owner即使知道获胜结果，也会先因Token不匹配返回`FENCED`，不能冒充获胜Worker的幂等重放。

## 集成实验

CI中的PostgreSQL 16 service container使用全新数据库执行migration。8个Python Worker各自持有独立C++ Gateway进程，并且每次Store操作创建独立数据库连接。目标验收包括：

- 300/300任务完成且符合冻结契约；
- 输出聚合摘要与单Worker、SQLite Worker实验一致；
- 重新创建Store连接后仍能读取300个终态；
- 60次Heartbeat生效；
- 60次租约有效期内接管被阻止；
- 60次过期接管成功且Token递增；
- 60次旧Worker提交被Fence；
- 60次获胜结果幂等重放；
- 60次不同结果冲突被拒绝。

这里的“重新连接”只证明元数据跨客户端连接持久存在，不代表PostgreSQL进程重启、主从切换或灾难恢复。

## 尚未证明

- PostgreSQL服务重启与WAL恢复期间的Worker行为；
- 多主机Worker和真实网络分区；
- 主从复制、自动故障转移、备份恢复与高可用SLA；
- 长任务租约续期线程和协调器进程被强制终止后的持续接管；
- S3/MinIO对象与PostgreSQL元数据之间的跨系统事务。
