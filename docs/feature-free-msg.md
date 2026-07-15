# Free MSG

源码：`dialog_ft8.c`（`free_msg_*`、`force_free_text`、让位逻辑），持久化在 `/mnt/ft8_freetext.txt`。
分支：`bg7nzl/ft8-free-msg-hook`。

## 背景

有时需要发一句标准 QSO 流程之外的话，又不想让 QSO 状态机去操心“下一步该发什么”。典型场景是标明自己在异地或便携发射（呼号要带后缀）、提示对方换频（QSY）这类短消息，长度不超过 13 个字符，走 FT8 free-text 编码。

顺带也改了一下 lib：有时只想发一句 “de <呼号>/<后缀>” 注明身份，若按标准消息编码，这种非标准呼号会走 hash 路径而不是明文，对方没有前文、对不上 hash 就解不出来；free-text 直接发明文，正好绕开这个问题。

## 工作方式

- 直接预定：按 OK 就写入 `tx_msg`，不检查“当前是否在通联中”。
- 关 CQ：预定时把 `cq_enabled` 设为 `CQ_OFF`，免得 CQ 和自由文本抢时隙。
- 可 defer：`oneshot = false`，本隙来不及就等下一个同奇偶时隙再发，不会因过窗丢掉。
- 让位：引擎和 CQ 都不能覆盖还没发出的 Free MSG（`free_msg_pending`）。
- 编码前校验：OK 之前先用 `ftx_message_encode_free` 试编码，过长直接拒。

旧 tip 曾是“TX busy 就拒绝”，现在 master 改成覆盖预定，与“直接预定”一致。

## 实现

### 用户路径

1. Page1 的 **Free MSG** 打开 textarea（上限 13）。
2. `ft8_freetext_load` 载入上次内容。
3. 按 OK：`sanitize` → 可选保存 → encode 校验 → `CQ_OFF` → 填 `tx_msg`：

```text
msg = clean
repeats = 1
force_free_text = true
tx_msg_oneshot = false
tx_time_slot = 下一可发奇偶（见下）
tx_enabled = true
```

### 选隙

和 TX 门控用同一套逻辑：

```c
float max_delay = (protocol == FT8) ? MAX_TX_START_DELAY : MAX_TX_START_DELAY_FT4;
// 5.0s / 1.5s
tx_time_slot = !get_time_slot(...);
if (time_since_slot_start < max_delay)
    tx_time_slot = !tx_time_slot;  // 还赶得上本隙就翻回本隙
```

### TX 路径

`tx_worker_run_with_config` 带上 `force_free_text=true`，`ftx_worker_generate_tx_samples` 就走 free-text 编码。

### 让位（slot end）

见 [architecture-overview.md](architecture-overview.md) §5。引擎这时仍可以 `save` peers。

### 清理 `force_free_text`

凡是会清空 `tx_msg.msg` 的路径都要顺手清掉 `force_free_text`（`qso_setting_changed`、oneshot 过窗丢弃、TX 完成后等），否则后面的标准消息会被误当成 free 编码发出去。

### Sanitize

转大写、过滤到合法字符集、截断到 `FT8_FREETEXT_MAX_LEN`（13）。

## 与引擎的边界

| 会做 | 不做 |
|------|------|
| 占用一次 TX 出口（可 defer） | 不进 `compute_one` |
| 暂停 CQ | 不改 peers 账本语义 |
| 让位 guard | 不做“多段自由文本队列” |

## 测试建议

- FT8 / FT4 分别在宽限期前后预定，确认选隙和门控一致。
- Free MSG pending 时点解码表，确认引擎不会冲掉预定文本。
- 发出后确认 `force_free_text` 已清，再点 CQ / Auto 行为正常。
