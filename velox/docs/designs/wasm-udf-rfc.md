# RFC: 在 Velox 进程内执行不可信 Wasm UDF

- **状态**：Draft
- **日期**：2026-10-05
- **范围**：Velox scalar UDF 与 aggregate UDF

## 摘要

Wasm UDF 的独特作用，是让 Velox 在查询进程内运行来源不受信任的函数代码，同时将代码的内存访问和宿主能力限制在明确的边界内。部署单位是一个自包含的 `.wasm` 文件：Rust SDK 将 SQL 签名写入 `velox.udf.v1` custom section，将批处理入口导出；宿主读取其中的全部声明，验证模块与入口，然后注册到 Velox 的函数 registry。

该方案减少独立 worker 的进程与通信开销，也避免把第三方 C++ 动态库直接加载进 Velox 的原生地址空间。它的核心价值是**进程内的不可信代码执行**，而不是单纯增加一门 UDF 开发语言。安全边界由 Wasm runtime、typed imports 的能力白名单、宿主对 ABI 数据的校验以及资源限制共同构成。当前实现提供 fuel、epoch deadline 与 task cancellation，但生产部署仍需独立控制模块编译、JIT 和进程总资源，并验证宿主桥接边界。当前状态为实验性实现。

## 背景与问题

Velox 原生 UDF 通常与宿主一同编译，或以动态库形式加载。动态库中的代码与 Velox 共享原生地址空间和进程权限；一个越界写、无限循环或未经授权的系统调用，都可能影响整个查询进程。将 UDF 放到独立进程可以提供更强的操作系统隔离，但需要额外的部署、进程管理和跨进程通信。

我们希望允许团队或租户提交自定义函数，并在 Velox 的向量化执行路径中调用它们。对于这样的代码，仅仅“可以动态加载”并不足够；关键要求是宿主能够限制它能访问什么，以及单次调用能消耗多少资源。

| 方案 | 执行位置 | 代码权限与故障边界 | 主要成本 |
|---|---|---|---|
| C++ 动态库 | Velox 进程 | 原生进程权限与地址空间 | 宿主难以隔离不可信代码 |
| 独立 worker | 单独进程 | 可使用操作系统隔离机制 | 部署、进程管理和 IPC |
| Wasm UDF | Velox 进程内的 Wasm instance | 线性内存与显式 imports | runtime、批量数据转换和资源治理 |

这里的“进程内”是性能和集成位置的描述，不意味着 Wasm 与原生代码具有同等权限；“不可信”也不意味着可以省略对编译器、runtime 和宿主桥接代码的安全维护。

## 目标

1. 宿主只接收一个 `.wasm` 文件，加载其中声明的全部 scalar 和 aggregate UDF，无需外置 manifest。
2. 默认不给 guest 文件、网络、时钟、进程、线程或任意宿主函数能力。
3. 限制单个 instance 的线性内存与调用栈，并为每次执行设置可中断的计算预算。
4. 保持 Velox 的 SQL 类型、null、`TRY`、选中行、聚合状态和 partial/final aggregation 语义。
5. 在越界访问、trap、格式错误或超出预算时，让当前查询失败，并尽量保持宿主进程可用。

本 RFC 不承诺 Wasm 是独立进程级别的隔离，也不承诺与 C++ simple function 同等的逐行性能。热更新、WASI 和任意宿主 imports 不属于当前范围。

## 执行模型

```text
Rust UDF + SDK macros
       │ 编译
       ▼
单个 .wasm 文件
  ├─ velox.udf.v1：SQL 名称、类型、null 语义、入口映射
  └─ exports：scalar 批处理入口、aggregate 生命周期入口
       │ registerWasmModule(path)
       ▼
Velox 校验 metadata、编译模块并注册所有 UDF
       │ SQL 调用
       ▼
Velox vectors → Arrow IPC → Wasm linear memory → Rust 函数
              ← Arrow IPC ← Wasm linear memory ← 结果
```

SQL 名称默认取 scalar 的 Rust 函数名；aggregate 默认取 struct 名的 snake_case。用户可以显式指定名称或 aggregate export prefix；默认 SQL 名称不加 `wasm_`。一个 module 可包含多个 UDF，宿主一次加载并注册全部声明。custom section 只承载静态注册信息，真正执行由 exports 完成。

宿主跨边界按批传输 Arrow IPC。Rust SDK 在 guest 内遍历行并调用用户编写的普通 Rust 函数，因此每个 Velox 批次通常只跨越一次 Wasm 调用边界。aggregate 的活动状态留在 guest 内，Velox 只保存不透明的 state handle；中间状态经序列化后可用于 partial/final aggregation 和 spill。

## 与 native aggregate 的能力对齐

新增 `VeloxRowAggregate`／`#[velox_row_aggregate]`：Input 为逐行值或
Generic／ARRAY／MAP／ROW／VariadicView 借用视图；State 必须拥有跨调用保留的内容。
宿主绑定泛型、variadic 和整数约束，intermediate 可声明为 ROW／ARRAY 等逻辑类型，
支持 partial→intermediate→final、single、spill 和 native window。默认 NULL 行为
保持空组／全 NULL 组为 NULL；非默认行为也允许 merge 接收 NULL intermediate。
初始化传绑定类型、常量 mask 和声明的配置；原始常量不会自动传到下游 merge，
应把其作用保存在 intermediate。serialize／finalize 不得改变状态的 SQL 内容。

row UDAF 的 `type Error = RowError` 保留 native Status 语义：UserError 为用户
异常，其余 Status 为系统异常，与 native EvalCtx::setStatus 一致。create／update／
merge／serialize／finalize／toIntermediate 均适用。任何失败都会废弃 Store，不能
复用部分更新的状态；它不提供 scalar 的按行 TRY。普通 String 错误保留旧 export
failure 行为。新 row UDAF 声明 ABI 3：沿用 ABI 2 的 typed invocation status，
在成功 payload 中加入每组状态大小报告，并要求 compact 入口；调用签名仍保持一致。
宿主兼容 ABI 1／2 的旧 payload，旧宿主会在加载时拒绝新声明。

整段 bridge 调用串行化以满足并发 spill extraction；跨 create batch 检查 handle
唯一性。row UDAF 提供 toIntermediate 的临时逐行状态实现；DISTINCT／mask／ORDER BY
沿用 native 算子。旧 aggregate ABI 保留兼容。OPAQUE 长期状态需序列化 codec，
不能保留 invocation handle。自动注册 `_partial`／`_merge`／`_merge_extract`／
`_extract`，复用 native companion 签名、返回类型后缀和适配器；重载变更刷新函数族，
生成名称冲突会回滚整个 module。extract 的整次调用 UserError 在 TRY 下也会抛出，
与 native companion 一致；失败后释放实例，后续 API 重试创建新实例。
State 实现 `AggregateState`，owned String／Vec／nested Value 递归统计预留容量，
struct 可 derive；共享／自定义分配需要作者明确归属策略。create／update／merge／
extract 沿已有调用报告大小，宿主更新 native 每组 row-size，不增加每批边界调用。
Runtime 通过 Wasmtime host memory creator，在创建／增长可访问 linear pages 前向
native query pool 计费，不再预留整个配置上限。callback 保留 native 内存失败，即使
guest 忽略 memory.grow 的 -1 也会抛出并废弃 Store。guard／虚拟地址空间保持不可访问。
compact 先收缩 guest 容器并更新大小；启用 `checkpoint = true` 的 row UDAF 还支持
完整 owned State 检查点和活动 Store 重建。即使 native 只传入部分 group，也保存全部
活动 State、handle 高水位及私有配置，在释放旧 Store 后用原 InitContext 恢复，
保留 NULL 标志和共享 row-size 计数；不能用可能丢失私有 update 状态的 SQL intermediate
替代检查点。只有实际可访问页减少才报告回收字节，空间／碎片阈值不满足时返回 0，
让 native 继续 spill。最后一组成功销毁也会释放 Store／页计费；native spill／flush
仍使用 SQL intermediate。检查点是 ABI 3 的可选能力，作者须明确声明行为可从 State
完整重建；状态依赖 mutable globals／guest 地址／invocation handles 时不能直接启用。
SQL lambda 已通过显式 typed callback 接入 native evaluator，并验证 grouped／staged／spill／Store 重建。
已补上批量 lambda 回调及可选的 update_batch／merge_batch；作者接收值／借用视图 iterator，
不接收 RecordBatch。reduce 按 chunk 映射并批量合并，宿主限制每批行数与累计求值行数。
lambda 形参遮蔽同名输入列；修复了 AggregateInfo 把形参名误作捕获列而增加参数的问题。
真正的外部列捕获还需明确 raw／combine／spill／merge 的值传递契约，当前 native reduce
同样只向 evaluator 提供形参 ROW。lambda 与 variadic 合用已实现：SQL 参数按固定值、
FUNCTION、尾参数排列；Rust tuple／constant 索引排除 FUNCTION。SQL resolver 支持展开／
空尾部，SignatureBinder 在未解析 lambda 后继续绑定已知值类型。只依赖尾部的 T 仍需要
实际 typed 值来推断，固定 T 参数则能绑定空尾部。独立 merge／extract 若不调用
lambda，可保留输入类型 T 已擦除的符号槽位；初始化元数据显式标记 types_bound=false，
不伪造替代类型。SDK 的 has_bound_types 可检查，实际调用未绑定槽位则明确失败；
结果与 intermediate 的必需类型仍须解析。需要 lambda 求值的独立阶段还需要传递
native 表达式与 evaluator 权限。更高效 IPC、零额外空间下的
流式／分段回收和 native 优化路径仍需实现，详见
[UDAF 能力表](wasm-udaf-parity.md)，这些不是泛型／nested 的 WASM 根本限制。

UDAF 初始化现可区分工厂绑定的 raw／intermediate 输入类型；该信息不等同于 operator
阶段，因为 native staged plan 可能用 raw types 构造 final 实例。独立 merge 的常量
intermediate 单独标记并通过 intermediate_constant_value 读取，raw 常量配置 mask
清零，避免把 ROW 数据解释为原始 BIGINT 因子。常量 SQL NULL 与缺失常量仍可区分。
初始化 hint 只能用于检查／容量规划，不能预先累计而在 merge 时再次计入每一行。
直接绑定 intermediate 的实例拒绝 raw update，并不宣称支持 raw toIntermediate。

## 与 native Simple Function 的能力对齐

SDK 的作者接口使用逐行标量、Generic、VariadicView 和 ARRAY/MAP/ROW 借用视图。
VariadicView 在 batch 层检查尾参数类型与长度，每行只借用参数列表；索引和
迭代才读取具体值，不创建每行 Vec。`skip_nulls()` 跳过 NULL 但保留解码
错误，`Result` 继续走按行错误通道。已有 owned Variadic API 保留兼容。
`Generic::hash/compare/equals` 直接读取 guest 缓冲区，比较在出现决定性差异时
停止；hash 必须遍历所有元素。固定参数和 variadic 的 primitive reader 现在按批缓存
类型转换与 NULL metadata，保留按访问转换值和错误的语义。VariadicView 共享缓存、
实现 Clone；逐行创建／clone 不分配参数 Vec。该变化需要依赖隐式 Copy 的 Rust
调用方改用 clone，预编译 module／host ABI 仍兼容。相同 benchmark binary 交替运行
旧／新 module，row SUM 批次耗时下降 37.92%，仍为 native 的 30.48 倍；详见能力表。
这没有实现宿主投影或跨边界的按需加载：输入仍先 gather 并解析完整 IPC。

宿主 nested output codec 支持按列延迟解码：ROW 字段、ARRAY 元素和 MAP value
可保留序列化 payload，首次访问再解码整列。整列加载保证 native LazyVector
一次加载与共享表达式结果复用之间的正确性；尚未实现只解码所选行。
MAP key、OPAQUE handle 和顶层 scalar codec 结果仍立即处理；aggregate 与
borrowed IPC decoder 保留 eager 行为。wire/schema/payload 非 NULL 等结构
检查立即完成，codec 字节内容在加载时解释。延迟失败作为系统错误绕过 TRY，
丢弃部分结果并使仍存活的 Store 失效。结果持有 IPC/pool 所有权，使用 weak
Store 回调，不依赖 producer 的生命周期。Native LazyDereference 查询路径能
跳过未用字段；普通 ROW reader 和 sparse copy 仍可能加载全部子字段。

`call_ascii` 使用 native 输入 ASCII 缓存，经 IPC schema metadata 传入 guest，
每 batch 选择一次路径。宿主检查实际 VARCHAR 返回字节并缓存结果编码，不
根据 guest 声明推断输出一定为 ASCII。完整能力与剩余差异见
[Simple Function 对齐表](wasm-scalar-sfi-parity.md)，当前实测与证据见
[社区 review 后续](wasm-udf-community-review.md)。

## 信任边界

**guest 不可信。** 宿主必须将 custom section、export、返回的指针和长度、Arrow IPC 内容、错误文本及 aggregate handle 都视为不可信输入。模块编译成功只能证明它是有效的 Wasm，不能证明其业务逻辑正确或资源消耗有限。

**能力显式授权。** 只接受固定类型的 `velox_udf_v1.lambda_call`／`lambda_call_batch`／`lambda_result` imports。仅声明 lambdas 的 row UDAF query instance 绑定对应 native evaluator；scalar、legacy 和 module start 无此能力。请求／结果校验类型与内存范围，单次消费 token，记入 query pool，并受字节数／调用次数／每批及累计求值行数／取消／deadline 约束。native 异常只在 Wasmtime 返回后重新抛出。WASI 和任意其他宿主 imports 被拒绝，guest 无文件系统／网络能力。

**内存访问受 runtime 约束。** Wasm 指令只能访问其被允许的内存；宿主在读取 guest 返回的数据前还需验证线性内存范围。当前每个 instance 的线性内存上限默认为 64 MiB，Wasm 栈上限为 2 MiB。模块编译、宿主侧 Arrow 缓冲区、缓存和进程其他内存不包含在这 64 MiB 中。

**隔离范围有限。** Wasm sandbox 不是操作系统进程边界；runtime 或宿主 C++ 桥接代码的漏洞仍可能危及进程。guest 可以通过合法返回值泄露它已获准处理的数据；本方案本身不是数据访问控制。上线策略仍需决定谁可以发布模块、谁可以调用函数，以及哪些数据可以进入函数。

## 资源治理与上线门槛

| 资源或风险 | 当前实现 | 上线前要求 |
|---|---|---|
| 线性内存增长 | Wasmtime Store limiter，默认每 instance 64 MiB | 根据并发 instance 数确定进程总预算；监控 limit trap |
| Wasm 调用栈 | runtime 上限 2 MiB | 保持配置并验证深递归失败路径 |
| 无限循环与 CPU 消耗 | 每 export 1 亿 fuel；每 logical invocation 默认 1 秒 epoch deadline；full exec 连接 task token | 验证实际取消延迟；按部署负载配置预算，不把单次 fuel 当作 query 总预算 |
| 模块编译与宿主缓冲区 | 模块 snapshot 默认 64 MiB；IPC/Arrow 分配上限 64 MiB 并记入 MemoryPool；JIT/metadata 另设进程预算 | 限制模块大小、注册并发与批次大小；计入宿主内存 |
| aggregate guest state | 保存在 guest，Velox 只看见 handle | 可访问页在创建／增长前向 query pool 计费；验证 grouped state reports、checkpoint 回收和 operator spill/arbitration |
| runtime/桥接漏洞 | 依赖 Wasmtime 和宿主代码正确性 | 固定并维护 runtime 版本；对 metadata、IPC 和指针范围做负向测试 |

当前同时启用 fuel 与 epoch interruption。epoch ticker 周期为 10 ms，deadline 覆盖一整个 invocation，fuel 按 export 刷新；start、alloc/free 和 aggregate 生命周期入口都受限。10 ms 是检查频率，不是实时取消保证。table 元素数默认 10,000、table 数默认 1，独立于 64 MiB linear-memory limit。注册是特权 startup 路径；epoch 不控制编译/JIT，应通过隔离编译和部署预算保护该路径。

## 加载与失败语义

宿主在注册前读取 `velox.udf.v1`，检查 ABI 版本、函数数量与 metadata 大小、SQL 签名、重复名称及所有导出的类型。模块只允许上述 typed lambda imports，实际 authority 按 query instance 绑定。任一声明无效时，整个 module 加载失败；不应留下已经注册的一部分函数。当前先构造所有条目和替换 map，再通过不抛异常的 swap 发布。注册仅允许在 startup 完成 native 注册后执行，不允许与查询或 native/window registry 修改并发；WASM 注册之间串行。这不是运行中 hot reload 的跨 registry 事务。名称先规范化；禁止遮蔽 native scalar；跨模块不同 scalar overload 可合并，overwrite 只替换同一 dispatch signature。

结果解码先验证声明的 schema，再读取唯一的 record batch，并拒绝 EOS 后的尾随字节。
MAP 暂以物理布局相同的 LIST/STRUCT 读取，先检查 MAP entry 和 key 本身非 NULL，
再共享原缓冲区恢复 MAP 描述，避免 Arrow 构造器在校验前 abort 宿主。
非 NULL 的 ARRAY/ROW key 可以含 NULL 子值，与 native Simple Function reader/writer
及 map_from_entries 一致；SQL map() 的 indeterminate-key 限制由该函数单独执行。
SDK lookup 直接比较借用的 key，使用 NULL-as-value equality，避免按候选 key 构造值树。

执行期间，trap、资源超限、越界范围和损坏的 IPC 使当前查询失败。只有合法 scalar error column 承载的业务错误可按行返回，以保留 Velox `TRY` 行级语义；运行时、资源或协议错误绕过 TRY 并废弃 Store。aggregate 清理不会将 guest 异常传播到 native 析构函数：失败时直接释放 Store、清除 host handle。aggregate 更新可能已修改 guest 状态，因此发生错误后应终止该查询并丢弃相关状态，而不是继续使用部分更新的状态。

## 验收标准

- 一个 `.wasm` 内至少包含多个 scalar 和 aggregate UDF，单次加载后全部可在 SQL 中调用，且默认名称没有 `wasm_` 前缀。
- 拒绝带 WASI 或非白名单 imports 的模块；拒绝无效 metadata、错误 export 类型和越界返回值。
- 超过线性内存、栈与计算预算时，调用在有界时间内失败，宿主可继续处理后续查询。
- 验证选中行、null、`TRY`、grouped aggregation、partial/final aggregation、spill 和错误清理路径。
- 在目标负载下测量端到端延迟、峰值进程内存、并发 instance 数以及查询取消延迟；据此确定批次和资源默认值。

## 相关实现与参考资料

- 仓库实现：[Rust SDK](../../functions/wasm/sdk/README.md)、[注册入口](../../functions/wasm/Registration.cpp)、[runtime](../../functions/wasm/Runtime.cpp)、[metadata 解析](../../functions/wasm/Manifest.cpp)。
- [Wasmtime Security](https://docs.wasmtime.dev/security.html)：Wasm sandbox 的安全模型与边界。
- [Wasmtime Interrupting Execution](https://docs.wasmtime.dev/examples-interrupting-wasm.html)：fuel 与 epoch interruption。
- [WebAssembly custom sections](https://webassembly.github.io/spec/core/appendix/custom.html)：custom section 不参与 Wasm 执行语义。


本轮检查点实现已通过 73 个 Rust 测试、68 个 full GTest、两种构建的 8 项 CTest，
以及新旧 module 各 31 项 benchmark 正确性检查。相同当前宿主、交错运行各三次，
1024 行／100 次 warmup／100M fuel：native SUM 中位数均为 1.73 us，row WASM SUM
为 53.04 → 53.49 us（+0.85%，当前约 30.92 倍 native）。该对比未测量 Store
重建成本、C++ 宿主前后变化或全查询性能；不能据此宣称性能已对齐。原始数据和哈希见
`benchmark_results/wasm-checkpoint-2026-10-05-paired/`，验证证据见
`.deps/alignment-checkpoint-2026-10-05/`。
