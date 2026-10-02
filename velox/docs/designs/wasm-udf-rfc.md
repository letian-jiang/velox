# RFC: 在 Velox 进程内执行不可信 Wasm UDF

- **状态**：Draft
- **日期**：2026-09-30
- **范围**：Velox scalar UDF 与 aggregate UDF

## 摘要

Wasm UDF 的独特作用，是让 Velox 在查询进程内运行来源不受信任的函数代码，同时将代码的内存访问和宿主能力限制在明确的边界内。部署单位是一个自包含的 `.wasm` 文件：Rust SDK 将 SQL 签名写入 `velox.udf.v1` custom section，将批处理入口导出；宿主读取其中的全部声明，验证模块与入口，然后注册到 Velox 的函数 registry。

该方案减少独立 worker 的进程与通信开销，也避免把第三方 C++ 动态库直接加载进 Velox 的原生地址空间。它的核心价值是**进程内的不可信代码执行**，而不是单纯增加一门 UDF 开发语言。安全边界由 Wasm runtime、无 imports 的能力策略、宿主对 ABI 数据的校验以及资源限制共同构成。当前实现尚缺少可中断的 CPU 预算，因此在补齐这项控制前，不应宣称可以安全承载任意不可信代码。

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

## 信任边界

**guest 不可信。** 宿主必须将 custom section、export、返回的指针和长度、Arrow IPC 内容、错误文本及 aggregate handle 都视为不可信输入。模块编译成功只能证明它是有效的 Wasm，不能证明其业务逻辑正确或资源消耗有限。

**能力默认关闭。** 当前加载器拒绝任何 imports，也不提供 WASI。guest 因此不能通过本 ABI 直接调用文件系统、网络或宿主 API。未来若引入 imports，必须逐项定义权限、输入校验、配额和审计；不能把整套 WASI 能力作为默认配置。

**内存访问受 runtime 约束。** Wasm 指令只能访问其被允许的内存；宿主在读取 guest 返回的数据前还需验证线性内存范围。当前每个 instance 的线性内存上限默认为 64 MiB，Wasm 栈上限为 2 MiB。模块编译、宿主侧 Arrow 缓冲区、缓存和进程其他内存不包含在这 64 MiB 中。

**隔离范围有限。** Wasm sandbox 不是操作系统进程边界；runtime 或宿主 C++ 桥接代码的漏洞仍可能危及进程。guest 可以通过合法返回值泄露它已获准处理的数据；本方案本身不是数据访问控制。上线策略仍需决定谁可以发布模块、谁可以调用函数，以及哪些数据可以进入函数。

## 资源治理与上线门槛

| 资源或风险 | 当前实现 | 上线前要求 |
|---|---|---|
| 线性内存增长 | Wasmtime Store limiter，默认每 instance 64 MiB | 根据并发 instance 数确定进程总预算；监控 limit trap |
| Wasm 调用栈 | runtime 上限 2 MiB | 保持配置并验证深递归失败路径 |
| 无限循环与 CPU 消耗 | 尚无 fuel 或 epoch interruption | 为 guest 执行配置可中断预算，并与查询取消联动 |
| 模块编译与宿主缓冲区 | 不受 guest 线性内存上限约束 | 限制模块大小、注册并发与批次大小；计入宿主内存 |
| aggregate guest state | 保存在 guest，Velox 只看见 handle | 明确与 Velox memory arbitration、spill 的关系；至少观测总内存与失败率 |
| runtime/桥接漏洞 | 依赖 Wasmtime 和宿主代码正确性 | 固定并维护 runtime 版本；对 metadata、IPC 和指针范围做负向测试 |

Wasmtime 提供 fuel 和 epoch interruption 两类机制。具体选择应通过性能与取消延迟测试决定；无论选择哪种，都要覆盖 scalar、aggregate 的所有入口，包括分配和状态生命周期调用。只限制内存无法阻止 `loop {}` 长时间占用查询线程。

## 加载与失败语义

宿主在注册前读取 `velox.udf.v1`，检查 ABI 版本、函数数量与 metadata 大小、SQL 签名、重复名称及所有导出的类型。模块不得导入宿主能力。任一声明无效时，整个 module 加载失败；不应留下已经注册的一部分函数。实际注册阶段的失败和名称冲突也需要保持这一原子性，这是需要验证的实现要求。

执行期间，trap、资源超限、越界范围和损坏的 IPC 使当前查询失败。scalar 业务错误可按行返回，以保留 Velox `TRY` 行级语义。aggregate 更新可能已修改 guest 状态，因此发生错误后应终止该查询并丢弃相关状态，而不是继续使用部分更新的状态。

## 验收标准

- 一个 `.wasm` 内至少包含多个 scalar 和 aggregate UDF，单次加载后全部可在 SQL 中调用，且默认名称没有 `wasm_` 前缀。
- 拒绝带 WASI 或其他 imports 的模块；拒绝无效 metadata、错误 export 类型和越界返回值。
- 超过线性内存、栈与计算预算时，调用在有界时间内失败，宿主可继续处理后续查询。
- 验证选中行、null、`TRY`、grouped aggregation、partial/final aggregation、spill 和错误清理路径。
- 在目标负载下测量端到端延迟、峰值进程内存、并发 instance 数以及查询取消延迟；据此确定批次和资源默认值。

## 相关实现与参考资料

- 仓库实现：[Rust SDK](../../functions/wasm/sdk/README.md)、[注册入口](../../functions/wasm/Registration.cpp)、[runtime](../../functions/wasm/Runtime.cpp)、[metadata 解析](../../functions/wasm/Manifest.cpp)。
- [Wasmtime Security](https://docs.wasmtime.dev/security.html)：Wasm sandbox 的安全模型与边界。
- [Wasmtime Interrupting Execution](https://docs.wasmtime.dev/examples-interrupting-wasm.html)：fuel 与 epoch interruption。
- [WebAssembly custom sections](https://webassembly.github.io/spec/core/appendix/custom.html)：custom section 不参与 Wasm 执行语义。
