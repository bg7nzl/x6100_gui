# 兄弟组件：ft8_lib（FT8/FT4 编解码库）

> 受众只是开发者，对最终用户不可见，别链进 [user-manual.md](user-manual.md)。

源码：`ft8_lib`，链进 gui 使用。下面只讲和 BG7NZL 分发相关、相对上游轻量库多出来的那部分和契约，不是完整的库手册。

上游的目标是一个嵌入式可用的 FT8/FT4 编解码库（见该仓 `README.md`）。本固件依赖的分支一般是 `bg7nzl`。

## 1. 为什么单独记（还是不给用户看）

NA VHF、Free MSG、CQ modifier、非标准呼号这些空中比特，是由库来生成和解析的。gui 只决定“发哪句”，能不能变成合法载荷取决于 ft8_lib。库缺能力时，开发侧看到的是“菜单有、空中不对”；用户手册只写菜单操作就够了。

## 2. 库侧的几点取向

- 和 WSJT-X 比特兼容：编码解码要对齐公开协议和 golden vector，竞赛格式尤其如此。
- 面向资源受限环境：本来是给 MCU / SDR 用的，gui 在 Linux ARM 上链接同一套 C API。
- 编解码对称：能解的格式尽量也能发（R-grid 以前“能解不能编”，已经在 bg7nzl 补齐）。

## 3. 分发相关的能力

### 3.1 R-grid（NA VHF 的关键）

代表 commit：`9878ec0` *Add R-grid message encoding*。

| 点 | 行为 |
|----|------|
| 问题 | 解码很早就支持 `ir=1 + grid`，但编码会把 `"R FN42"` 拆丢，或者 `packgrid` 没置 ir |
| 修复 | `ftx_message_encode`：第三个 token 是 `R`/`r` 且后面跟 grid，就拼成 `"R <grid>"` 再 `encode_std`；`packgrid("R …")` 置 `igrid4 \| 0x8000` |
| 验证 | `test/test.c` roundtrip，加 WSJT-X `ft8code` 的 77-bit / 79 tones 比对 |

gui 侧 [feature-na-vhf-processor.md](feature-na-vhf-processor.md) 的空中序列就靠这个能力。

### 3.2 CQ 修饰编码 `CQ_nnn` / `CQ_abcd`

支持 `CQ_TEST`、`CQ_DX` 这类形式的打包，配合 gui 的 `cq_make_message` / CQ Modifier。要注意库在解码显示时常把 modifier 展开成带空格的 `CQ TEST …`；gui 如果 encode→decode 再 encode，就得处理这个显示形式，否则会丢呼号或 grid（见项目里的 CQ modifier 修复）。

### 3.3 非标准呼号与 CQ 哈希

- 带前缀等非标准呼号的编解码路径。
- CQ 场景下别错误地走 hash（`Prevent hash for CQ with nonstandard calls` 等）。

这些会影响自动选台和表内显示，但不单独占一个 gui 菜单。

### 3.4 Free-text

`ftx_message_encode_free` 和对应解码，最多约 13 个字符、字母表受限。gui 的 Free MSG 在 OK 之前会调它做编码校验，库拒了就 UI 提示、不发射。

### 3.5 其它解码侧增强（间接影响体验）

| 方向 | 说明 |
|------|------|
| 增量解码 | 窗内更早出结果（worker 侧） |
| SNR / 候选搜索时间偏移 | 解码灵敏度和时间容差 |
| mute decoded signals | 库 / 示例侧的能力，接不接线以 gui 为准 |
| payload 零初始化 / n3 位修复 | 稳定性（`a2600c8`） |

## 4. gui 的调用面（概念）

```text
dialog / qso / tx_worker
    → ftx_message_encode(_std|_free|…) / decode
    → 音调生成（gfsk / worker）
```

公共头主要在库的 `ft8/message.h` 等；gui 不复制协议表，只负责组字符串。

## 5. 开发与验收

1. 要改协议相关行为，先在 ft8_lib 加测试（涉及比特就带上 WSJT-X 向量）。
2. 再改 gui 引擎的字符串规则。
3. 整镜像回归：NA VHF 对打、`R grid`、Free MSG、带 modifier 的 CQ。
4. 版本滞后的典型症状：gui 编译过了但空中格式错，或者链接时缺符号——检查 `ft8lib` 包是不是指向了正确的 local tip。

## 6. 相关文档

- [distribution-and-siblings.md](distribution-and-siblings.md)
- [feature-na-vhf-processor.md](feature-na-vhf-processor.md)
- [feature-free-msg.md](feature-free-msg.md)
