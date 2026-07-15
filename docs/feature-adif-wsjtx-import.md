# ADIF / WSJT-X / LoTW 导入

源码：`src/adif.c`、`adif.h`；启动导入在 `main.c` → `qso_log_import_adif("/mnt/incoming_log.adi")`。
分支：`bg7nzl/adif-wsjtx-import`。
配套：`x6100_webserver` 的 Logbook（分支 `bg7nzl`，见 [companion-webserver.md](companion-webserver.md)）。

## 背景

导入其实是为 autosel（自动呼叫）服务的：把已经联系过的台灌进 worked 库，自动模式就不会再去叫老朋友。LoTW 之所以单独拎出来，是因为本地以为“联系过了”的，对方不一定确认过；LoTW 一般被当作金标准，所以提供了清空本地库、再导入 LoTW QSL 的做法。

问题在于，WSJT-X 导出和 LoTW 下载的 ADIF，在字段名、行尾、记录是否跨行上，都和本机写出的紧凑 ADIF 不一样，旧解析器会漏记，甚至整个文件解析失败。

## 工作方式

- 读得宽、写得严：导入时容忍差异，本机写出仍用固定字段集。
- 按 EOR 组记录：LoTW 常把一条 QSO 拆成多行，要一路拼到 `<EOR>`。
- 认字段别名：比如 `STATION_CALLSIGN`；时间允许 `TIME_ON` 的 HHMMSS。
- 和引擎的 comment 共存：`adif_add_qso(log, qso, comment)` 保留竞赛 / 备注列，import 修复里不砍引擎签名。

## 实现要点

| 能力 | 作用 |
|------|------|
| `line_has_eor` / `field_is` | 稳地识别记录边界和字段名 |
| 多行拼接 `rec` | 读到 EOR 前一直追加，不然 LoTW 几乎解析不出 |
| `STATION_CALLSIGN` | 映射到本机呼号一侧 |
| `TIME_ON` HHMMSS | 不只是 HHMM |
| 启动导入路径 | `/mnt/incoming_log.adi` |

写出侧仍写标准 `<EOR>\r\n` 和常用字段（含 `STATION_CALLSIGN`）。

## 与 webserver 的分工

gui 不实现 HTTP。用户路径是：

1. 浏览器打开 /logbook，Upload 一个 `.adi`；
2. Web 把它写到 `/mnt/incoming_log.adi`，并停 / 启 GUI；
3. gui 启动时 `qso_log_import_adif` 把 worked 灌进 `/mnt/qso_log.db`。

本机通联 ADIF 在 `/mnt/ft_log.adi`，Web 可以列表、下载、删除。展示用的解析在 web 的 `adif_parse.py`，导入的宽松规则在 gui 的 `adif.c`，两边职责不同。

## 开发注意

- 改 `adif_add_qso` 签名时，引擎保存路径和测试要一起改。
- 回归用真实的 WSJT-X / LoTW 样例文件（注意隐私，样例要脱敏）。
- 导入失败要能诊断（返回条数 / 日志），别静默全灭。
