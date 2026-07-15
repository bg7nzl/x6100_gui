# FT8 文件日志（RX / TX / DNF）

源码：`src/ft8/ft8_log.c`、`ft8_log.h`。
输出：`/mnt/ft8_logs/x6100_log_YYYYMMDD_HHMMSS.txt`。
分支：`bg7nzl/ft8-log-hook`。

## 背景

真机调试和赛后复盘需要一份和 QSO 引擎解耦的事实日志：每个时隙听到了什么、实际发了什么、Auto DNF 什么时候下的陷波。它不替代 ADIF 通联日志。

## 工作方式

日志是个旁路观察者，只记录，不改 `tx_msg`，也不碰引擎决策。接线用直接调用的 `ft8_log_on_init` / `cleanup` / `rx_msg` / `slot_end` / `pre_tx`。几个约定：

- TX 记的是快照文本：`pre_tx` 在 dialog 做完 `tx_text` 快照之后才调用，避免记到随后被改写的缓冲。
- 目录不可写就静默跳过：先写一个 `.test_write` 探测，写不了也不拖垮 FT8 会话。
- RX 按 slot 聚合：入队去重，slot_end 时刷盘，带上 slot 时间和基频。

## 实现

### 生命周期

| 调用 | 行为 |
|------|------|
| `ft8_log_on_init` | 准备；真正 open 可延迟到首次可写 |
| `ft8_log_on_rx_msg` | 入队（上限约 512），同文去重 |
| `ft8_log_on_slot_end` | 写 RX 块、分隔 |
| `ft8_log_on_pre_tx(info, tx_text)` | 写 TX 行；`CQ_` 显示成空格形式 |
| `ft8_log_dnf(slot_start, center_hz, delta_db)` | Auto DNF apply 时 |
| `ft8_log_on_cleanup` | 关文件、清队列 |

### pre_tx 顺序（集成不变量）

```text
snapshot tx_text → ft8_autodnf_on_pre_tx → ft8_log_on_pre_tx(info, tx_text)
```

### 路径与权限

- 目录 `/mnt/ft8_logs`，一般在数据分区。
- 文件名带本地时间戳。
- 刷机或挂载后要保证目录存在且可写。

## 与其它模块

- 不依赖 AutoSel；`DNF` 行由 Auto DNF 调用写入。
- 和系统级的 [feature-tx-log.md](feature-tx-log.md)（`/mnt/tx_logs`）不是一回事：本模块记 FT8 协议层，tx_log 记射频 / PTT 诊断。

## 开发注意

- 没做快照前，别把可变的 `tx_msg.msg` 指针直接塞进长期缓冲。
- 保持“打不开文件也不报错、不炸 UI”的策略，除非产品明确要求提示。
