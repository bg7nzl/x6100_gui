# 自动发起的操作员确认（TX2 confirm）

源码：`src/ft8/qso.cpp` 里的 `is_tx2_text`、`confirmed_*`、`pending_*`、`ftx_qso_commit_pending` / `ftx_qso_abort_pending`；`src/dialog_ft8.c` 里的 `confirm_*`、`ft8_consume_ptt`、`ft8_confirm_consume_rotary`。
测试：`tests/test_ft8_qso.cpp`（"Auto TX2 confirm gate" 等用例）。

## 背景

Full / Pre 档会主动发起通联——回别人的 CQ，或 tail-end。发起的第一报都是 Tx2（`PEER 我的呼号 网格`）。没有这道确认，机器可以在无人值守时自行跟新台开 QSO；加上它之后，对每个远端呼号，本次 FT8 会话内的**第一条** Tx2 必须由操作员按键放行，机器只负责把机会摆到眼前。

约束对象只有发起。对方已经叫我的应答（报告、RR73、73）不经过这道门；手动点击本身就是授权，也不经过。

## 行为

1. 自动选台选中一个没确认过的呼号、且待发文本是 Tx2 → 引擎不发射、不记退避计数，屏幕提示 `TX2 <呼号>? PTT/VFO=OK`。
2. 在本时隙的发射窗口内（与尾对齐同一窗口：FT8 5 秒 / FT4 1.5 秒）按 **PTT（长按短按都行）** 或转 **主 VFO 旋钮** = 同意，这一隙立刻发出 Tx2。
3. 不按到超时 = 本轮作罢，什么也不发；下次再解到同一机会会**再次**弹出提示（不存在"问过一次就不问了"）。
4. 同意过的呼号本会话内不再问，后续重试直接走正常退避（见 [feature-auto-tx-backoff.md](feature-auto-tx-backoff.md)）。手动点击过的台视同已同意。
5. 会话边界 = 退出 FT8、FT4/FT8 切换、换波段（这些都会 `ftx_qso_reset`，确认名单清零）。

确认期间只有 PTT 和主 VFO 被征用：VFO 的这一下转动不改频率（确认后立刻恢复调谐），其余按键旋钮照常工作。**FT8 界面打开期间 PTT 全程不再键控发射机**——数字模式里它本来就没用，正好独占为确认键。

## 实现要点

### 为什么按文本形态判定，不按 order

回 CQ（order 2）和 tail-end（order 0）发出的文本一模一样：`PEER me GRID4`。门控关心的是"这是不是一条发起报文"，文本形态是唯一可靠的判据，order 继续留给档位过滤和退避。判定：3 个 token、中间是本机呼号、第三个通过网格校验——**并显式排除 `RR73`/`RRR`**，因为 `qth_grid_check("RR73")` 恰好为真（RR73 被协议选中就因为它形似网格），不排除会把自己的 Tx5 拦下来。

### 确认前引擎零痕迹

选中未确认呼号时只做一件事：把候选快照进 `pending`，`response` 返回 `need_confirm`（action 仍是 RX）。**不** `blacklist_bump`、不更新 `last_call`、不 `emit_candidate`（所以不会提前记 `qso_start` / `rst_sent`）。超时 `abort` 只清 pending——退避预算一点不烧，peers 账本也没脏数据。同意时 `commit_pending` 补齐这三件事再发射，与没有这道门时的路径完全等价。

`decide()` 入口无条件作废遗留 pending：一条 pending 绝不跨决策周期存活。

### 双时钟

- UI 侧：弹窗时记墙钟 deadline（`get_time() + tx_max_delay`），按键回调只比这个，不碰 worker 局部量；
- worker 侧：`on_tick` 在发射窗口过后兜底 abort + 撤提示。

两边基准都是发射时隙起点，与 oneshot 过窗即弃同一套常量（`MAX_TX_START_DELAY` / `_FT4`）。commit 与 abort 的竞态由 `engine_mutex` + `pending_valid` 收敛，谁先拿到锁谁算数。

### 确认名单

`confirmed_calls[256]` 环形表，按呼号（大小写不敏感）记。`clear_decision_state`（拨 Auto / Auto Mode / CQ / 点击）**保留**它——用户重启决策不等于要对点过头的台再问一遍；只有 `reset` 清空。

### 输入接线

- PTT：`main_screen_keypad_cb` 的 `KEYPAD_PTT` case 顶部调 `ft8_consume_ptt(state)`——FT8 dialog 打开恒 true（整体屏蔽键控），pending 时 PRESS 边沿触发确认（PRESS 先于长/短按判定到达，天然覆盖两种按法）。
- 主 VFO：`main_screen_rotary_cb` 顶部调 `ft8_confirm_consume_rotary()`——仅 pending 时 true，吞掉本次 `freq_shift`。
- Remote 注入的 `PTT` / rotary 命令与实体输入走同一事件队列，技术上等效同意。但这是设计点而非漏洞：**提示只画在机身屏幕上**——不推送给 remote、也没有提示音，远端只能靠反复刷截图碰运气，赶不上几秒的确认窗。结果就是这道门实质上要求人守在机器旁盯着屏幕，远程全自动被机制本身阻断，与"FT8 禁止无人值守自动发射"对齐。不要为 remote 加确认通知或代确认接口。

另有两道与弹窗互斥的门：Free MSG 占坑的时隙、TX Call 拨在 Disabled 时，确认机会直接作罢不弹窗（后者在按键侧也再查一次，防止窗口期内刚拨下的暂停被 `apply_qso_response` 重新打开）。

## 改这块时注意

- 先动 `tests/test_ft8_qso.cpp`。现有用例覆盖：确认前退避计数为零、commit 后同呼号不再问、abort 后每次都重新问、tail-end 同样被拦（证明与 order 无关）、`RR73` 碰撞回归、`clear_decision_state` / `reset` 的名单语义、遗留 pending 被 `decide()` 作废。
- 门控条件动过之后，务必重跑 `PEER me RR73` 负例——那是最容易踩回去的坑。
- 不要给 Res 档加这道门：Res 不发起，是给手动 TX CQ 当应答档的。
