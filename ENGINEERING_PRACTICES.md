# obmm UCT transport 工程实践复盘

本文总结 `obmm` UCT transport 从 V1/V2 到 V3 设计阶段形成的工程实践。目标不是记录流水账，而是固化哪些做法让项目成功、哪些做法造成了无用功，以及后续 V3 开发时应怎样避免重复踩坑。

## 为什么 V2 能成功

### 1. 先把事实固化，再写代码

项目最关键的成功因素，是把硬件/环境事实从“聊天上下文”转成了可反复读取的约束：

- `AGENTS.md` 规定了 retrieve → 读环境事实 → 设计 → review → 实现 → 验证的流程。
- `.github/skills/obmm-api-and-env/SKILL.md` 固化了 libobmm API、测试环境、禁止调用 export/import、NC/CC 一致性等事实。
- `ucx/src/uct/obmm/DESIGN.md` 固化了 wire format、FIFO 语义、pool version、pending、V3 hybrid 设计。

这避免了 UCX 框架开发中最危险的问题：上一次讨论中的隐含假设在后续实现时被遗忘，或者被“看起来像 mm transport”的代码覆盖。

### 2. 以 UCX 框架代码为准，不凭印象猜

成功的修复基本都有同一个模式：

1. 先定位报错字符串或调用栈。
2. 读产生报错的 UCX/OMPI/UCP 代码。
3. 反推出真实前置条件。
4. 只改触发前置条件失败的最小面。

典型例子：

- cross-node MPI `Unsupported operation`：先 grep `select.c` 报错，再读 `ucp_worker.c` 的 `NET_ONLY` 地址过滤，最终发现缺 `UCT_IFACE_FLAG_INTER_NODE`，一行修复。
- OSU 256 KiB segfault：从 `ucp_proto_rndv_progress_rkey_ptr()` 调用栈追到 obmm 错误广告 `RKEY_PTR`，删除不真实的 rkey_ptr 能力后恢复正确协议选择。
- OSU bibw 4096 hang：读 UCP pending 路径，确认 `UCS_ERR_BUSY` 会让 UCP 在同一个调用栈里 busy-spin，最终按 mm 模式补真实 pending arbiter。

这些问题如果只靠经验猜，都会走很远的弯路。

### 3. 把测试分层，而不是一步到 MPI/OSU

V2 能稳定，是因为测试逐层推进：

| 层级 | 作用 |
| --- | --- |
| `ucx_info` / symbol | 确认组件注册、caps、配置键、符号 |
| `ucx_perftest` UCT 层 | 绕开 MPI/UCP 复杂性，先验证 UCT 基础收发 |
| MPI sanity/correctness/pingpong/bw/collective | 验证 OMPI → UCP → UCT 集成 |
| MPI v2 多尺寸/多进程测试 | 覆盖 short/bcopy/fragment 边界和 >2 ranks |
| OSU | 暴露 UCP 协议选择、pending、rndv、双向压力等真实工作负载问题 |

每一层失败时只解释这一层的前置条件，不把 OSU 失败直接归因到硬件或 FIFO。

### 4. 用户反馈闭环非常快

本地没有硬件，成功依赖“agent 负责读代码和设计，用户负责真实节点验证”的闭环：

- 用户报告准确的命令、size、日志、调用栈。
- agent 必须把报告映射到源码证据，不能编造硬件行为。
- 需要硬件事实时用问题确认，例如 V3 `obmm_set_ownership()` 粒度最终由用户确认是 4 KiB page。

这使得没有硬件的开发仍然能推进，但前提是不能把未验证的猜测写进 transport。

### 5. V2 保持了正确的最小能力集

V2 成功不是因为能力多，而是因为能力广告和真实实现一致：

- 实现并广告 `AM_SHORT`、`AM_BCOPY`、`PENDING`、`CONNECT_TO_IFACE`、`CB_SYNC`、`INTER_NODE`。
- 不广告 PUT/GET/atomic/zcopy/rkey_ptr。
- 不让 UCP 选择 obmm 无法支持的协议。

UCT transport 最容易失败的地方不是“少一个功能”，而是“广告了一个不真实的功能”。

## 走过的弯路，以及最后如何纠正

### 1. 先猜后查导致 cross-node wireup 浪费时间

cross-node MPI 失败时，曾经围绕 AM_SYNC、is_reachable、版本不一致等方向猜测。用户指出单节点已经能跑，说明基础 AM 能力不是问题。之后才回到正确流程：grep literal error string → `select.c` → 地址过滤 → `INTER_NODE`。

纠正后的规则：

- UCX wireup/select 错误先 grep 报错字符串。
- 读 emit site 往前追条件。
- 不列假设清单，除非每个假设都有源码路径支撑。

### 2. 复制 sm/mm 的 rkey_ptr 语义导致 OSU 大消息崩溃

obmm 只 mmap 预导入的 128 MiB region，并没有 attach 对端进程任意 user heap。复制 `sm` 的 `RKEY_PTR` 能力会让 UCP rendezvous 直接 memcpy 一个本地未映射的远端地址。

纠正后的规则：

- 参考 mm/self 只能参考框架结构，不能复制能力语义。
- 每个 cap 必须回答：“这个指针/地址/内存句柄在 obmm 中真实存在吗？”
- 不确定时宁可不广告，让 UCP 选择更慢但正确的 AM 协议。

### 3. FAA reservation 在满 FIFO 时泄漏 head

最初按共享内存 FIFO 习惯想到 FAA，但跨主机 FIFO 满时无法回滚 head，留下永远不会 publish 的 gap，receiver 顺序扫描会卡死。

纠正后的规则：

- 跨主机 FIFO slot reservation 必须用 load + CAS。
- capacity check 必须发生在提交 head 前。
- “本地共享内存可接受的技巧”不能直接迁移到 obmm。

### 4. pending_add 返回 BUSY 只适合早期，不适合真实压力

早期为了跑通，`pending_add` 返回 `UCS_ERR_BUSY` 看似能让 UCP retry。OSU bibw 的双向窗口压力证明它会在 UCP 内部 busy-spin，不让 progress 运行，双方都无法 drain FIFO。

纠正后的规则：

- `PENDING` 一旦广告，就要有真实队列语义。
- 对称双向压力是 pending 正确性的必测场景。
- “先返回 BUSY”只能作为短期 bring-up 手段，不能带入 V2/V3 稳定基线。

### 5. build/run 环境混淆导致无效脚本

曾经为 run host 设计了依赖 python/perl/dd sync/本地编译的探针，但真实 run host 没有这些工具，且构建和运行机器分离。

纠正后的规则：

- run host 脚本：LF、English-only、不依赖 python/perl/compiler。
- 需要二进制探针时，在 build host 编译后 scp。
- 本地 Windows 环境不跑 UCX build；只做静态检查和交付 Linux 命令。

### 6. V3 设计中最危险的幻觉：把 CC 当成更快的 NC

CC 不是“更快的普通共享内存”。它有 host-level ownership 约束：同一时刻只能 all-read/none 或 one-writer/all-none。FIFO/control 如果放在 CC 上，会变成每条消息都要 ownership handoff。

纠正后的规则：

- V3 只做 hybrid：NC 控制面，CC bcopy payload。
- short/FIFO/tail/pending 控制仍在 NC。
- CC chunk publish 必须先完成 ownership release，再发布 NC FIFO descriptor。

## 什么情况下会做无用功

以下情况应该立即停下，回到证据收集：

1. **没有读报错源码就解释 UCX 错误。** UCX 的错误字符串常常是筛选结果，不是根因。
2. **把 mm/self 的能力位直接复制到 obmm。** 框架钩子可以参考，内存语义不能复制。
3. **在本地 Windows 环境尝试 Linux UCX build。** 当前环境没有 bash/WSL/gcc，这只会浪费时间。
4. **运行需要硬件/两节点的测试。** 本地只能做静态检查；真实验证由用户在节点上跑。
5. **为 run host 写依赖丰富工具链的脚本。** run host 没 python/perl/compiler，脚本必须极简。
6. **性能优化早于协议正确性。** V2 的成功来自先修 cap、pending、wire format、fence，再谈 OSU 性能。
7. **把紧耦合的 UCT transport 实现拆给多个低级 sub-agent。** md/iface/ep/ops/caps/reachability 必须整体一致，分头写很容易产生接口错位。
8. **未更新设计文档就改 wire format。** pool version、iface address、FIFO flags、caps 必须先写清楚。

## V3 开发的最佳实践

### 设计前

- 先读 `ENGINEERING_PRACTICES.md`、`AGENTS.md`、`obmm-api-and-env`、`DESIGN.md`。
- 涉及 UCX 框架宏/ops/caps/reachability 时，用 vector retrieval 找 mm/self 参考，再直接读源码。
- 涉及 OBMM API、mmap、ownership、memid、topology 时，先读 `obmm-api-and-env`，缺事实就问用户。

### 实现中

- 默认保护 V2 NC path；`MEM_MODE=nc` 是 regression boundary。
- 每次新增能力必须同步三处：ops table、`iface_query` caps、UCP 可见地址/reachability。
- 每次改 wire format 必须同步：`DESIGN.md`、pool version/geometry、address struct、reachability diagnostics。
- CC chunk 逻辑必须围绕 ownership 状态机审查，不允许“先 publish 再补 ownership”。

### 调试时

- 先定位，少改代码。
- 字符串报错：grep literal string。
- 调用栈报错：从最顶层 UCX/UCP 函数向下读到 transport capability/ops。
- hang：先确认 progress/pending/backpressure，再看 fence/ownership。
- crash：先看 UCP 是否选择了 obmm 不真实广告的能力。

### 验证时

- 本地 Windows：只做 `git diff --check`、静态 grep、code-review。
- Linux build host：再跑 UCX build、`ucx_info`、`nm`。
- OBMM run host：只跑用户确认的 MPI/OSU/硬件测试命令。

## 当前工作流调整结论

- 保留 `vector-db-retrieval`：它减少 UCX 框架幻觉。
- 保留并更新 `uct-transport-patterns`：它应覆盖 V2/V3 的 AM_BCOPY、pending、INTER_NODE、rkey_ptr 陷阱。
- 保留并重写 `obmm-api-and-env`：旧版“不要调用 set_ownership”只适用于 NC V2，V3 hybrid 需要允许 CC chunk ownership。
- 保留但降级 `ucx-build-verify`：它不应在本地 Windows 自动要求跑 build，而应先判断环境；无 Linux build 环境时只输出应交给用户/build host 的命令。
- `AGENTS.md` 已从 “only am_short” 更新为 “V2 NC short+bcopy stable，V3 hybrid under development”。
