# Auto DNF

源码：`src/ft8/auto_dnf.c`、`auto_dnf.h`；接线函数 `ft8_autodnf_on_*`；配置项 `ft8_auto_dnf`。
分支：`bg7nzl/ft8-auto-dnf-hook`。

## 背景

起因是本地常有一个强台。X6100 基带后面的 ADC 只有 8 bit，动态范围有限，一个强信号很容易就把它打爆，其它信号全解不出来。手动 DNF 陷波能压住这种干扰，可要调 DNF 就得退出 FT8，而通联当中往往根本退不出来。

好在 FT8 是分 slot 的、纠错又强：可以在每个时隙前段拿开头那段信号当尺子，量出主峰，一旦超过阈值就自动把陷波对准它，到 slot 末尾再解除。实测非常好用。当然，要是那个强台连基带都打爆了，也就没辙了。

## 工作方式

Auto DNF 只做射频辅助，不参与 QSO 选台。几个要点：

- 扫描按时隙对齐，用 PSD 帧的时间戳（对应音频时刻）判断窗口，而不是看当前钟点。
- 检测放在 worker 线程（`auto_dnf_on_psd` 里不碰 LVGL），下陷波这类 UI 操作经 `scheduler_put` 回到主线程。
- 撤陷波用墙钟 one-shot timer，在 `slot_end - clear_time_sec` 触发，不靠 PSD 帧来驱动。
- 进 FT8 dialog 时记下用户原来的 DNF 设置，退出时还原，不永久改用户偏好。
- 本机 TX 时隙直接跳过；`pre_tx` 阶段再清一次 notch，避免陷波影响己方发射。

## 算法

头文件注释里的模型：

1. 在时隙约 0.25–0.75 s（可由 `ft8_tuning_t` 配置）采集 PSD。
2. 时间维做 max-pool，取第 25 百分位当底噪。
3. 峰值高出底噪超过 `min_delta_db` 时，把陷波中心对准峰频。
4. Overlay 画一条蓝线标峰，附 ΔdB 标签，z-order 压在解码表之下。

## 接线

| API | 线程 | 作用 |
|-----|------|------|
| `ft8_autodnf_on_init` / `set_waterfall` | UI | 建 ctx、overlay |
| `ft8_autodnf_on_psd` | worker | 检测，必要时 schedule apply |
| `ft8_autodnf_on_pre_tx` | TX 路径 | 清 notch |
| `ft8_autodnf_on_cleanup` / `restore_entry` | UI | 还原进 dialog 前的 DNF |
| `ft8_log_dnf` | apply 时 | 可选记入 FT8 文件日志 |

按钮在 Page3 的 **Auto DNF**，开关写 `cfg.ft8_auto_dnf`。

## 与其它模块

- FT8 Log：下陷波时写一条 `DNF` 记录（需 Log 已 init、目录可写）。
- 引擎：无直接依赖。
- TailAlign、Free MSG：无共享状态。

## 开发注意

- `on_psd` 里不要直接调 LVGL。
- C++ 代码不要 include 整份带 C99 complex 的 audio 头，用现成 getter。
- 调参改 `ft8_tuning_t` 或 cfg，别把魔法数散落到各处。
