# 架构总览（master / Stateless 集成）

## 1. 总体思路

X6100 上的 FT8 要在嵌入式 UI 线程、解码 worker、射频控制之间稳定协作。`master` 现在的架构是一路演进收敛下来的结果：

1. 无状态 QSO 内核（`qso.cpp`），决策可测、可重放。
2. `dialog_ft8.c` 外壳管时隙、TX 门控、CQ、UI，对引擎不做功能假设。
3. Module extension point 加直接调用，各 feature 在固定生命周期点挂接，没有 `ft8_hooks.h` / `register_hooks`。
4. feature 之间相互正交，按管线位置叠加，不交叉状态。

### 一路是怎么走到这里的

上游（gdyuldin）最早的 FT8 就是一个巨大的单文件状态机，想动哪儿，都得先在盘根错节的状态流转里找到能插进去的位置，很难维护。那时候我照着这个 legacy 版本，把几乎全部功能（连 autosel 也算上）一口气写完，一次性给作者提了个数千行的大 PR，作者的回复是：太大了，没法 review。

于是只好回过头来拆。把那一大坨拆成一小块一小块分批提，作者也就陆续接受、合并了进去。再往后，为了让各个功能的插入点更清晰，我又做了一版基于 hook 的实现；可作者觉得没必要上 hook，直接函数调用就够了，我就把 hook 去掉、改回直接调用，也就是今天的 Module extension point。这之后上游那边再没合并、也没回复，集成这条线便只能在本仓自己往前走。

真正让我下决心重写的，是 hook 那套虽然连 autosel 都跑得起来，可“往哪儿插”和“怎么判定选台”这两件事根本没法并行维护，想再多加点功能就处处别扭。索性把整个 QSO 引擎推倒重写，改成无状态的流程，让 QSO 判定和实际收发消息彻底分开，之后再改就干净多了。NA VHF 就是最好的例子：这要搁在旧状态机上，估计得大动干戈；放到无状态引擎里，不过是多认一种消息类型而已。

## 2. 分层

```text
┌─────────────────────────────────────────────────────────────┐
│  UI / LVGL（dialog_ft8.c，主线程）                            │
│  按钮、表格点击、textarea、cfg Subject、TX CQ / Auto 旋钮     │
└───────────────────────────┬─────────────────────────────────┘
                            │ apply_qso_response / on_tick
┌───────────────────────────▼─────────────────────────────────┐
│  QSO 引擎（ft8/qso.cpp）                                     │
│  compute_one + MANUAL/AUTO 选择；peers 账本；NA VHF 等档位   │
└───────────────────────────┬─────────────────────────────────┘
                            │
┌───────────────────────────▼─────────────────────────────────┐
│  音频 / 解码 / 发射                                          │
│  audio_worker · worker · tx_worker · gfsk                    │
└───────────────────────────┬─────────────────────────────────┘
                            │
┌───────────────────────────▼─────────────────────────────────┐
│  旁路 feature（不改引擎决策）                                 │
│  Auto DNF(PSD) · FT8 Log · TailAlign(门控/掐头) · Remote ·   │
│  tx_log · ADIF 导入 · 10W 钳位                               │
└─────────────────────────────────────────────────────────────┘
```

唯一会和引擎抢 TX 出口的是 Free MSG（共用 `tx_msg`，靠让位 guard 协调）。

## 3. Module extension point

约定是在 `dialog_ft8.c` 的生命周期里直接调用具名函数，空实现或真实实现由对应的 `.c` 提供。典型的挂接点：

| 时机 | 调用示例 |
|------|----------|
| dialog 构造 | `ft8_log_on_init` · `ft8_autodnf_on_init` |
| dialog 析构 | `ft8_log_on_cleanup` · `ft8_autodnf_on_cleanup` / `restore_entry` |
| 每条解码 | `ft8_log_on_rx_msg` |
| slot end | `ftx_qso_on_decoded_messages`（无条件）→ 再 `ft8_log_on_slot_end` |
| PSD 帧 | `ft8_autodnf_on_psd` |
| 即将 TX | 快照 `tx_text` → `ft8_autodnf_on_pre_tx` → `ft8_log_on_pre_tx(info, tx_text)` |
| Auto DNF 下陷波 | `ft8_log_dnf(...)` |

好处是接线集中、依赖单向：dialog 调 feature，feature 不反过来改引擎状态机。

## 4. TX 输出契约

引擎和 Free MSG / CQ 最后都落到 `tx_msg`：

| 来源 | `repeats` | `oneshot` | `force_free_text` |
|------|-----------|-----------|-------------------|
| 引擎 `apply_qso_response` | 1 | true（过窗即弃） | false |
| CQ `cq_rearm` | cfg `ft8_max_repeats` | false | false |
| Free MSG | 1 | false（可 defer 到下一同奇偶隙） | true |

`on_tick_cb` 里，奇偶匹配、且 `sec_since_slot_start < tx_max_delay` 才真正开 TX。`tx_max_delay` FT8 是 5.0 s、FT4 是 1.5 s（和 TailAlign、Free MSG 选隙同源）。

## 5. Free MSG 让位（§5.1）

```c
bool free_msg_pending = tx_msg.force_free_text && (tx_msg.msg[0] != '\0');
if ((response.action == FTX_QSO_ACTION_TX) && !free_msg_pending && ...)
    apply_qso_response(...);
else if ((cq_enabled != CQ_OFF) && !free_msg_pending && ...)
    cq_rearm();
```

引擎这时仍可以更新 peers、`save`，只是让出 TX 装填和 CQ 再装填。

## 6. 正交性（为什么能各自独立开发）

| Feature | 管线位置 | 与引擎 |
|---------|----------|--------|
| Auto DNF | 解码前 PSD | 无（本机 TX 隙跳过，pre_tx 清 notch） |
| TailAlign | TX 启动 / 波形 | 无 |
| FT8 Log | 旁路记事实 | 无 |
| Free MSG | 共用 `tx_msg` | 有让位 |
| Remote / tx-log / ADIF / 10W | 系统层 | 无 |

`feature/ft8-auto-sel-hook` 不合入：自动选台由引擎的 Auto / Auto Mode（SNR / Dist / Rnd / Grid）加 remove_worked 承担。

## 7. 分支模型（开发约束）

```text
origin/main (gdyuldin)
 └── bg7nzl/ft8-stateless     # 引擎 + hook 基建
      └── master              # 纯集成：merge feature + §4 按钮 + 补洞
```

- 业务改动在对应的 `bg7nzl/<feature>`（或上游 feature）上做，再合进 `master`。
- `master` 上不放没有 merge 依据的随手业务 patch（集成补洞除外）。
- gui 任务的 scope 默认只有 `x6100_gui`。

## 8. 按钮页终态（§4）

| 页 | 按钮 |
|----|------|
| 1 | Page · Free MSG · TX CQ · Auto · Auto Mode |
| 2 | Page · Show CQ all · FT4/FT8 · Hold freq · TX Call |
| 3 | Page · Force QSO save · CQ Modifier · Time Sync · Auto DNF |
| 4 | Page · Processor（含 NA VHF） |

## 9. 相关源码入口

| 区域 | 路径 |
|------|------|
| Dialog / 接线 | `src/dialog_ft8.c` |
| 引擎 | `src/ft8/qso.cpp` · `qso.h` |
| TX | `src/ft8/tx_worker.c` |
| 测试 | `tests/test_ft8_qso.cpp` · `run_tests.sh` |

## 10. 分发边界

gui 不是唯一交付物，同一镜像里还有 webserver（用户会用的浏览器台）和协议库等。

- 用户说明只维护 [user-manual.md](user-manual.md)（含 Web，不含库细节）。
- 开发契约见 [distribution-and-siblings.md](distribution-and-siblings.md)、[companion-webserver.md](companion-webserver.md)、[companion-ft8lib.md](companion-ft8lib.md)（最后这篇仅供开发）。
