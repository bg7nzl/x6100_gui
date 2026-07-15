# 兄弟组件：x6100_webserver（Web 配置台）

> 这些页面最终用户会用到，操作步骤写在 [user-manual.md](user-manual.md)；本文是实现说明。

源码：`x6100_webserver`，在分发镜像里是一个局域网 HTTP 服务，导航见 `views/base.html`。推荐分支 `bg7nzl`（含 Logbook、Remote、OTA、dmesg、Time、Files 等）。

## 1. 背景

面板小，文件又都在 DATA 分区（`/mnt`），所以需要用浏览器来完成一些事：

- 编辑波段 / 数字模式频率；
- 浏览、下载日志和录音；
- 对时；
- 远程面板加截屏；
- 通联 ADIF / worked 库的导入导出；
- 只更新 gui 二进制（OTA），不用整卡重刷。

gui 这边提供 FIFO、截屏、启动导入，产品化的 UI 在 webserver。

## 2. 几点取向

- 路径契约稳定：和 gui 共用 `/mnt/*`、`/tmp/x6100_remote_ctrl`、`/dev/shm/remote_screen.jpg`（见 [distribution-and-siblings.md](distribution-and-siblings.md)）。
- 改库先停 GUI：Logbook / OTA 通过 `_with_gui_stopped` 调 `S95gui`，免得和 gui 抢 sqlite / ADI / 二进制。
- 后端做薄：Bottle 加简单模板 / jQuery，复杂协议解析能省则省（Logbook 表用 `adif_parse.py` 只抽要展示的字段）。
- 和引擎解耦：Web 不做 QSO 决策，只管配置、文件、遥控输入。

## 3. 功能一览（用户可见路由）

| 路由 | 功能 |
|------|------|
| `/` | 总览说明 |
| `/bands` | 波段表编辑（含 Active 绑定上下键） |
| `/digital_modes` | FT8/FT4 等数字频率表 |
| `/files/` | DATA 分区文件浏览器（默认倾向 `/mnt`） |
| `/time` | 时区、手动校时、NTP |
| `/logbook` | ADIF 查看 / 下载 / 上传 / 删除；清空 qsolog |
| `/ota` | 上传 zip 替换 `/usr/sbin/x6100_gui` |
| `/remote` | 仿面板遥控加拉截屏 |
| `/dmesg` | 内核日志末尾约 200 行 |

### 3.1 Logbook

- 展示 `/mnt/ft_log.adi`（gui 写出的 FT8 通联）。
- Download / Delete 本地 ADI。
- Upload 会存成 `/mnt/incoming_log.adi`，重启 GUI 后 gui 用 `qso_log_import_adif` 灌进 worked。
- Reset qsolog DB 清空 `/mnt/qso_log.db` 里的 `qso_log` 表。

和 gui 的 [feature-adif-wsjtx-import.md](feature-adif-wsjtx-import.md) 配套：gui 管宽松解析，Web 管投递文件和运维按钮。

### 3.2 Remote

- 页面往 `/api/remote_input` POST JSON（key / knob / knob_press）。
- 后端写入 FIFO，gui 的 [feature-remote.md](feature-remote.md) 注入事件。
- `/api/remote_screen`：touch 请求文件，等 gui 写出 JPEG 再返回（超时返回 503，要再刷新）。

### 3.3 OTA（只换 GUI 二进制）

包格式（页面里也有说明）：

1. zip 里恰好一个成员；
2. 成员的文件名等于内容的小写 sha256（64 hex）；
3. 校验通过后写入 `/usr/sbin/x6100_gui` 并重启 GUI。

它不更新 webserver、内核、ft8lib 动态库，那些还得整镜像或走别的通道。上限约 32MB（`GUI_OTA_MAX_BYTES`）。

### 3.4 Time

NTP（`ntpdate`）、手动 `date -s`、symlink `/etc/localtime`。FT8 很依赖准确时钟，也可以用机内的 Time Sync。

### 3.5 Files / Bands / Digital modes / dmesg

- Files：浏览下载，下载前 `sync`；并发写 `params.db` 时拷贝可能偶发不一致（页面里已提示）。
- Bands / Digital modes：改 sqlite 参数库里的表（走 models API）。
- dmesg：BusyBox 友好，只取尾部行。

## 4. 实现要点（开发者）

| 模块 | 路径 |
|------|------|
| 路由 | `src/x6100_webserver/apps.py` |
| 路径常量 | `settings.py` |
| Logbook ADIF 展示解析 | `adif_parse.py`（不是 gui 的 `adif.c`） |
| 模板 | `views/*.html` |
| 部署辅助 | `deploy-views.sh`（开发机刷模板用） |

`FILEBROWSER_DEFAULT_START` 为空时，由 init 脚本把浏览根指到合适位置（注释里提醒：init 已经设成 `/mnt` 时别和配置打架）。

## 5. 和 gui 文档的分工

| 主题 | 以谁为主 |
|------|----------|
| FIFO 命令字、事件注入 | gui `feature-remote.md` |
| 网页布局、OTA 包格式、Logbook 按钮 | 本文 + `user-manual.md` |
| ADIF 字段宽松解析 | gui `feature-adif-wsjtx-import.md` |
| 上传后的导入时机 | 本文契约 + gui `main.c` 启动导入 |

## 6. 验收清单

- [ ] 浏览器打开电台 IP，导航九项都进得去。
- [ ] Remote 按键和截屏刷新正常。
- [ ] Logbook 上传 WSJT-X / LoTW ADI 后 worked 生效（重启 GUI 由 API 触发）。
- [ ] OTA 错误包（多文件 / 错哈希）被拒，正确包之后版本 / 行为更新。
- [ ] 校时后 FT8 解码对齐变好。
