# AGENTS — UCX OBMM 传输层 AI 编码代理规范

本文档为所有在本仓库中执行任务的 AI 编码代理提供约束性工作规范。规范基于 V1→V3 开发阶段的经验总结（详见 `ENGINEERING_PRACTICES.md`）。

---

## 0. 首要原则

> **代码为证，不凭记忆。** 所有关于 UCX 框架行为、OBMM API 语义的结论，必须通过向量数据库检索或直接读源文件来验证，不得从记忆中推断。

---

## 1. 每次会话开始时（Session Init）

1. 用 `vector-db-retrieval` skill 检索与本次任务直接相关的 UCX/OBMM 符号。
2. 阅读与改动相关的 `docs/*.md` 和 `ENGINEERING_PRACTICES.md` 中对应章节。
3. 确认当前处于哪个开发版本（nc 模式 / hybrid 模式）及对应的设计文档位置。

---

## 2. 构建任务前置检查（Build Env Gate）

在执行任何需要 `./autogen.sh`、`make`、`gcc` 的任务之前，**必须**先运行：

```bash
# Linux 直接验证
which bash && which gcc && gcc --version

# Windows + WSL 验证
wsl.exe -e bash -c 'which gcc && gcc --version'
```

- 若工具链不可用：**立即停止**，在进度报告中注明"构建验证阻塞：工具链不可用"，不得反复尝试同一命令。
- 若工具链可用：继续执行并记录编译输出。

---

## 3. 编码任务工作流（Coding Workflow）

每个功能开发或 Bug 修复必须遵循以下顺序：

```
Step 1: 设计先行
  - 更新或创建对应的 docs/*.md 设计文档
  - 明确：接口签名、数据布局、算法、边界条件、错误处理

Step 2: 关键假设验证
  - 使用 vector-db-retrieval skill 检索相关 UCX/OBMM 符号
  - 阅读原始源文件确认宏/回调的真实语义

Step 3: 实现
  - 对照 Step 1 的设计文档编写代码
  - 只做增量扩展，不重写已稳定的路径（特别是 NC V2 路径）

Step 4: 静态检查
  - git diff --check（空白检查）
  - 确认 git status 只包含预期的文件

Step 5: 代码评审
  - 运行代码评审工具
  - 每条建议对照具体语义判断是否适用，不得盲目采纳

Step 6: 编译验证（需 Linux 环境）
  - cd ucx && ./autogen.sh
  - ./contrib/configure-devel --prefix=$PWD/install
  - make -j && make install
  - ucx_info -d -t obmm 确认能力枚举正常

Step 7: 运行时测试（需真实硬件）
  - 先用 MEM_MODE=nc 验证 V2 不回归
  - 再用 MEM_MODE=hybrid 验证新功能
```

---

## 4. OBMM V3 Hybrid 模式不变量

以下约束是 V3 设计的核心，**任何代码改动都不得违反**：

| 不变量 | 说明 |
|--------|------|
| NC 控制面不可变 | FIFO、owner-bit 协议、`am_short` 路径全部走 NC 内存 |
| CC 仅用于 payload | `am_bcopy` payload chunk 走 CC；其 FIFO descriptor 仍走 NC |
| V2 路径不回归 | `MEM_MODE=nc` 下 V2 行为必须与改动前完全一致 |
| 拓扑不自动探测 | NC/CC memid 列表必须由用户通过 env var 显式声明 |
| CC 不用于 FIFO | CC 内存所有权切换粒度为 4KiB，频繁切换开销不可接受 |

---

## 5. 调试工作流（Debug Protocol）

遇到 UCX wireup 错误、意外状态、崩溃时：

```
1. grep 字面错误字符串 → 找到代码发出点（不要先列假设）
2. 向上读代码 → 找到失败的前置条件
3. 向量检索相关符号 → 确认框架语义
4. 确定根因后才动代码
```

**禁止**：在未看代码的情况下推测错误原因并直接修改代码。

---

## 6. 验证声明规范（Verification Claims）

- 只声称已经执行过的验证。
- 验证阻塞（如无 Linux 环境、无硬件）必须在进度报告中明确注明。
- 不得用"代码看起来正确"代替实际编译/运行验证。

格式示例：
```
已完成：
  - git diff --check: 通过
  - 代码评审 agent：1 个问题已修复

验证阻塞：
  - 本地编译：Windows 无 WSL/gcc，需在 Linux 环境运行
  - 运行时测试：需真实 OBMM 硬件
```

---

## 7. UCX 常见陷阱（Quick Reference）

| 陷阱 | 正确做法 |
|------|---------|
| `UCS_STATIC_ASSERT` 在文件作用域使用 | 必须在函数体内使用（switch/case 语句宏） |
| 忘记声明 `UCT_IFACE_FLAG_INTER_NODE` | 跨节点传输必须声明，否则被 NET_ONLY 过滤器静默剔除 |
| FIFO slot 用 FAA 而非 load+CAS | FAA 无法在跨主机场景回滚，必须用 load+CAS |
| 用 remote memid 匹配本地 import | import memid 是本机本地编号，与 export memid 不等 |
| 对 CC 内存做频繁 `set_ownership` | 4KiB 粒度，仅在 am_bcopy payload 发送/接收时切换 |
| 假设 export.addr == import.addr | export.addr 是 UBA，import.addr 是本机物理地址 |

---

## 8. 进度报告规范

- 每完成一个有意义的工作单元后调用 `report_progress`。
- checklist 格式：`- [x]` 已完成，`- [ ]` 待完成。
- 始终包含"验证阻塞"小节（若有）。

---

*本文档由 AI 编码代理根据 V1~V3 开发经验自动生成，最后更新：2026-05-13。*
