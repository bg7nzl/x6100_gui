# NA VHF / Processor

相关：`qso.cpp` 里的 contest / processor 档、`dialog_ft8.c` Page4 的 **Processor** 按钮。

## 背景

CQ WW VHF 这类北美 VHF 数字赛，交换的是呼号加 4 位 grid，不要求信号报告。比日常 FT8 少一步（不发 ±nn / R±nn），序列和 WSJT-X 的 “NA VHF Contest” 标准流程对齐。

## 工作方式

- 仍走同一套无状态内核：`compute_one` 按消息类型分支，不另起状态机。
- Processor 只切换引擎的行为档和日志数据源，不动 dialog 的 TX 脚手架。
- 竞赛的 worked / dupe 和生涯日志分开（竞赛专用表），不污染日常的 remove_worked。

## 标准空中序列

```text
CQ TEST K1ABC FN42
                      K1ABC W9XYZ EN37
W9XYZ K1ABC R FN42
                      K1ABC W9XYZ RRR/RR73
W9XYZ K1ABC 73          （可省）
```

和日常 FT8 的关键差别：应答 grid 用 `R <grid>`，不是 `±nn`。

## 实现要点

| 层 | 内容 |
|----|------|
| `ft8_lib`（必须匹配） | `R FN42` 编码；细节见 [companion-ft8lib.md](companion-ft8lib.md) |
| 引擎 | `ftx_msg_type_t` 含 R-grid 档；`FTX_QSO_PROC_NA_VHF` 等 processor |
| dialog | Page4 Processor 切换；NA VHF 期间 CQ Modifier 走会话内存（默认 `TEST`，不写 params）；回 Normal / 关 dialog 后继续用持久化的 params |
| 日志 | 竞赛 QSO 单独一张表；ADIF `adif_add_qso(..., comment)` 可带注释 |

分发时如果只更新 gui 没更新 ft8lib 包，就会出现“Processor 里有 NA VHF、但空中发不出合法 R-grid”。

CQ modifier 经 encode→decode 校验时，库会把 `CQ_TEST` 展开成带空格的显示形式；`cq_make_message` 要避免二次编码丢掉呼号或 grid（引擎 tip 已修）。

## 与 Auto Mode 的配合

Auto Mode 选 **Grid** 档加竞赛表数据源，会优先没通联过的新 grid（按乘数导向）。Rover 的 `/R` 呼号按原样比较，不做特殊剥壳。

## 开发注意

- 枚举顺序会影响手动 filter 的“阶段深度”排序，新消息类型插到哪必须和计划一致。
- 混模（对方还在发报告）时报告行可以跟随，但默认应答走 R-grid。
- 真机对打前先用 Catch2 加 `ft8_lib` golden vector 守住编码。
