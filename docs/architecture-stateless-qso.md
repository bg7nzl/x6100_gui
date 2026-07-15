# Stateless QSO 引擎

源码：`src/ft8/qso.h`、`src/ft8/qso.cpp`。
测试：`tests/test_ft8_qso.cpp`。

## 背景

传统 FT8 客户端会维护“我在和谁通、进行到第几步”的状态机。嵌入式场景下，超时、交叉 QSO、对方重发上一步，都会让状态和空中现实脱节，各种边界情况组合起来很难覆盖全。本项目里还有一层现实原因：旧的单文件状态机把选台判定和收发流程缠在一起，插入点和判定逻辑没法并行维护，加功能很别扭。

这个引擎改用 ft8d 的思路：QSO 进度不记在状态机里，而是从当前收到的消息推断出来。核心是一个纯函数：

```text
compute_one(一条解码) → 至多一条回复（带 order）
```

输入相同，输出就相同。粘性、blacklist、手动目标这些，是函数外面的一小块外部状态，只由两个公开入口去改。

## 几个设计取向

### 外壳不对引擎做假设

`dialog_ft8.c` 不假设引擎有什么功能：

- 回复只针对紧接着的那一个指定奇偶 slot，发一次之后等引擎下一次响应。
- 外壳里没有“阶段倒数”，也没有显式的 QSO 结束判定。
- 过窗即弃（引擎路径 `oneshot=true`）：错过起始窗口就直接丢，下一个 slot end 引擎重新决策。

### 每个时隙决策一次

`on_slot_end` 无条件调用 `ftx_qso_on_decoded_messages`，空 slot 也调。空 slot 本身是一种信号（对方沉默，或者在和别人通），是 73 收尾和粘性重试的前提。

### 点击处理

`ftx_qso_on_user_message`（仅手动点击入口，Auto 不走这里）：

1. 解析点击行；若是**别人通联中途**（GRID / ±nn / R±nn / R-grid，且 `!to_me`），先改写成 `CQ <call_de>`——对方网格已在此前 slot end 的 `analyze_rx` 进过 peers，合成 CQ 不必再带 grid；
2. 设手动目标（呼号加奇偶锁定）；
3. 种进粘性槽（存的是评估用文本，因此中途行会以 `CQ <call>` 形式重试）；
4. 对评估文本跑 `compute_one`，与点 CQ / 点别人 RR73·73（tail-end）一样发出 Tx2。

叫你的消息不改写，仍按阶梯回一步。叫你的 73 无话可回（`action=RX`）。

能不能赶上本隙由 dialog 的 tick 判断，引擎不管墙钟。

### 手动模式的粘性重试

手动模式下：

- 对方有真实消息，就永远回复，粘性计数清零；
- 对方沉默，就粘性重试，连续 5 次之后放手。

RR73 是特例：回 73 不进粘性槽，避免对方已经停发、这边还在连发 73。

## 实现要点

### 数据流

```text
表格点击 ──► ftx_qso_on_user_message ──►（可选改写为 CQ）──► compute_one ─┐
                                                                          ├─► response
slot end  ──► ftx_qso_on_decoded_messages ──► decide() ──────────────────┘
                                                          │
                                                          ▼
                                              apply_qso_response（dialog）
                                                          │
                                              on_tick：奇偶 + 宽限期 → TX
```

两个入口用 `engine_mutex` 互斥（UI 线程和 worker）。

### 公开 API（摘要）

| 接口 | 作用 |
|------|------|
| `ftx_qso_on_decoded_messages` | slot end；`n` 可以为 0 |
| `ftx_qso_on_user_message` | 点击（别人中途行先改写成 `CQ <call>`） |
| `ftx_qso_parse_rx_text` | 解析（UI 高亮也用它） |
| `ftx_qso_flush_complete` | 弹出报告齐全、但没收 / 发 73 的 QSO |
| `ftx_qso_reset` | 会话开始时清掉全部内部状态 |
| `ftx_qso_clear_decision_state` | 清 target / sticky / blacklist / last_call，peers 保留 |

`response.action` 是 `RX` 或 `TX`。`save` 和 `qso` 与 action 相互独立（可以 RX 的同时入库）。

### compute_one（日常 FT8）

| 收到 | 回复 | order |
|------|------|-------|
| CQ … CALL [GRID] | CALL me grid4 | 2 |
| me CALL GRID | CALL me ±nn | 3 |
| me CALL ±nn | CALL me R±nn | 4 |
| me CALL R±nn | CALL me RR73 | 5 |
| me CALL RR73/RRR | CALL me 73 | 6 |
| me CALL 73 | 不回 | — |
| 别人的 RR73/73（tail-end） | CALL me grid4 | 0 |

NA VHF 档见 [feature-na-vhf-processor.md](feature-na-vhf-processor.md)（交换 grid，少一步报告）。

### 内部状态（私有）

| 状态 | 用途 |
|------|------|
| target + odd | 手动锁定 |
| sticky ≤5 | 手动重试；RR73/73 不入槽 |
| last_call | 自动选台粘滞 |
| blacklist[64] | 自动发起（order≤2）的重试退避，按 TX 文本计数；见 [feature-auto-tx-backoff.md](feature-auto-tx-backoff.md) |
| peers[256] LRU | 按呼号攒 grid / 报告 / 时间 |

### Auto / Auto Mode

外壳的旋钮映射到 `ftx_qso_context_t` 的 `auto_level` 和 `sel`（SNR / Dist / Rnd / Grid）。自动路径：`compute_one` 全表 → blacklist → 取 order 最大 → last_call → 按模式 tie-break。

autosel 值得单说一句。它的起源很现实：机内界面选台远不如电脑上 WSJT-X 用鼠标点得快，稍一迟疑就错过时机，只能等下一个同奇偶周期——而 6m 开窗往往转瞬即逝，一等，可能整个联系就错过了。自动选台就是来补这点手速的。

至于实现，在 legacy 里它是一整套独立的自动选台逻辑；到了这个模型里，“主动应答别人的 CQ” 不过是 `compute_one` 表里一个较小的 order（发起类响应本就排在延续中的 QSO 之后），选台就是在没有更高 order 可做时，从这些低 order 候选里按 Auto Mode 挑一个。功能一样，实现却简洁了太多。

拨 CQ / Auto / Auto Mode、手动点击这些操作都会 `ftx_qso_clear_decision_state()`，免得旧决策污染。

## 与外壳的契约（集成不变量）

1. slot end 无条件进引擎。
2. `response.save` 不受 Free MSG 让位影响。
3. 引擎 TX：`repeats=1`、`oneshot=true`、`force_free_text=false`。
4. `worker_init` 后 `ftx_qso_reset`；析构路径先 `flush` 未完成的 QSO。
5. C++ 模块不要直接 `#include` 含 C99 `complex` 的 dialog / audio 头，走 getter 或干净的 C API。

## 改引擎时注意

- 先补 `tests/test_ft8_qso.cpp`，本地 `run_tests.sh`。
- 不要为“以后可能用”预置没用到的字段。
- 往 upstream 提 PR 时保持一 PR 一功能、无 hook 注册表。
