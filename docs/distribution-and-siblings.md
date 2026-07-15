# 分发包与兄弟仓库

`x6100_gui` 是操作和文档的主入口，但真正刷进 SD 卡的镜像里，除了 gui 还有另外几个组件。

对最终用户来说，会用机内 UI 和 Web 配置台就够了（见 [user-manual.md](user-manual.md)）；ft8_lib 这类协议库的变更对用户不可见，不要写进用户手册。对开发者来说，Web 和库的契约仍记在本目录，免得分发时漏组件。

## 1. 镜像里实际有什么

分发镜像一般包含这几个组件：

| 组件 | 源码 | 机上角色 |
|------|------|----------|
| **gui** | `x6100_gui`（本仓） | LVGL 主程序 `/usr/sbin/x6100_gui` |
| **ft8_lib** | `ft8_lib` | FT8/FT4 编解码库（gui 链接） |
| **webserver** | `x6100_webserver` | 局域网 Web 配置 / Remote / Logbook / OTA |
| **X6100Control** | `X6100Control` | 射频 / 寄存器控制库 |

本 `docs/` 以 gui 为叙事中心，但功能说明必须覆盖同一镜像里的 ft8lib 和 webserver 增量（见下面两篇）。Control 只在影响用户可感知行为时才点一下。

## 2. 推荐分支（分发时对齐）

| 组件 | 常用分支 | 说明 |
|------|----------|------|
| gui | `master`（Stateless 集成） | 本文档默认 |
| ft8_lib | `bg7nzl` | 含 R-grid 编码等；tip 见该仓 |
| webserver | `bg7nzl`（含 Logbook + OTA + Remote） | 旧文档写过 `feature/logbook`，能力已并入 `bg7nzl` |
| X6100Control | 与 gui / `main` API 匹配的 tip | 版本滞后会编不过，别在 gui 任务里手搓 stub |

刷机前确认这三个仓的 tip 与打出该镜像时用的版本一致。

## 3. 跨仓契约（路径与 IPC）

下面这些路径是 gui 和 webserver 之间的硬约定，改一侧就得改另一侧：

| 路径 | 生产者 | 消费者 | 用途 |
|------|--------|--------|------|
| `/tmp/x6100_remote_ctrl` | web `/api/remote_input` | gui `remote_control` | 按键 / 旋钮 FIFO |
| `/tmp/remote_screen.req` | web 截屏请求 | gui `remote_screen_tick` | 触发截屏 |
| `/dev/shm/remote_screen.jpg` | gui 截屏 | web `/api/remote_screen` | 远程画面 |
| `/mnt/incoming_log.adi` | web Logbook 上传 | gui 启动 `qso_log_import_adif` | 导入 worked |
| `/mnt/ft_log.adi` | gui FT8 ADIF 写出 | web Logbook 列表 / 下载 | 通联 ADIF |
| `/mnt/qso_log.db` | gui QSO DB | web「Reset qsolog」 | worked 库 |
| `/usr/sbin/x6100_gui` | 镜像 / OTA | web OTA 替换后 `S95gui` 重启 | 只更 gui 二进制 |

Logbook / OTA 写文件之前会先停 GUI、操作完再启（`/etc/init.d/S95gui`），避免和 sqlite / ADI 并发。

## 4. 协议层契约（gui 与 ft8_lib）

| 能力 | 库侧 | gui 侧依赖 |
|------|------|------------|
| 标准 / free-text / CQ_nnn 编码 | `ftx_message_encode*` | CQ、Free MSG、引擎组报 |
| `R <grid>` 编码 | `packgrid` ir 位 + encode 拼 token（`9878ec0`） | NA VHF Processor |
| 非标准呼号 / CQ 哈希策略 | pack28 / hash 规则 | 解码显示与选台 |
| 增量解码等 | decoder | audio / worker（间接） |

缺 R-grid 编码时，gui 就算有 NA VHF 逻辑，也发不出正确的 `R FN42` 类交换。

## 5. 文档地图

| 读者 | 文档 |
|------|------|
| 用户 | [user-manual.md](user-manual.md)（机内 + Web；不含 ft8lib） |
| 开发者 · Web | [companion-webserver.md](companion-webserver.md) |
| 开发者 · 库（用户不可见） | [companion-ft8lib.md](companion-ft8lib.md) |
| 开发者 · gui | `feature-*.md` / `architecture-*.md` |

## 6. 维护原则

- 分发说明写在 gui/docs：用户拿到的主说明就在这个仓。
- 实现细节还是以各仓源码为准；这里只记契约、入口、用户步骤，不复制整份上游 README。
- 兄弟仓行为变了，先改契约表和 user-manual，再改 feature 专篇。
