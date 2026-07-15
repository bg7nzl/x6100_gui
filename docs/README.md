# x6100_gui 文档索引

## 给最终用户

| 文档 | 内容 |
|------|------|
| [DISCLAIMER.md](DISCLAIMER.md) | 免责声明（侧载 / TF 卡，必读） |
| [user-manual.md](user-manual.md) | 机内 FT8 加浏览器（Logbook / Remote / OTA 等） |

先看免责声明，再看使用手册。浏览器功能来自镜像里的 webserver。

## 给开发者

本目录以 gui 为文档主入口，镜像里还有其它组件，契约见下。

### 分发与 Web（用户可见能力的实现侧）

| 文档 | 内容 |
|------|------|
| [distribution-and-siblings.md](distribution-and-siblings.md) | 镜像组成、路径 / IPC 契约 |
| [companion-webserver.md](companion-webserver.md) | Web 路由，Logbook / OTA / Remote 实现 |

### 协议库（用户不可见，仅开发）

| 文档 | 内容 |
|------|------|
| [companion-ft8lib.md](companion-ft8lib.md) | ft8_lib：R-grid 编码等，不写进用户手册 |

### gui 内核与 feature

| 文档 | 内容 |
|------|------|
| [architecture-overview.md](architecture-overview.md) | 分层、extension point、数据流 |
| [architecture-stateless-qso.md](architecture-stateless-qso.md) | 无状态 QSO 引擎 |
| [feature-auto-tx-backoff.md](feature-auto-tx-backoff.md) | 自动发起的重试退避（blacklist） |
| [feature-na-vhf-processor.md](feature-na-vhf-processor.md) | NA VHF / Processor |
| [feature-free-msg.md](feature-free-msg.md) | Free MSG |
| [feature-auto-dnf.md](feature-auto-dnf.md) | Auto DNF |
| [feature-tx-tail-align.md](feature-tx-tail-align.md) | TX 尾对齐 |
| [feature-ft8-log.md](feature-ft8-log.md) | FT8 文件日志 |
| [feature-remote.md](feature-remote.md) | Remote FIFO + 截屏 |
| [feature-tx-log.md](feature-tx-log.md) | TX 诊断日志 |
| [feature-adif-wsjtx-import.md](feature-adif-wsjtx-import.md) | ADIF 解析 |
| [feature-power-limit.md](feature-power-limit.md) | FT8 放开到 10W（解除 5W 限制） |
