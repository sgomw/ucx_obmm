# UCX OBMM 传输层工程实践总结

> 基于 V1→V2→V3 全对话历史与代码演进的事后分析（Post-Mortem）

---

## 一、项目概述

本项目在 UCX UCT 框架下实现了一个名为 `obmm` 的共享内存传输层，底层对接 OBMM（On-Board Memory Module）硬件。开发分三个阶段：

| 阶段 | 目标 | 结果 |
|------|------|------|
| V1 | TL/MD/iface/ep 骨架 + `am_short` 最小可用 | 成功，ucx_info 可枚举 |
| V2 | 完整 NC 内存 `am_bcopy`（FIFO + pool + 流控）| 成功，MPI/OSU 测试通过 |
| V3 | Hybrid NC+CC 模式，NC 保控制面，CC 仅用于 bcopy payload | 代码实现完成，待 Linux 编译验证 |

---

## 二、成功要素

### 2.1 设计先行（Design-First）

**实践**：每个版本在动代码之前先完成 `DESIGN.md`（或 `docs/*.md`），明确接口、数据布局、算法和边界条件。

**为什么有效**：
- 防止"幻觉漂移"——在写代码时不依赖模糊记忆，而是对照已确认的设计文档。
- 让代码评审（code review）有可对照的规格，而不是凭感觉审查。
- 在 V3 混合模式中，先设计"NC 控制面 + CC bcopy payload"的分工，才避免了将 CC 引入 FIFO/控制面这条危险路径。

**操作要点**：先写设计文档，再改代码；如果设计有更新，先更新文档，再同步实现。

---

### 2.2 代码为证（Retrieve, Don't Recall）

**实践**：所有关于 UCX 框架行为、OBMM API 语义的结论，必须通过向量数据库检索或直接读源文件来确认，而不是凭记忆推断。

**为什么有效**：
- UCX 宏（如 `UCT_TL_COMPONENT_DEFINE`、`UCS_STATIC_ASSERT`）的语义不能靠猜——`UCS_STATIC_ASSERT` 是 switch/case 语句宏，只能在函数体内使用，在文件作用域使用会导致编译错误。
- UCT capability flags（如 `UCT_IFACE_FLAG_INTER_NODE`）静默影响地址打包，不看 `ucp_worker.c` 就不知道 NET_ONLY 过滤器的存在。
- FIFO slot reservation 必须用 load+CAS 而不是 FAA，因为 FAA 在跨主机场景下无法回滚。

**典型反例**：不看代码就下结论说"UCX 框架会自动处理跨节点过滤"——实际上必须显式声明 `UCT_IFACE_FLAG_INTER_NODE`。

---

### 2.3 增量稳定（Incremental Stability）

**实践**：V2 通过测试后，V3 只在 V2 代码上增量扩展，不重写 NC 路径。

**为什么有效**：
- 防止回归——NC V2 路径（`am_short` + NC `am_bcopy`）在 V3 改动后必须仍然通过原有测试。
- "保留 V2 稳定性"被明确写入设计约束，代码评审也以此为基准。

**操作要点**：任何 V3 改动提交前，先确认 `MEM_MODE=nc` 下 V2 行为不变。

---

### 2.4 静态验证代替空洞乐观

**实践**：在无法编译的情况下，仍然运行 `git diff --check`（检查空白错误），用代码评审 agent 做局部分析。

**为什么有效**：
- `git diff --check` 捕获格式错误，成本极低。
- 代码评审 agent 在 V3 实现中发现了 `size_t → uint32_t` 隐式收窄转换（已修复）。
- 明确记录"无法本地编译"是一个验证阻塞点，而不是假装验证通过。

---

### 2.5 边界事实外显化（Topology & Config Explicitness）

**实践**：将所有不可自动推断的硬件事实记入 memory 和设计文档：
- NC/CC 类型由用户通过环境变量声明，程序无法自行探测。
- 每节点 2 个导出（1 NC + 1 CC），N 节点有 2(N-1) 个导入。
- import memid 是本机本地编号，不等于 export memid。
- UBA（Unified Bus Address）与本机物理地址不等同。

**为什么有效**：这些拓扑事实如果在每次会话中重新推断，极易出错。外显化后成为可引用的设计约束。

---

## 三、走过的弯路与纠正

### 3.1 在无 Linux 环境的机器上反复尝试编译（浪费精力）

**现象**：多次执行 `bash -lc 'cd ucx && ./autogen.sh ...'`，WSL 检测，`Get-Command gcc/clang`——均失败，因为开发机是 Windows 且没有安装 WSL/gcc。

**根因**：没有在开始编译任务之前先验证构建环境是否具备。

**纠正**：将"验证构建环境"设为所有编译任务的前置步骤（见 §五）。

**教训**：环境假设是代价最高的错误之一。先问"能跑吗"，再问"怎么跑"。

---

### 3.2 CC 控制面路径（错误设计，及时纠正）

**现象**：初期讨论中曾考虑将 CC 内存也用于 FIFO/控制消息（短消息路径）。

**根因**：没有充分考虑 CC 内存的所有权语义——`obmm_set_ownership` 要求 4KiB 粒度，频繁切换所有权会产生极高开销，且 FIFO 的 owner-bit 协议在 CC 内存上不成立。

**纠正**：设计锁定为"NC 负责所有控制面（FIFO + am_short）；CC 仅用于 am_bcopy 的 payload chunk"。这是 V3 hybrid 模式的核心约束。

---

### 3.3 文件作用域使用 UCS_STATIC_ASSERT（编译错误）

**现象**：在某个头文件的文件作用域使用了 `UCS_STATIC_ASSERT`。

**根因**：误认为 `UCS_STATIC_ASSERT` 是一个可以在文件级别展开的编译时断言，实际上它是基于 `switch/case` 的语句宏，只能在函数体内使用。

**纠正**：将 `UCS_STATIC_ASSERT` 调用移入函数作用域，或使用 C11 `_Static_assert`（如果工具链支持）。

---

### 3.4 代码评审建议的取舍（正确判断，未盲从）

**现象**：代码评审 agent 建议将 CC region 查找限制为"仅 import"，过滤掉 export/self。

**判断**：未采纳——因为 CC 查找按 exporter index 做匹配，local export/self lookup 在某些合法场景下（如自测或环回）是有效的。

**教训**：代码评审工具是辅助，不是权威。每条建议都要结合具体语义判断是否适用。

---

## 四、无用功的触发条件

以下操作模式在本项目中产生了无用功，需要在 V3 阶段避免：

| 场景 | 无用功表现 | 规避方法 |
|------|-----------|---------|
| 在 Windows 无 WSL 机器上尝试编译 | 多次失败的 bash/wsl/gcc 调用 | 先用"构建环境检查"步骤确认工具链存在 |
| 在没有设计文档的情况下直接写代码 | 后期频繁重构，逻辑前后矛盾 | 每版本先写/更新设计文档 |
| 从记忆推断 UCX 框架行为 | 发现与实际代码不符后需要返工 | 先向量检索/读源码，再下结论 |
| 代码评审建议无差别采纳 | 引入不必要的限制 | 每条建议对照具体语义过滤 |
| 大段重写已稳定的代码 | 引入回归风险 | 只做增量扩展，V2 路径不动 |
| 对无硬件测试结果的代码声称"测试通过" | 误导后续决策 | 只声称已做的验证，明确记录阻塞点 |

---

## 五、V3 阶段核心最佳实践

### 5.1 构建环境前置检查

在执行任何编译任务之前，执行：

```bash
which bash && which gcc && gcc --version   # Linux
# 或
wsl.exe -e bash -c 'which gcc && gcc --version'  # Windows + WSL
```

若工具链不可用，**立即停止**并报告阻塞，不要反复尝试。

### 5.2 设计优先工作流

```
1. 更新/创建 DESIGN.md 或 docs/*.md（明确接口、数据布局、算法）
2. 同行评审设计文档（向量检索 + 源码验证关键假设）
3. 实现代码（对照设计文档）
4. 静态检查（git diff --check）
5. 代码评审 agent（针对性采纳建议）
6. 编译验证（Linux 环境）
7. 运行时测试（真实硬件）
```

### 5.3 混合模式开发约束

V3 hybrid 模式的不变量（任何改动都不得违反）：

1. **NC 控制面不可变**：FIFO、owner-bit 协议、`am_short` 路径全部走 NC 内存。
2. **CC 仅用于 payload**：`am_bcopy` 的 payload chunk 走 CC，其 FIFO descriptor 仍走 NC。
3. **V2 回归测试**：每次 V3 改动后，`MEM_MODE=nc` 下的 V2 行为必须不变。
4. **拓扑不自动探测**：NC/CC memid 列表必须通过环境变量显式声明，代码不做类型推断。

### 5.4 调试工作流

遇到 UCX wireup 错误或意外行为：

```
1. grep 字面错误字符串 → 找到代码发出点
2. 向上读代码 → 找到失败的前置条件
3. 向量检索相关符号 → 确认框架语义
4. 确定根因后才动代码
```

不要先列假设、后验证——先看代码，再下结论。

---

## 六、拓扑与配置速查

| 参数 | 说明 |
|------|------|
| `UCX_OBMM_MEM_MODE` | `nc`（默认）或 `hybrid` |
| `UCX_OBMM_NC_MEMIDS` | NC 内存 memid 列表，**必填** |
| `UCX_OBMM_CC_MEMIDS` | CC 内存 memid 列表，hybrid 模式**必填**，nc 模式时 warn |
| `UCX_OBMM_CC_CHUNK_SIZE` | CC chunk 大小（4KiB 对齐） |
| 每节点导出数 | 2（1 NC + 1 CC） |
| 每节点导入数 | 2*(N-1)（N 为节点数） |
| NC/CC 类型可探测？ | **否**——由用户通过 env var 声明 |
| import memid == export memid？ | **否**——import memid 是本机本地编号 |
| export desc.addr | UBA（Unified Bus Address） |
| import desc.addr | 本机物理地址 |

---

## 七、代码模块速查

| 文件 | 职责 | V3 新增内容 |
|------|------|------------|
| `obmm_region.{h,c}` | 内存区域 open/mmap 抽象 | CC region：无 O_SYNC，PROT_NONE mmap；`set_ownership` 封装 |
| `obmm_pool.{h,c}` | NC 导出池布局与 slot 分配 | pool version v2(NC)/v3(hybrid)；CC chunk 元数据 |
| `obmm_md.{h,c}` | MD 生命周期、memid 分类、region 查找 | NC/CC memid 解析；CC exporter 表/hash |
| `obmm_fifo.h` | FIFO wire 格式助手 | `CC_CHUNK` 标志；CC descriptor pack/unpack |
| `obmm_iface.{h,c}` | iface 配置/能力/地址/进度 | hybrid init/cleanup/RX/reclaim；CC caps；exporter 地址 |
| `obmm_ep.{h,c}` | EP 连接与 AM 发送路径 | hybrid inflight 队列；CC bcopy 路径；chunk reclaim |

---

*本文档由 AI 编码代理（GitHub Copilot）根据 V1~V3 全对话历史自动生成，最后更新：2026-05-13。*
