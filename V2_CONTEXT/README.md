# V2 上下文检索入口

这个目录用于让一个**完全无上下文**的新窗口，快速补齐从项目启动到 **V2 阶段完成** 为止的设计、实现和 debug 历史。

## 使用原则

1. **先读本文件，再按顺序检索。**
2. `V2_CONTEXT\history\` 下的历史会话文件均为**原样搬运**，未改内容。
3. `plan_v2.md` 文件名虽叫 V2，但内容实际是 **V1 am_short 方案**，只能当早期设计背景看，**不能当 V2 最终方案**。
4. 本目录**只覆盖到 V2 完成**；历史会话材料只收录到 `009-...`。

## 建议检索顺序

### 0. 工作流和环境事实

先读这些仓库内原始文件：

1. `AGENTS.md`
2. `.github\skills\obmm-api-and-env\SKILL.md`
3. `.github\skills\uct-transport-patterns\SKILL.md`
4. `.github\skills\vector-db-retrieval\SKILL.md`
5. `.github\skills\ucx-build-verify\SKILL.md`

作用：

- 明确 obmm transport 的强约束、不可假设项、参考实现和验证边界
- 保持检索视角稳定在 V1/V2 阶段

### 1. 从 0 到 V1 的设计与落地

按顺序读 `V2_CONTEXT\history\`：

1. `001-designing-obmm-transport-am-sh.md`
2. `002-implementing-obmm-sysfs-discov.md`
3. `003-implementing-obmm-pool-and-fif.md`
4. `004-diagnosing-obmm-device-eperm.md`

这些文件覆盖：

- 项目启动时的目标、拓扑、约束
- 为什么选 NC 路径、为什么不能在 UCT 内调用 export/import 生命周期
- sysfs 发现、mmap、pool/FIFO 基础结构
- 早期 bring-up 期间的设备/权限问题

### 2. V2 的实现、测试和收敛

继续按顺序读：

1. `005-adding-mpi-integration-tests.md`
2. `006-implementing-am-bcopy-and-pend.md`
3. `007-fixing-cross-node-inter-node-c.md`
4. `008-implementing-v2-desc-paired-am.md`
5. `009-fixing-osu-rkey-ptr-segfault-t.md`

这些文件覆盖：

- V2 测试矩阵是怎么建立起来的
- `am_bcopy`、pending、`INTER_NODE` 修正
- V2 paired-desc 方案的最终设计和落地
- OSU 暴露出的 `rkey_ptr`/RNDV 问题及其修复

### 3. 误名但仍值得参考的历史文件

1. `V2_CONTEXT\history\plan_v2.md`

说明：

- 这是早期的 **V1 am_short plan**
- 可用来理解最初的设计切入点
- 不可作为 V2 最终状态依据

## V2 阶段完成后的代码落点

另一个窗口在读完上述历史后，应直接检索这些实现文件：

### obmm 设计文档

- `ucx\src\uct\obmm\DESIGN.md`

### obmm 核心实现

- `ucx\src\uct\obmm\base\obmm_md.h`
- `ucx\src\uct\obmm\base\obmm_md.c`
- `ucx\src\uct\obmm\base\obmm_pool.h`
- `ucx\src\uct\obmm\base\obmm_fifo.h`
- `ucx\src\uct\obmm\base\obmm_iface.h`
- `ucx\src\uct\obmm\base\obmm_iface.c`
- `ucx\src\uct\obmm\base\obmm_ep.h`
- `ucx\src\uct\obmm\base\obmm_ep.c`

## V2 阶段完成时的验证/复现资料

如果新窗口还要理解 V2 是如何被验证到“可用”的，继续检索这些根目录文件：

### MPI / OSU 测试入口

- `build_mpi_tests.sh`
- `run_mpi_tests.sh`
- `run_osu_tests.sh`
- `MPI_TESTS.md`

### V2 专用 MPI 用例

- `mpi_correctness_v2.c`
- `mpi_pingpong_v2.c`
- `mpi_bw_v2.c`
- `mpi_multi_v2.c`

### 早期兼容/恢复辅助

- `obmm_pool_reset.c`

## 最小可用阅读集

如果新窗口只想快速进入 V2 终态，最少读这些：

1. `AGENTS.md`
2. `.github\skills\obmm-api-and-env\SKILL.md`
3. `V2_CONTEXT\history\001-designing-obmm-transport-am-sh.md`
4. `V2_CONTEXT\history\008-implementing-v2-desc-paired-am.md`
5. `V2_CONTEXT\history\009-fixing-osu-rkey-ptr-segfault-t.md`
6. `ucx\src\uct\obmm\DESIGN.md`
7. `ucx\src\uct\obmm\base\obmm_{md,iface,ep,fifo,pool}.*`

## 截止范围

本目录的历史材料只包含：

- `001-...`
- `002-...`
- `003-...`
- `004-...`
- `005-...`
- `006-...`
- `007-...`
- `008-...`
- `009-...`
