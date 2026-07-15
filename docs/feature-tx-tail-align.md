# TX Tail Align（尾对齐）

源码：`dialog_ft8.c`（`on_tick_cb` 门控、`sec_since_slot_start`）、`src/ft8/tx_worker.c`。
分支：`bg7nzl/ft8-tx-tail-align-hook`。

## 背景

FT8/FT4 接收端靠时隙内的符号对齐。本机如果在时隙里起步偏晚、还从波形头开始播完整 79 个音，尾部就会越过理想结束点，对方解码变差。更糟的是起步太晚还硬发，白白浪费功率、干扰别人。

TailAlign 做两件事：

1. 门控：超过协议允许的起步宽限期就本隙不发（引擎的 oneshot 直接丢弃、等下一轮决策；Free MSG 不是 oneshot，改为 defer）。
2. 掐头：已经起步但偏晚，就丢掉波形前面一段采样，让尾部仍落在 `FT8_TX_END_SEC`（约 14.5s）之前结束。

掐头之所以可行，靠的还是 FT8 本身的容错：开头少几个音，接收端多半仍能解出来，于是起步时间上就多出一点自由度。

## 几点约定

- 属于脚手架级别的修正，不再捡回 hook 时代那套“CQ mid-slot 特殊逻辑”；CQ 交给引擎时代的 `cq_rearm` 管。
- 门控、选隙、oneshot 丢弃共用同一个 `tx_max_delay`，免得 Free MSG、引擎、TX 各搞一套阈值。
- 通过 `ft8_tx_config_t.sec_since_slot_start` 传参，避免改一长串位置参数引发 merge 冲突。

## 实现

### 常量

| 符号 | 值 | 含义 |
|------|-----|------|
| `MAX_TX_START_DELAY` | 5.0 s | FT8 最晚起步 |
| `MAX_TX_START_DELAY_FT4` | 1.5 s | FT4 最晚起步 |
| `FT8_TX_END_SEC` | 14.5 s | 目标结束时刻（掐头的依据） |

### 门控（`on_tick_cb`）

```text
tx_max_delay = FT8 ? 5.0 : 1.5
if (sec_since_slot_start < tx_max_delay && tx_slot_pending)
    → 组装 ft8_tx_config_t，跑 tx_worker
if (oneshot && sec_since_slot_start >= tx_max_delay)
    → 丢弃 tx_msg，清 force_free_text
```

### 掐头（`tx_worker_run_with_config`）

生成完整采样之后：

```text
remain_sec = FT8_TX_END_SEC - sec_since_slot_start
若 remain 不够播完整波形 → 跳过波形头部，只播尾部能塞进 remain 的部分
```

同时可以把功率钳到 10W（见 [feature-power-limit.md](feature-power-limit.md)）。

## 与 Free MSG

Free MSG 选隙用的是同一个 `max_delay` 公式。要是写死成 `MAX_TX_START_DELAY`，FT4 就会出现“选了本隙却被门控挡下”的假回归。

## 开发注意

- 改宽限期要三处一起对齐：tick 门控、oneshot 丢弃、Free MSG 选隙。
- `sec_since_slot_start` 应从 slot 时钟推导，和 audio 路径一致。
- 别为 CQ 单独搞第二套延迟语义。
