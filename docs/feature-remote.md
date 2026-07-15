# Remote 控制（浏览器 / FIFO / 截屏）

源码（gui）：`src/remote_control.c`、`remote_control.h`、`src/remote_screen.c`。
源码（web）：`x6100_webserver` 的 `/remote`、`/api/remote_input`、`/api/remote_screen`。
分支：gui 用 `bg7nzl/remote`，web 见 `bg7nzl`（[companion-webserver.md](companion-webserver.md)）。

## 背景

电台可能装在天线侧或机柜里，操作员想在局域网用浏览器（或脚本）模拟面板按键和旋钮，还能看到当前屏幕，同时不去动 FT8 的决策逻辑。

## 工作方式

- 注入已有事件总线：Remote 只是转成 `EVENT_KEYPAD` / `EVENT_ROTARY` / `event_send_key`，和真键走同一条路径。
- FIFO 文本协议：`/tmp/x6100_remote_ctrl`，一行一条命令；Web 只是往 FIFO 写的前端。
- 按需截屏：Web 去 touch `/tmp/remote_screen.req`，gui 的 `remote_screen_tick` 写出 `/dev/shm/remote_screen.jpg`。
- init 放得靠后：`main.c` 在 keyboard 之后才 `remote_control_init()`；主循环里调 `remote_screen_tick()`。
- 和 QSO 引擎零耦合，不直接改 `tx_msg`。

## 协议

行格式（空白分隔，命令大小写不敏感）：

```text
KEY <NAME> <PRESS|RELEASE|LONG|LONG_RELEASE|CLICK>
KNOB <VFO|MFK|VOL> <delta>
KNOB_PRESS <MFK|VOL> <PRESS|CLICK>
```

`KEY` 名例子：`F1`…`F5`、`PTT`、`BAND_UP`、`MODE_SSB`、`ATU`、`AGC` 等（见 `keypad_keys[]`）。

`KNOB VFO` 分配 `int32_t` delta，用 `lv_event_send(..., EVENT_ROTARY, delta)`。注意别走会 double-free 的 `event_send` 包装（见源码注释）。

`KNOB MFK/VOL` 按步发左右键；VOL 要区分 SELECT / EDIT 状态。

任何命令都会顺带 `backlight_tick()`，免得远程操作时背光熄了被误当成死机。

## 实现要点

- 非阻塞读 FIFO，用 `REMOTE_LINE_MAX` 拼行缓冲。
- `remote_control_poll()` 由主循环调用。
- 未知键名直接忽略。

## 截屏路径

| 路径 | 角色 |
|------|------|
| `/tmp/remote_screen.req` | Web 请求戳 |
| `/dev/shm/remote_screen.jpg` | gui 写出，Web 拉取（没就绪返回 503） |

编码在设备上偏慢，Web 侧会短等一下，用户需要手动刷新画面。

## 开发注意

- 加面板键时，`keypad_keys[]` 和 web 的 `remote.html` 要一起改。
- 改 rotary 所有权语义时，真编码器和 remote 两边一起测。
- 产品说明和 OTA / Logbook 在同一站：见 [companion-webserver.md](companion-webserver.md)、[user-manual.md](user-manual.md)。
