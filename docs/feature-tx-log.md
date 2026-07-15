# TX 诊断日志（tx-log）

源码：`src/tx_log.c`、`tx_log.h`。
输出：`/mnt/tx_logs/tx_log_YYYY-MM-DD_HH-MM-SS.txt`。
分支：`bg7nzl/tx-log`。

## 背景

排查 PTT、载波、模式切换一类问题（包括 OEM 寄存器“同值不写”导致的泄漏）时，需要一份带时间戳、频率、模式、功率，以及 Sple 影子寄存器的异步日志，而且不能在射频热路径上同步写盘阻塞。

## 工作方式

- 生产者-消费者：调用方 `tx_log_event` 入队，后台线程写文件。
- Sple shadow：记录 modem / iptt / atu / swrscan 等位，对照 `x6100_control` 的低层定义。
- 可选 backtrace：条目可以带一小段栈，方便定位“是谁触发的 TX”。
- 系统级：不绑 FT8 dialog，`main` 里在 `params` 之后 `tx_log_init()`。

## 实现

```c
void tx_log_init(void);
void tx_log_event(const char *event, int32_t freq_hz, int32_t mode,
                  float pwr, const char *detail);
```

- 目录 `/mnt/tx_logs`，mkdir 失败就放弃。
- 队列用 mutex + cond；写文件另有一把 file mutex。
- Sple 格式化成 `modem=` `iptt=` `atu_tune=` `swrscan=`。

## 与 FT8 Log 的区别

| | FT8 Log | tx-log |
|--|---------|--------|
| 路径 | `/mnt/ft8_logs` | `/mnt/tx_logs` |
| 内容 | 解码文 / TX 文 / DNF | PTT 事件 / 寄存器影子 |
| 生命周期 | FT8 dialog | 进程级 |

## 开发注意

- 热路径只入队，不要 `fprintf`。
- 栈深度有上限（`TX_LOG_STACK_DEPTH`），别在中断上下文里乱用。
- 分析载波泄漏时，结合 radio 层“强制制造寄存器差异”那类修复（见项目 memory / bug 笔记）。
