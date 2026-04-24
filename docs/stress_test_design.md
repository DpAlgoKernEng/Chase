# Chase HTTP Server 24小时压力测试设计文档

## 1. 概述

本文档描述 Chase HTTP Server 24小时压力测试的设计方案，包括测试目标、负载模式、监控策略、结果报告模板以及运行说明。

### 1.1 测试目标

- **稳定性验证**: 验证服务器在长时间高负载下的稳定运行能力
- **内存泄漏检测**: 检测是否存在内存泄漏导致的内存持续增长
- **崩溃恢复测试**: 测试服务器崩溃后的自动重启机制
- **性能退化分析**: 分析长时间运行后性能是否退化
- **极限负载验证**: 验证服务器在峰值负载下的表现

### 1.2 测试时长

默认测试时长为 **24小时**，可根据需要调整（通过脚本参数）。

## 2. 测试负载模式

### 2.1 低负载模式

| 参数 | 值 | 说明 |
|------|-----|------|
| 线程数 | 1 | wrk 测试线程 |
| 连接数 | 100 | 并发连接 |
| 每周期时长 | 10秒 | 单次测试时长 |
| 目标吞吐量 | ~1k req/s | 预期吞吐 |

**测试端点**:
- `/` (主页)
- `/api` (API接口)
- `/health` (健康检查)

### 2.2 中负载模式

| 参数 | 值 | 说明 |
|------|-----|------|
| 线程数 | 4 | wrk 测试线程 |
| 连接数 | 500 | 并发连接 |
| 每周期时长 | 30秒 | 单次测试时长 |
| 目标吞吐量 | ~5k req/s | 预期吞吐 |

**测试端点**:
- `/` (主页)
- `/api` (API接口)
- `/static/test_1k.html` (1KB静态文件)

### 2.3 高负载模式

| 参数 | 值 | 说明 |
|------|-----|------|
| 线程数 | 8 | wrk 测试线程 |
| 连接数 | 1000 | 并发连接 |
| 每周期时长 | 60秒 | 单次测试时长 |
| 目标吞吐量 | ~10k req/s | 预期吞吐 |

**测试端点**:
- `/` (主页)
- `/api` (API接口)
- `/static/test_10k.html` (10KB静态文件)

### 2.4 峰值负载模式

| 参数 | 值 | 说明 |
|------|-----|------|
| 线程数 | 16 | wrk 测试线程 |
| 连接数 | 10000 | 并发连接（极限） |
| 每周期时长 | 30秒 | 短时爆发测试 |
| 目标吞吐量 | 变化 | 验证极限能力 |

**测试端点**:
- `/` (主页)

### 2.5 负载周期安排

每个完整负载周期包含:
1. 低负载测试（3个端点，各10秒） = ~30秒
2. 中负载测试（3个端点，各30秒） = ~90秒
3. 高负载测试（3个端点，各60秒） = ~180秒
4. 峰值负载测试（1个端点，30秒） = ~30秒

**单周期总时长**: ~330秒（约5.5分钟）
**24小时总周期数**: ~260个周期

## 3. 监控脚本

### 3.1 CPU 使用率监控

监控脚本每5秒采样一次系统CPU使用率。

**监控指标**:
| 指标 | 说明 |
|------|------|
| cpu_total | 总CPU使用率 (%) |
| cpu_user | 用户态CPU使用率 (%) |
| cpu_system | 系统态CPU使用率 (%) |
| load_avg_1 | 1分钟平均负载 |
| load_avg_5 | 5分钟平均负载 |
| load_avg_15 | 15分钟平均负载 |

**输出文件**: `stress_test_results/<timestamp>/monitor/cpu_usage.csv`

**格式**:
```csv
timestamp,cpu_total,cpu_user,cpu_system,load_avg_1,load_avg_5,load_avg_15
2026-04-24_10:00:00,45.2,30.5,14.7,2.5,3.2,3.8
```

### 3.2 内存使用监控

监控脚本每10秒采样一次服务器进程内存使用。

**监控指标**:
| 指标 | 说明 |
|------|------|
| pid | 进程ID |
| rss_mb | 实际物理内存使用 (MB) |
| vms_mb | 虚拟内存大小 (MB) |
| cpu_percent | 进程CPU使用率 (%) |
| worker_count | Worker子进程数量 |

**输出文件**: `stress_test_results/<timestamp>/monitor/memory_usage.csv`

**格式**:
```csv
timestamp,pid,rss_mb,vms_mb,cpu_percent,worker_count
2026-04-24_10:00:00,12345,128,512,35.2,4
```

### 3.3 Worker 进程状态监控

通过 `pgrep -P <master_pid>` 统计 Worker 进程数量，验证：
- Worker 进程数量是否稳定（应为配置的 Worker 数量）
- Worker 进程是否异常退出

### 3.4 崩溃检测与重启计数

**崩溃检测机制**:
- 每1秒检查 Master 进程是否存活 (`kill -0 <pid>`)
- 如果进程消失，记录崩溃事件并自动重启

**崩溃重启策略**:
| 参数 | 值 |
|------|-----|
| 最大崩溃次数 | 100次 |
| 重启延迟 | 2秒 |
| 超过最大崩溃次数后 | 终止测试 |

**输出文件**:
- `stress_test_results/<timestamp>/monitor/crash_events.log` - 崩溃事件日志
- `stress_test_results/<timestamp>/server_status.csv` - 服务器状态记录

## 4. 结果报告模板

### 4.1 小时报告模板

每小时生成一次中间报告，包含：
1. 服务器运行状态
2. 崩溃次数统计
3. 最近1小时内存/CPU数据
4. 最近1小时吞吐量数据

**文件**: `stress_test_results/<timestamp>/reports/hourly_<n>.md`

```markdown
# 压力测试小时报告 - 第 N 小时

**生成时间**: YYYY-MM-DD HH:MM:SS

## 服务器状态

- 运行状态: 正常运行/异常
- 崩溃次数: X
- PID: XXXXX

## 内存使用

timestamp,pid,rss_mb,vms_mb,cpu_percent,worker_count
...

## CPU 使用

timestamp,cpu_total,cpu_user,cpu_system,load_avg_1,load_avg_5,load_avg_15
...

## 测试吞吐量

timestamp,test_name,threads,connections,duration,req_per_sec,latency_avg,errors
...
```

### 4.2 最终报告模板

测试完成后生成综合报告。

**文件**: `stress_test_results/<timestamp>/reports/final_report.md`

```markdown
# Chase HTTP Server 24小时压力测试最终报告

## 测试概览

| 项目 | 值 |
|------|-----|
| 开始时间 | YYYY-MM-DD HH:MM:SS |
| 结束时间 | YYYY-MM-DD HH:MM:SS |
| 实际运行时长 | XXXX 秒 (约 XX.XX 小时) |
| 目标时长 | 24 小时 |
| 服务器类型 | production/minimal |
| Worker 数量 | 4 |
| 监听端口 | 8080 |

## 崩溃统计

| 指标 | 值 |
|------|-----|
| 总崩溃次数 | X |
| 崩溃率 | X.XX 次/小时 |

## 内存使用统计

| 指标 | 值 |
|------|-----|
| 最大 RSS | XXX MB |
| 平均 RSS | XXX MB |
| 最小 RSS | XXX MB |
| 内存增长 | XXX MB |

## CPU 使用统计

| 指标 | 值 |
|------|-----|
| 最大 CPU | XX.X% |
| 平均 CPU | XX.X% |

## 吞吐量统计

| 负载模式 | 平均吞吐量 (req/s) |
|----------|-------------------|
| 低负载 (100 conn) | XXXX |
| 中负载 (500 conn) | XXXX |
| 高负载 (1000 conn) | XXXX |
| 峰值负载 (10000 conn) | XXXX |

**总 Socket 错误**: X

## 稳定性评估

### **结论**: 服务器在测试期间运行稳定，无崩溃发生。

（或）

### **结论**: 服务器在测试期间发生 X 次崩溃（崩溃率: X.XX 次/小时）。

建议检查崩溃日志进行分析:
- 服务端日志: stress_test_logs/<timestamp>/server_stdout.log
- 崩溃事件: stress_test_results/<timestamp>/monitor/crash_events.log

## 文件列表

| 文件类型 | 路径 |
|----------|------|
| 服务端日志 | ... |
| 错误日志 | ... |
| 内存监控数据 | ... |
| CPU 监控数据 | ... |
| wrk 测试结果 | ... |
| 小时报告 | ... |

---
*报告生成时间: YYYY-MM-DD HH:MM:SS*
```

### 4.3 关键指标说明

| 指标 | 计算方式 | 健康阈值 |
|------|----------|----------|
| 最大 RSS | 监控数据最大值 | < 500 MB |
| 内存增长 | 结束 RSS - 开始 RSS | < 50 MB (无泄漏) |
| 崩溃次数 | 崩溃事件计数 | = 0 (理想) |
| 错误率 | Socket 错误 / 总请求 | < 0.1% |
| 吞吐量稳定性 | 最大吞吐 / 最小吞吐 | > 80% |

## 5. 运行说明

### 5.1 环境要求

**必需工具**:
| 工具 | 版本要求 | 安装方法 |
|------|----------|----------|
| wrk | >= 4.0 | `brew install wrk` (macOS) 或 `make` (Linux) |
| bash | >= 3.0 | 系统自带 |
| ps | 任意 | 系统自带 |

**可选工具**:
| 工具 | 用途 |
|------|------|
| curl | HTTPS 测试 |
| top | CPU 监控增强 |

**服务器要求**:
- 已编译的 Chase 服务器 (`build/examples/production_server`)
- 端口 8080 可用（HTTP）
- 端口 8443 可用（HTTPS，可选）

### 5.2 启动步骤

#### 步骤 1: 编译服务器

```bash
cd /Users/ninebot/code/open/dpalgokerneng/self/Chase
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

#### 步骤 2: 运行压力测试脚本

**默认 24小时测试**:
```bash
./scripts/stress_test_24h.sh
```

**自定义参数**:
```bash
./scripts/stress_test_24h.sh <测试时长小时> <端口> <HTTPS端口> <服务器类型> <Worker数量>
```

**示例**:
```bash
# 1小时测试
./scripts/stress_test_24h.sh 1

# 自定义端口和 Worker 数量
./scripts/stress_test_24h.sh 24 9090 9443 production 8

# 使用 minimal 服务器
./scripts/stress_test_24h.sh 24 8080 8443 minimal 1
```

#### 步骤 3: 监控测试进度

测试运行时，可通过以下方式监控：

**查看实时日志**:
```bash
# 查看服务器输出
tail -f stress_test_logs/<timestamp>/server_stdout.log

# 查看监控数据
tail -f stress_test_results/<timestamp>/monitor/memory_usage.csv
```

**查看小时报告**:
```bash
cat stress_test_results/<timestamp>/reports/hourly_*.md
```

#### 步骤 4: 等待测试完成

测试完成后，脚本会自动生成最终报告。

#### 步骤 5: 分析结果

```bash
# 查看最终报告
cat stress_test_results/<timestamp>/reports/final_report.md

# 分析内存趋势（使用脚本）
# TODO: 可创建 analyze_memory_trend.sh 脚本
```

### 5.3 提前终止测试

按 `Ctrl+C` 发送 SIGINT 信号，脚本会：
1. 停止所有监控进程
2. 停止服务器
3. 生成截至当前的最终报告
4. 清理资源后退出

### 5.4 结果分析指南

#### 内存泄漏检测

**方法 1: 观察 RSS 曲线**
- 如果 RSS 随时间持续增长，可能存在内存泄漏
- 正常情况下，RSS 应在一定范围内波动

**方法 2: 计算内存增长**
- 查看最终报告中的 "内存增长" 指标
- 如果增长 > 50 MB，建议进一步分析

**方法 3: 绘制内存趋势图**
```bash
# 使用 gnuplot 或其他工具绘制
# 示例: 提取内存数据
awk -F',' 'NR>1 {print $1, $3}' stress_test_results/*/monitor/memory_usage.csv > memory_data.txt
```

#### 崩溃分析

如果发生崩溃：
1. 检查 `server_stdout.log` 获取崩溃前日志
2. 检查 `crash_events.log` 了解崩溃时间
3. 如果 macOS，可查看系统崩溃报告
4. 如果 Linux，检查 `/var/log/syslog` 或 `dmesg`

#### 性能退化分析

**方法**: 比较不同时间段的吞吐量

```bash
# 提取前8小时和后8小时的吞吐量
grep "homepage" stress_test_results/*/wrk_summary.csv | head -50  # 前8小时
grep "homepage" stress_test_results/*/wrk_summary.csv | tail -50  # 后8小时
```

如果后8小时吞吐量显著低于前8小时，可能存在性能退化。

## 6. 输出文件结构

测试完成后，目录结构如下：

```
stress_test_logs/
└── <timestamp>/
    ├── server_stdout.log      # 服务器标准输出
    ├── errors.log             # 错误日志
    └── warnings.log           # 警告日志

stress_test_results/
└── <timestamp>/
    ├── test_config.json       # 测试配置
    ├── server_status.csv      # 服务器状态记录
    ├── wrk_summary.csv        # wrk 测试汇总
    ├── wrk/
    │   ├── low_homepage_*.txt
    │   ├── medium_api_*.txt
    │   ├── high_static_*.txt
    │   └── ...
    ├── monitor/
    │   ├── memory_usage.csv   # 内存监控数据
    │   ├── cpu_usage.csv      # CPU 监控数据
    │   └── crash_events.log   # 崩溃事件日志
    └── reports/
        ├── hourly_1.md
        ├── hourly_2.md
        ├── ...
        └── final_report.md    # 最终报告
```

## 7. 安全注意事项

### 7.1 端口占用

确保测试端口未被其他服务占用：
```bash
# 检查端口占用
lsof -i :8080  # macOS/Linux
netstat -tuln | grep 8080  # Linux
```

### 7.2 系统资源

24小时压力测试会持续消耗系统资源：
- 确保系统有足够内存（建议 > 2GB 可用）
- 关闭其他高负载应用
- 监控系统整体负载

### 7.3 文件系统空间

测试会生成大量日志文件：
- 每小时约 1MB 监控数据
- 24小时约 24MB 监控数据
- wrk 测试结果约 100KB/测试
- 总计约 50MB 文件

确保磁盘空间充足。

### 7.4 测试隔离

建议在专用测试环境运行：
- 不要在生产服务器上运行
- 考虑使用 Docker 容器隔离

## 8. 故障排除

### 8.1 wrk 连接错误

如果 wrk 测试频繁报错：
- 检查服务器是否正常运行
- 检查端口是否正确
- 检查防火墙规则
- 降低并发连接数

### 8.2 服务器无法启动

如果服务器启动失败：
- 检查编译是否成功
- 检查端口权限（是否需要 root）
- 查看 `server_stdout.log` 错误信息

### 8.3 监控脚本异常

如果监控数据缺失：
- 检查 `ps` 命令是否可用
- 检查进程是否仍在运行
- 查看 `errors.log` 和 `warnings.log`

## 9. 扩展建议

### 9.1 可选扩展

1. **增加 HTTPS 测试**: 使用 OpenSSL 或 curl 测试 SSL 连接
2. **增加 API 测试场景**: 测试不同 API 响应大小
3. **增加 WebSocket 测试**: 如果支持 WebSocket
4. **增加数据库连接测试**: 如果涉及数据库操作

### 9.2 结果可视化

建议使用以下工具可视化结果：
- **gnuplot**: 绘制内存/CPU 趋势图
- **matplotlib**: Python 绘图库
- **Grafana**: 实时监控仪表盘

### 9.3 自动化集成

可将脚本集成到 CI/CD 流程：
- 每周自动运行1小时压力测试
- 发布前运行24小时稳定性测试
- 失败时自动发送通知

---

*文档版本: 1.0*
*创建时间: 2026-04-24*
*作者: minghui.liu*