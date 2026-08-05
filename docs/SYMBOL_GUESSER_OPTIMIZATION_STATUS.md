# 符号猜测器优化状态

## 仓库状态

- Windows worktree：`C:\Users\ReallocAll\Code\cpp\spark-symbol-opt`
- Linux worktree：`/home/ubuntu/spark-symbol-opt`
- 分支：`perf/symbol-guesser-optimization`
- 基础提交：`5d1994245d85f8242be54f25c631becd63d6f8da`（`v0.4.1`）
- `origin` fetch URL：`git@github.com:EndstoneMC/spark.git`
- `origin` push URL：`disabled://origin-read-only`
- `personal`：`git@github.com:ReallocAll/spark.git`
- personal 推送状态：FDE 实验提交前，尚未推送

## 二进制与 profile

### Linux BDS 1.26.33.1 真值输入

- IDA 数据库：`C:\Users\ReallocAll\Code\ida\l26.33.1\bedrock_server.i64`
- IDA 输入：`C:\Users\ReallocAll\Code\ida\l26.33.1\bedrock_server`
- SHA-256：`61995841f21baf9bfab96e0d9b0cb798501dcc9789dab68e496f3b8e3bc83375`
- GNU Build ID：输入 ELF 中不存在
- 基准 profile：`C:\Users\ReallocAll\Documents\sample.sparkprofile`
- 基准 profile SHA-256：`ce1c500b506e1771047a1fdb33c2d6406dbc3b21670d74e62efeaeb4a249e370`
- profile 模块 SHA-256 与 IDA 输入完全一致。
- profile 元数据：Minecraft `26.33`、Endstone `0.5.4.2.dev1901`、endstone-spark `0.4.1`、4 ms interval、2,402 ticks、120.036 s。
- workload：1 名在线玩家、170 个实体，不是纯空闲服务器数据。
- IDA MCP 已连接到完全匹配的数据库；所有查询均为只读定点分析。

### Linux 实际测试服务器

- 主机：`ubuntu@192.168.170.140`
- SSH 可用；使用代理前已验证远端现有 `127.0.0.1:7890` 能完成 HTTPS 请求。
- 精确的 BDS 1.26.33.1 二进制仍保存在 `/home/ubuntu/endstone/server/bedrock_server`，SHA-256 与上述真值一致。
- 活跃测试安装已由外部更新为 BDS `1.26.40.8`，Build ID `48609423`，SHA-256 `689e1ee7e23646c06c3908ef08e1f9635793e2c582d0043a6861be71d94aa034`。
- 1.26.40.8 用作额外跨版本运行验证；绝不把它的 RVA 与 1.26.33.1 IDA 数据库直接对应。
- 当前部署插件：`/home/ubuntu/endstone/bedrock_server/plugins/endstone_spark.so`
- 当前插件 SHA-256：`b1e5b0359ede78985c6c86c0483637d2f0bc33c644858012819c2410dbf70d84`
- 可回滚副本：
  - `plugins/backups/endstone_spark.pre-fde.6d07b98d.so`
  - `plugins/backups/endstone_spark.pre-rva-extension.8d88d371.so`

## 基线

### BDS 1.26.33.1 profile 全局基线

- 全线程 Self：`17,135.727 ms`
- 主模块唯一采样 PC：`4,615`
- exact FDE 机器函数身份：`2,272`（其中 `2,269` 个范围含 profile PC，另有 3 个 PC 位于 FDE 外的 PLT）
- high 标签函数：`493`
- `?` 标签函数：`2`
- 未标记函数：`1,774`
- 函数语义覆盖：`21.816%`
- 主模块 Self：high `23.272%`，`?` `0.027%`，总 useful `23.299%`
- 全线程 Self useful：`20.913%`
- 主模块 depth-weighted Total useful：`42.947%`
- 证据：`vtable` 903 个采样 PC / `3,554.806 ms` Self；`str` 28 / `24.605 ms`；`str?` 6 / `4.152 ms`
- 本基准没有 `rtti`、`thunk` 或 `vtable?` 标签。

### `Level::tick()` 子树基线

- 主模块 Self high/useful readable：`21.54%`
- 主模块 depth-weighted Total readable：`36.30%`
- Top 20/50/100 useful 解释率：`25% / 38% / 31%`
- 这些 Top-N 计数仍按旧 profile 中的原始 PC 分裂；旧格式没有独立的 sampled-PC/function-root 字段，因此 evaluator 会诚实地把归一覆盖报告为不可用，而不从标签倒推。

### 未修改 Windows 构建基线

- `conan install . --build=missing`：通过，冷缓存 `267.808 s`
- `cmake --preset conan-relwithdebinfo`：通过，`156.361 s`
- `cmake --build --preset conan-relwithdebinfo`：通过，`49/49`，`13.607 s`
- 初始 CTest：`3/3` 通过，`14.253 s`
- `python tests/test_release_changelog.py`：`5/5` 通过
- allocation-only、statistics-only 与 allocation benchmark 均通过
- 基准 DLL SHA-256：`667b0bcb1250bf62ff9b52c18d5cba1202c2292a4a18edbeed4b8fa0ce890971`
- 本地 clang-cl `22.1.3`；CI 仍以 clang-cl 20 为标准。

## 实验一：精确 Linux FDE 范围与函数身份归一

状态：**已接受，待提交并推送 personal。**

### 假设

逐项解析 CIE/FDE 的 `initial_location` 与 `address_range`，能够消除旧“延伸到下一个 unwind 起点”对 gap/PLT 的错误归属，并在不改变语义标签策略的前提下把采样 PC 归一为稳定机器函数身份。

### 实现

- 新增有界、平台无关的 CIE/FDE 解析器。
- 覆盖 DWARF pointer encoding、CIE 回指、record 长度、加法溢出、可执行映射、单调 header、duplicate、同起点冲突、传递 overlap、gap 与半开区间查询。
- 在 decoded `.eh_frame` 所在可读 `PT_LOAD` 内顺序扫描；只有完整扫描遇到零终止记录时，才接受未被 header 索引的 FDE。
- 新增 `GuessResult`、`GuessKind`、`Confidence`、`evidence_count` 与 `function_rva`，同时保留旧 label-only API。
- 只处理已经由模块范围证明属于主程序、且普通符号系统未解析的帧；真实符号优先，非主模块不归一也不猜测。
- profile 使用上游 viewer 会忽略的私有 protobuf 字段 `1001`（原始 sampled RVA）与 `1002`（验证后的 unwind root）。这样既不改变显示标签，也能在 evaluator 中查看 offset、测量归一率，并保留不同调用上下文。
- Linux profile 元数据新增索引时间、batch 时间、近似内存、FDE/range/gap/RTTI/string 统计。
- evaluator 支持全局与 `Level::tick()` 子树的 sampled-PC、function-root、Self、depth-weighted Total 与 Top-N 解释率。
- 将原有字符串唯一性检查的全 `.text` 逐字节循环改为等价的 `memchr` opcode 候选扫描，不改变标签结果。稳态 batch 从 `268,767 us` 降到 `92,345 us`。

### BDS 1.26.33.1 FDE 真值

- `.eh_frame_hdr` 表项：`504,514`
- header 中有效范围：`504,513`
- 拒绝 1 个 indexed zero-range FDE。
- 从安全终止的完整 `.eh_frame` 恢复 1 个同起点、未索引但有效的 FDE。
- 最终范围：`504,514`
- duplicate：`0`
- overlap：`0`
- unindexed recovered：`1`
- 内部 gap：`484,414`，总计 `4,044,628` bytes
- 旧 next-start 规则过度延伸 `484,415 / 504,514` 个范围（`96.02%`），错误吞入 `4,050,471` bytes padding 或非 FDE 代码。
- 精确范围覆盖基准 profile 的 `4,612 / 4,615` 个唯一主模块 PC；主模块 Self `99.8928%`，主模块 depth-weighted Total `99.99375%`。
- 3 个 FDE 外 PC 是真实 PLT：`pthread_mutex_unlock`、`memmove`、`pthread_self`，共 `16.485 ms` Self；不再归到最后一个 FDE 是正确行为。
- `0xc09a980..0xc0a1327` 包含 `0xc0a1118`、`0xc0a111b`，应归一为同一机器函数。
- root `0xb960780` 在旧 profile 中包含 21 个采样 PC，合计 `1,171.824 ms` Self。

### BDS 1.26.40.8 跨版本运行验证

- 最终 profile：`/home/ubuntu/endstone/bedrock_server/plugins/spark/profiles/profile-1785938294020.sparkprofile`
- 本地评估副本：`%TEMP%\spark-symbol-guesser-evaluation\linux-26.40-fde-final.sparkprofile`
- profile SHA-256：`54f3ee56f9b9ab6587d959d4d07833b3a9ea5123e764b42a687a92e799f80c70`
- profile 内 BDS SHA-256 与实际 1.26.40.8 可执行文件一致。
- function table entries/ranges：`529,113 / 529,113`
- `.eh_frame` records：`529,117`
- rejected/duplicate/overlap/unindexed：`1 / 0 / 0 / 1`
- gap：`508,488`，共 `4,263,678` bytes
- index initialization：`617,630 us`
- batch：`92,345 us`，输入 264 个 sampled functions
- approximate bytes：`42,088,748`（`40.14 MiB`）
- 全主模块：`404` 个原始采样 PC → `264` 个 function roots；unique-PC、Self、depth-weighted Total 归一覆盖均为 `100%`。
- `Level::tick()` 子树：`295` 个原始采样 PC → `220` 个 function roots；unique-PC 与 Self 归一覆盖均为 `100%`。
- 代表性聚合：root `0xaba5b80` 保留 14 个原始 offset，`0xc1153e0` 保留 13 个，`0x9a3e3c0` 保留 12 个。
- 原有 `vtable`、`str`、`str?` 标签仍存在；没有 conflicting RVA label。
- BDS 正常加载插件，多次完成 profiler start/stop/export；无崩溃、越界、malformed unwind 异常或可见卡顿。测试服务器当前保持运行。

### 测试命令与结果

- Windows `conan install . --build=missing`：通过
- Windows `cmake --preset conan-relwithdebinfo`：通过
- Windows `cmake --build --preset conan-relwithdebinfo`：通过
- Windows `ctest --test-dir build\RelWithDebInfo --output-on-failure`：`4/4` 通过（DWARF、evidence、Windows symbol guess、完整 selftest）
- Windows `python tests\test_release_changelog.py`：`5/5` 通过
- Windows `python tests\test_profile_evaluator.py`：`8/8` 通过
- Linux 初次完整构建：`47/47` 通过；最终增量构建通过
- Linux `ctest --test-dir build/RelWithDebInfo --output-on-failure`：`4/4` 通过
- Linux `python3 tests/test_profile_evaluator.py`：`8/8` 通过

### 接受结论

双平台构建和测试通过；真实服务器运行正常；index、batch 与内存均达到门槛；FDE gap 不再错误继承相邻函数；节点身份碎片显著减少；原始 PC 没有丢失；真实符号与非主模块优先级测试通过；没有加入任何会虚增语义覆盖的规则。因此接受本实验。

## 已验证的后续热点证据

- 1.26.33.1 的最高未知 root `0xb960780` 同时出现在 `EditorStructureBlockSource` 与 `BlockSource` 的相同 vtable slot 41；IDA RTTI 证明前者公开单继承后者。后续 RTTI 实验只有在运行时也能证明祖先关系时，才可输出高置信 `BlockSource::vfn[41]`。
- `0xc09a980` 是由高置信 `Level::_subTick()::$_3` wrapper 调用的大型 Dimension 遍历体；全局传播最多使用 `call?:`，不能伪装为精确方法名。
- 二分查找、unordered_map lookup、多类共享 no-op 等热点缺少身份信息，应保留 RVA。

## 下一实验

提交并显式推送本轮 FDE 实验后，实现严格、热点导向的 Linux thunk：只接受 canonical direct/adjustor jump、PLT/GOT thunk 和最多两层的安全链；必须从真实指令边界开始、检测循环、验证目标函数范围，不扫描或命名无关业务函数。
