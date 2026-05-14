# OBMM UCT Transport 详细设计文档

> **适用范围**：本文档面向参与 OBMM UCT Transport 开发或代码评审的工程师，用于团队串讲。
>
> **版本说明**：描述从阶段一骨架到阶段二（am\_short + am\_bcopy）完整实现的设计思路与关键决策。

---

## 目录

1. [项目背景与总体架构](#1-项目背景与总体架构)
2. [OBMM 硬件与软件环境](#2-obmm-硬件与软件环境)
3. [UCX 框架简介](#3-ucx-框架简介)
4. [整体模块划分](#4-整体模块划分)
5. [Memory Domain（MD）设计](#5-memory-domainmd设计)
6. [iface 设计](#6-iface-设计)
7. [FIFO 环形队列：核心数据结构](#7-fifo-环形队列核心数据结构)
8. [Endpoint（ep）设计](#8-endpointep设计)
9. [am\_short 实现](#9-am_short-实现)
10. [am\_bcopy 实现](#10-am_bcopy-实现)
11. [内存屏障与并发控制](#11-内存屏障与并发控制)
12. [Pending Arbiter 流控设计](#12-pending-arbiter-流控设计)
13. [MD 能力标志修复：禁止 RKEY/RNDV](#13-md-能力标志修复禁止-rkeyrndv)
14. [iface\_query 能力标志设计](#14-iface_query-能力标志设计)
15. [测试验证](#15-测试验证)
16. [已知遗留问题](#16-已知遗留问题)
17. [关键设计决策索引](#17-关键设计决策索引)

---

## 1. 项目背景与总体架构

### 1.1 背景

本项目的目标是在 UCX 框架的 UCT（Unified Communication Transport）层新增一个名为 `obmm` 的传输层实现，使 HPC 超算应用能够通过 OBMM 硬件实现跨节点共享内存通信，从而替代传统的网络传输路径，降低时延、提升带宽。

### 1.2 整体调用链

```
HPC 应用（osu_latency / osu_bw 等 MPI Benchmark）
    │
    ▼
OpenMPI / OMPI（MPI 层）
    │  使用 UCX PML（pml_ucx）
    ▼
UCP（UCX User-level Communication Protocol）
    │  根据能力选择传输层
    ▼
UCT obmm transport（本项目核心）
    │  通过 mmap 操作跨节点共享内存
    ▼
OBMM 共享内存（硬件：UB Memory 链路）
```

**核心原则**：
- 重点是 UCX 和 OBMM，不过度关注 MPI 内部实现。
- UCX 框架宏和钩子函数众多，所有结论**必须先查代码**，禁止"想当然"。
- 代码修改限定在 `ucx/src/uct/obmm/` 目录内，除非有明确理由需要改其他地方。

### 1.3 三个代码仓

| 代码仓 | 职责 |
|--------|------|
| `ucx/` | UCX 框架，包含 UCP/UCT/UCS 三层 |
| `obmm/` | OBMM 内核模块文档与 libobmm 用户态库 |
| `ompi/` | OpenMPI，作为 MPI 实现调用 UCX |

---

## 2. OBMM 硬件与软件环境

### 2.1 硬件拓扑

```
节点 0 (Node 0)                      节点 1 (Node 1)
┌─────────────────────┐              ┌─────────────────────┐
│  应用进程 (rank 0)   │              │  应用进程 (rank 1)   │
│                     │◄── UB 链路 ──►│                     │
│  export 128M 内存   │              │  export 128M 内存   │
│  import Node1 内存  │              │  import Node0 内存  │
└─────────────────────┘              └─────────────────────┘
```

### 2.2 内存访问模型

OBMM 采用**共享内存访问（mmap 模型）**：

- **export 端**：调用 `obmm_export()` 将本节点内存导出，获取 UBA（UB 域地址）和 `mem_id`。
- **import 端**：调用 `obmm_import()` 根据 UBA 引入远端内存，生成字符设备 `/dev/obmm_shmdev${mem_id}`。
- **访问方式**：通过 `open()` + `mmap()` 将字符设备映射到进程虚拟地址空间，之后用 `load/store` 指令直接访问远端内存。

### 2.3 生产环境约束

在我们的目标场景中：

1. **export/import 提前完成**：部署阶段已经完成跨节点的 export/import 操作，transport 代码内部**不做 export/unexport/import/unimport**。
2. **地址传递**：
   - export 端的 `desc.addr` 是 UBA（UB 域地址）。
   - import 端的 `desc.addr` 是本机物理地址（两者可以对应，但数值不等）。
3. **内存映射**：transport 代码内部使用 `mmap` 操作已经 import 的共享内存。
4. **缓存属性**：生产中使用 cacheable mmap 映射（非 `O_SYNC`），因此需要通过 `obmm_set_ownership()` 进行一致性维护（本 transport 框架阶段暂不调用，由上层应用负责）。
5. **大小**：每个节点 export 128M 内存；`FIFO_SIZE=64` 个槽位。

### 2.4 OBMM 内存寻址关键数据结构

```c
struct obmm_mem_desc {
    uint64_t addr;      /* export端：UBA；import端：本机PA */
    uint64_t length;    /* 内存大小 */
    uint8_t  seid[16];  /* 使用方 UB controller EID */
    uint8_t  deid[16];  /* 提供方 UB controller EID */
    uint32_t tokenid;   /* export 生成的令牌 */
    uint32_t scna;      /* 使用方 CNA */
    uint32_t dcna;      /* 提供方 CNA */
    uint16_t priv_len;  /* 私有数据长度 */
    uint8_t  priv[];    /* 私有数据 */
};
```

---

## 3. UCX 框架简介

### 3.1 UCX 分层架构

```
UCP（应用级接口）
 ├── UCS（工具/数据结构层）
 └── UCT（传输层抽象）
      ├── tcp/   ← TCP 传输（参考）
      ├── sm/mm/ ← 共享内存 mm（最重要的参考实现）
      └── obmm/  ← 本项目（新增）
```

### 3.2 UCT 核心对象

| 对象 | 职责 | 类比 |
|------|------|------|
| `uct_component_t` | 组件注册，MD 发现入口 | 驱动程序 |
| `uct_md_t` (Memory Domain) | 内存域，管理内存注册/注销/key | 内存控制器 |
| `uct_iface_t` (Interface) | 传输接口，持有 FIFO/资源/进度 | 网卡接口 |
| `uct_ep_t` (Endpoint) | 端点，对应一个对端连接 | Socket 连接 |

### 3.3 关键 UCT 钩子

```
uct_md_ops_t          → MD 操作表
uct_iface_ops_t       → iface 操作表（ep_am_short, ep_am_bcopy 等）
uct_iface_internal_ops_t → 内部操作表（is_reachable_v2 等）
```

### 3.4 am\_short 与 am\_bcopy 语义

| 方式 | 特点 | UCT 接口 |
|------|------|---------|
| am\_short | 数据直接内联在 FIFO element 中，零拷贝路径（sender 直写） | `uct_ep_am_short(id, header, payload, length)` |
| am\_bcopy | 通过 pack\_cb 回调由 sender 打包数据到 desc 区域 | `uct_ep_am_bcopy(id, pack_cb, arg, flags)` |

**UCT Active Message 语义**：
- Sender 调用 `uct_ep_am_short/bcopy`，写入远端 FIFO 槽位。
- Receiver 在 `iface_progress` 中轮询 FIFO，调用 `uct_iface_invoke_am` 触发 UCP 回调。
- `uct_iface_invoke_am(..., flags=0)` 表示 UCP 必须**同步消费**数据缓冲区，不能持有引用。

---

## 4. 整体模块划分

### 4.1 文件结构

```
ucx/src/uct/obmm/
├── base/
│   ├── obmm_md.c/h      # Memory Domain：能力声明、组件注册
│   ├── obmm_iface.c/h   # iface：FIFO 进度、能力查询、地址
│   ├── obmm_ep.c/h      # ep：TX 路径（am_short/am_bcopy）、pending
│   └── (obmm_pool.c/h)  # （可选）共享 FIFO 池的 attach/claim 逻辑
└── docs/
    ├── DESIGN.md         # 本文档
    ├── tl-design.md      # TL 注册设计
    ├── iface-design.md   # iface 骨架设计
    ├── md-design.md      # MD 骨架设计
    └── ep-design.md      # ep 骨架设计
```

### 4.2 对象关系图

```
uct_obmm_component
    └── uct_obmm_md_t
            └── uct_obmm_iface_t
                    ├── id (iface 唯一标识符)
                    ├── recv_ctl (本 iface 接收控制块，位于共享内存)
                    ├── fifo_elem_size / bcopy_seg_size
                    ├── fifo_size
                    ├── read_index (本 iface 消费进度)
                    ├── arbiter (pending 仲裁器)
                    └── uct_obmm_ep_t[]
                              ├── peer_ctl (对端接收控制块，mmap 到本进程)
                              ├── cached_tail (本 ep 缓存的对端 tail)
                              ├── arb_group (arbiter group)
                              └── fifo_elem_size / bcopy_seg_size (from ep)
```

---

## 5. Memory Domain（MD）设计

### 5.1 职责

MD 是 UCX 中组件级别的内存管理单元。对于 OBMM transport，MD 的职责是：
1. 向 UCX 声明本组件的存在（`query_md_resources`）。
2. 打开/关闭内存域（`md_open/close`）。
3. 向 UCP 广播本 transport 的**内存操作能力**（`md_query`）。

### 5.2 能力标志的关键设计决策

OBMM 的 MD 能力标志**不得包含** `UCT_MD_FLAG_REG | UCT_MD_FLAG_NEED_RKEY | UCT_MD_FLAG_RKEY_PTR`。

**原因**：若错误地声明了这些标志，UCP 在处理大消息时会选择 `rndv/rkey_ptr` 路径，最终调用：
```
ucp_proto_rndv_progress_rkey_ptr()
  → ucp_datatype_iter_unpack()
    → ucp_memcpy_unpack()
      → ucs_memcpy_relaxed()
        → memcpy_aarch64_sve()   ← SIGSEGV（访问 obmm 共享内存越界）
```

**正确做法**：MD 只声明 AM 相关的能力，不声明内存注册/远端 key 能力：
```c
static ucs_status_t uct_obmm_md_query(uct_md_h md, uct_md_attr_v2_t *attr) {
    uct_md_base_md_query(attr);
    /* 不设置 UCT_MD_FLAG_REG / NEED_RKEY / RKEY_PTR */
    attr->flags             = 0;
    attr->access_mem_types  = UCS_BIT(UCS_MEMORY_TYPE_HOST);
    return UCS_OK;
}
```

### 5.3 rkey\_unpack 的处理

虽然 MD 不声明 `NEED_RKEY`，但 `uct_obmm_component` 中仍需提供 `rkey_unpack` 回调（UCX 框架要求）。实现为 stub（直接返回成功，rkey=0）。

```c
ucs_status_t uct_obmm_md_rkey_unpack(...) {
    *rkey_p   = 0;
    *handle_p = NULL;
    return UCS_OK;
}
```

---

## 6. iface 设计

### 6.1 iface 生命周期

```
uct_iface_open()
    → UCS_CLASS_INIT_FUNC(uct_obmm_iface_t)
        ├── UCS_CLASS_CALL_SUPER_INIT(uct_sm_iface_t, ...)
        ├── 生成 id = ucs_generate_uuid()
        ├── 获取本 iface 的共享内存区域（mmap 或从 pool attach）
        ├── 初始化 FIFO 控制块（recv_ctl）
        └── 初始化 arbiter

iface_progress() 轮询循环（每个 worker tick 调用一次）
    ├── 按 read_index 检查 FIFO slot owner bit
    ├── 消费所有已 ready 的 slot
    └── 更新 tail，触发 arbiter dispatch

uct_iface_close()
    → UCS_CLASS_CLEANUP_FUNC(uct_obmm_iface_t)
        ├── arbiter 清理
        └── 解除 mmap / pool detach
```

### 6.2 iface 地址（`uct_obmm_iface_addr_t`）

类型为 `uint64_t`，值为 `ucs_generate_uuid((uintptr_t)self)`。

该 id 用于：
1. `iface_get_address()`：作为本 iface 地址对外广播。
2. `iface_is_reachable_v2()`：验证对端 iface 地址是否与本 iface id 匹配（**同节点可达性**）。
3. `ep_is_connected()`：验证 ep 连接的对端地址是否正确。

**注意**：对于跨节点通信，iface 可达性检查的关键在于 `uct_iface_scope_is_reachable()` 和 `UCT_IFACE_FLAG_INTER_NODE`，详见第 14 节。

### 6.3 iface 能力查询（iface\_query）

```c
static ucs_status_t uct_obmm_iface_query(uct_iface_h tl_iface,
                                          uct_iface_attr_t *attr) {
    size_t elem_hdr = sizeof(uct_obmm_fifo_element_t);

    attr->cap.flags = UCT_IFACE_FLAG_AM_SHORT    |
                      UCT_IFACE_FLAG_AM_BCOPY     |
                      UCT_IFACE_FLAG_PENDING      |
                      UCT_IFACE_FLAG_CONNECT_TO_IFACE |
                      UCT_IFACE_FLAG_CB_SYNC      |
                      UCT_IFACE_FLAG_INTER_NODE   |
                      UCT_IFACE_FLAG_EP_CHECK;

    attr->cap.am.max_short = iface->fifo_elem_size - elem_hdr;
    attr->cap.am.max_bcopy = iface->bcopy_seg_size;

    attr->iface_addr_len   = sizeof(uct_obmm_iface_addr_t);
    attr->device_addr_len  = uct_sm_iface_get_device_addr_len();
    attr->ep_addr_len      = 0;   /* CONNECT_TO_IFACE 模式无 ep 地址 */

    attr->latency          = UCS_LINEAR_FUNC_ZERO;
    attr->bandwidth.dedicated = iface->super.config.bandwidth;
    attr->overhead         = 100e-9;
    return UCS_OK;
}
```

---

## 7. FIFO 环形队列：核心数据结构

FIFO 环形队列是 OBMM UCT Transport 的通信核心。每个 iface（即每个接收方进程）拥有一个共享 FIFO，所有向该接收方发送消息的 sender 都写入同一个 FIFO。

### 7.1 物理布局

```
共享内存区域（节点间可见）：

┌─────────────────────────────────────────────────────────────┐
│  uct_obmm_recv_ctl_t                                        │
│  ├── head   (uint64_t, atomic) ← Sender 预约槽位用         │
│  └── tail   (uint64_t)        ← Receiver 更新，Sender 读   │
├─────────────────────────────────────────────────────────────┤
│  slot[0]:                                                   │
│  ├── uct_obmm_fifo_element_t  (FIFO 元素头部)               │
│  │   ├── flags    (uint8_t)   ← owner bit + BCOPY flag     │
│  │   ├── am_id   (uint8_t)   ← UCT AM ID                   │
│  │   ├── length  (uint16_t)  ← payload 长度                 │
│  │   ├── generation (uint32_t) ← 防止 stale 写入            │
│  │   └── header  (uint64_t)  ← am_short 的 8 字节 header   │
│  └── [inline payload 区域]   ← am_short 数据               │
│      (fifo_elem_size - sizeof(uct_obmm_fifo_element_t) 字节)│
├─────────────────────────────────────────────────────────────┤
│  desc[0]:  ← am_bcopy 专用 payload 区域（独立于 FIFO 元素） │
│  (bcopy_seg_size 字节)                                      │
├─────────────────────────────────────────────────────────────┤
│  slot[1] + desc[1]                                          │
│  ...                                                        │
├─────────────────────────────────────────────────────────────┤
│  slot[FIFO_SIZE-1] + desc[FIFO_SIZE-1]                     │
└─────────────────────────────────────────────────────────────┘
```

**关键尺寸参数**：
- `FIFO_SIZE = 64`（槽位数量）
- `fifo_elem_size`：每个 FIFO element（含 payload 内联区）的大小
- `bcopy_seg_size`：每个 desc 区域（bcopy payload）的大小
- `max_short = fifo_elem_size - sizeof(uct_obmm_fifo_element_t)`
- `max_bcopy = bcopy_seg_size`

**设计原则**：bcopy 的 payload **不挤占** FIFO element 主体，而是存放在 paired desc 区域中。可以牺牲一部分 short 最大长度来换取更大的 bcopy 大小（两者从同一块共享内存区域分配，调整 `fifo_elem_size` 和 `bcopy_seg_size` 即可）。

### 7.2 slot 寻址 helper

```c
/* 根据 slot_idx 获取 FIFO element 指针 */
static inline uct_obmm_fifo_element_t *
uct_obmm_slot_elem(uct_obmm_recv_ctl_t *recv_ctl, unsigned slot_idx) {
    /* 每个 slot 占 (fifo_elem_size + bcopy_seg_size) 字节 */
    uintptr_t base = (uintptr_t)(recv_ctl + 1);
    return (uct_obmm_fifo_element_t *)
           (base + slot_idx * (fifo_elem_size + bcopy_seg_size));
}

/* 根据 slot_idx 获取 bcopy desc 指针 */
static inline void *
uct_obmm_slot_desc(uct_obmm_recv_ctl_t *recv_ctl, unsigned slot_idx) {
    return (void *)((uintptr_t)uct_obmm_slot_elem(recv_ctl, slot_idx)
                    + fifo_elem_size);
}
```

### 7.3 Owner Bit 机制（关键并发控制）

FIFO 采用**所有权位（owner bit）**机制，实现 sender 写入与 receiver 消费之间的无锁同步：

```
owner bit = flags & UCT_OBMM_FIFO_ELEM_FLAG_OWNER

每圈翻转规则：
  第 0 圈 (lap=0)：owner bit 置 1，表示"本槽有效"
  第 1 圈 (lap=1)：owner bit 置 0，表示"本槽有效"
  第 2 圈 (lap=2)：owner bit 置 1，...

lap = head / FIFO_SIZE（写入时）或 read_index / FIFO_SIZE（读取时）

Sender 写入：
  expected_owner = ((head / FIFO_SIZE) & 1) ? 0 : OWNER_BIT

Receiver 期望：
  expected_owner = ((read_index / FIFO_SIZE) & 1) ? 0 : OWNER_BIT
```

**为什么用 owner bit 而不是简单的 filled/empty flag？**
因为 FIFO 是环形的，一个 flag 必须能区分"第 N 圈写入"和"第 N+1 圈还未写入"，用翻转的 owner bit 可以在无锁情况下做到这一点。

### 7.4 Generation 机制（防 stale 写入）

`generation` 字段是防止"幽灵写入"的第二道防线：

场景：Sender A 在预约 slot 后被调度走很久才真正写入，此时 receiver 以为 A 写完并消费了该 slot，slot 被重新分配给 Sender B，B 写完后 A 才写入——此时 A 的数据会覆盖 B 的数据。

`generation` 解决此问题：
- Sender 在 reserve slot 时，记录当前 slot 的 generation 值。
- Sender 在写入 `elem->flags` 之前，写入 `elem->generation = reserved_gen`。
- Receiver 在读取 slot 时，验证 `elem->generation == expected_generation`。
- 不匹配则认为这是一个 stale（陈旧）写入，丢弃并继续等待。

### 7.5 FIFO 满检测

```c
/* head - cached_tail >= FIFO_SIZE 时认为 FIFO 已满 */
if (head - ep->cached_tail >= iface->fifo_size) {
    /* 刷新 cached_tail（从共享内存读取最新 tail） */
    ep->cached_tail = recv_ctl->tail;
    if (head - ep->cached_tail >= iface->fifo_size)
        return UCS_ERR_NO_RESOURCE;  /* 真正满了 */
}
```

### 7.6 Slot 预约：CAS 而非 FAA

**Sender 预约 slot 必须使用 load + CAS（Compare-And-Swap），不能用 FAA（Fetch-And-Add）**。

原因：若使用 FAA，当 FIFO 满时无法回滚（FAA 已经使 head 前进），会在 FIFO 中留下永久空洞（gap），导致接收方的顺序消费无法越过该 gap 而永久阻塞。

CAS 失败则重试，或在 FIFO 满时直接返回 `UCS_ERR_NO_RESOURCE`，进入 pending 队列。

---

## 8. Endpoint（ep）设计

### 8.1 ep 创建与连接

ep 使用 `CONNECT_TO_IFACE` 模式（无独立 ep 地址）：

```c
UCS_CLASS_INIT_FUNC(uct_obmm_ep_t, const uct_ep_params_t *params) {
    UCT_EP_PARAMS_CHECK_DEV_IFACE_ADDRS(params);  /* 必须有 dev + iface addr */
    UCS_CLASS_CALL_SUPER_INIT(uct_base_ep_t, &iface->super.super);

    /* 将对端 iface 地址解析为本进程可访问的共享内存指针 */
    peer_memid = /* 从 iface_addr 查找对应的 import memid */;
    self->peer_ctl = /* mmap 对端共享内存区域 */;
    self->cached_tail = self->peer_ctl->tail;
    return UCS_OK;
}
```

### 8.2 ep 结构体关键字段

```c
typedef struct uct_obmm_ep {
    uct_base_ep_t         super;
    uct_obmm_recv_ctl_t  *peer_ctl;       /* 对端接收控制块（mmap 映射到本进程） */
    uint64_t              cached_tail;    /* 缓存的对端 tail，减少共享内存读 */
    ucs_arbiter_group_t   arb_group;      /* pending arbiter group */
    size_t                fifo_elem_size; /* 从 iface 复制，避免 deref iface 开销 */
    size_t                bcopy_seg_size;
} uct_obmm_ep_t;
```

---

## 9. am\_short 实现

### 9.1 TX 路径（Sender 端）

```c
ucs_status_t uct_obmm_ep_am_short(uct_ep_h tl_ep, uint8_t id,
                                   uint64_t header,
                                   const void *payload, unsigned length) {
    uct_obmm_ep_t    *ep    = ucs_derived_of(tl_ep, uct_obmm_ep_t);
    uct_obmm_iface_t *iface = ucs_derived_of(ep->super.super.iface, ...);

    /* 1. 参数校验 */
    UCT_CHECK_AM_ID(id);
    UCT_CHECK_LENGTH(sizeof(header) + length, 0,
                     ep->fifo_elem_size - sizeof(uct_obmm_fifo_element_t),
                     "am_short");

    /* 2. CAS 预约槽位（自旋重试，FIFO 满则返回 NO_RESOURCE） */
    uint64_t head      = ucs_atomic_fadd64(&peer_ctl->head, 0); /* load */
    /* ... CAS loop ... */
    unsigned slot_idx  = head % iface->fifo_size;
    uint8_t  owner_bit = ((head / iface->fifo_size) & 1) ? 0 : OWNER_BIT;

    /* 3. 获取 elem 指针，写入数据 */
    elem = uct_obmm_slot_elem(peer_ctl, slot_idx);
    elem->am_id      = id;
    elem->length     = length;
    elem->generation = reserved_gen;
    elem->header     = header;
    memcpy(&elem->header + 1, payload, length);  /* 内联数据写入 */

    /* 4. Store fence + 写入 flags（发布） */
    ucs_memory_bus_store_fence();  /* dmb oshst on aarch64 */
    elem->flags = owner_bit;       /* 不带 BCOPY flag */

    /* 5. Trace */
    uct_iface_trace_am(&iface->super.super, UCT_AM_TRACE_TYPE_SEND,
                       id, &elem->header, sizeof(header) + length,
                       "TX: AM_SHORT");

    UCT_TL_EP_STAT_OP(&ep->super, AM, SHORT, sizeof(header) + length);
    return UCS_OK;
}
```

### 9.2 RX 路径（Receiver 端，在 iface\_progress 中）

```c
/* iface_progress 中的消费逻辑（简化伪代码） */
while (polled < max_batch) {
    unsigned slot_idx   = read_index % fifo_size;
    unsigned lap        = read_index / fifo_size;
    uint8_t  exp_owner  = (lap & 1) ? 0 : OWNER_BIT;

    elem  = uct_obmm_slot_elem(recv_ctl, slot_idx);
    flags = elem->flags;

    /* 检查 owner bit：不匹配则该 slot 尚未被 sender 写完，停止 */
    if ((flags & OWNER_BIT) != exp_owner)
        break;

    /* Load fence：确保后续读取看到 sender 写入的完整数据 */
    ucs_memory_bus_load_fence();  /* dmb oshld on aarch64 */

    /* RX 健全性检查：防止 FIFO corruption 静默传递 */
    if (ucs_unlikely(elem->length > max_len)) {
        ucs_fatal("obmm RX corruption: ...");
    }

    /* Generation 检查：丢弃 stale 写入 */
    if (elem->generation != expected_gen)
        goto next;

    if (flags & BCOPY_FLAG) {
        desc = uct_obmm_slot_desc(recv_ctl, slot_idx);
        uct_iface_trace_am(..., "RX: AM_BCOPY");
        uct_iface_invoke_am(&iface->super.super, elem->am_id,
                            desc, elem->length, 0);
    } else {
        uct_iface_trace_am(..., "RX: AM_SHORT");
        uct_iface_invoke_am(&iface->super.super, elem->am_id,
                            &elem->header, sizeof(header) + elem->length, 0);
    }

next:
    read_index++;
    polled++;
}

/* 更新 tail（允许 sender 看到进度，回收槽位） */
if (polled > 0) {
    uct_obmm_bus_full_fence();   /* full fence，确保 receiver 已读完 desc */
    recv_ctl->tail = read_index;
}
```

---

## 10. am\_bcopy 实现

### 10.1 设计思路

am\_bcopy 的 payload 比 am\_short 可以更大（超出 FIFO element 内联区域），因此 payload 不存放在 FIFO element 本体，而是存放在**配对的 desc 区域（paired desc）**中。

```
FIFO slot 布局（bcopy 情形）：
┌──────────────────────────────────┐
│ uct_obmm_fifo_element_t          │
│  ├── flags = OWNER | BCOPY_FLAG  │
│  ├── am_id                       │
│  ├── length (payload 大小)       │
│  └── generation                  │
│  (header 字段未使用或置 0)        │
├──────────────────────────────────┤
│ [内联 payload 区：未使用]         │
├──────────────────────────────────┤   ← fifo_elem_size 边界
│ desc 区域（bcopy_seg_size 字节） │   ← payload 实际存放处
│ [pack_cb 写入的数据]              │
└──────────────────────────────────┘
```

### 10.2 TX 路径

```c
ssize_t uct_obmm_ep_am_bcopy(uct_ep_h tl_ep, uint8_t id,
                               uct_pack_callback_t pack_cb,
                               void *arg, unsigned flags) {
    /* 1. 参数校验 */
    UCT_CHECK_AM_ID(id);

    /* 2. 预约槽位（同 am_short 的 CAS 逻辑） */
    slot_idx = uct_obmm_ep_reserve_slot(ep, &owner_bit);
    if (slot_idx < 0) return UCS_ERR_NO_RESOURCE;

    /* 3. 获取 elem 和 desc 指针 */
    elem = uct_obmm_slot_elem(peer_ctl, slot_idx);
    desc = uct_obmm_slot_desc(peer_ctl, slot_idx);

    /* 4. 调用 pack_cb 将数据打包到 desc 区域 */
    length = pack_cb(desc, arg);
    ucs_assert(length <= ep->bcopy_seg_size);
    ucs_assert(length <= UINT16_MAX);

    /* 5. 写入控制字段 */
    elem->am_id      = id;
    elem->length     = (uint16_t)length;
    elem->generation = reserved_gen;

    /* 6. Store fence + 发布（带 BCOPY 标志） */
    ucs_memory_bus_store_fence();
    elem->flags = owner_bit | UCT_OBMM_FIFO_ELEM_FLAG_BCOPY;

    uct_iface_trace_am(..., "TX: AM_BCOPY");
    UCT_TL_EP_STAT_OP(&ep->super, AM, BCOPY, length);
    return length;
}
```

### 10.3 RX 路径（在 iface\_progress 中）

见第 9.2 节中 `if (flags & BCOPY_FLAG)` 分支：receiver 通过 `uct_obmm_slot_desc()` 获取 desc 指针，直接传给 `uct_iface_invoke_am()`。

**关键约束**：`uct_iface_invoke_am` 调用 flags=0，表示 UCP 必须同步消费数据，不能持有 desc 引用。因此 receiver 在 `invoke_am` 返回后即可认为 desc 已被消费，可以在下一圈覆盖。

### 10.4 Tail 更新与 Full Fence

```
消费完成后，Receiver 更新 tail：
    uct_obmm_bus_full_fence()   ← 保证 receiver 已经读完 desc 内容
    recv_ctl->tail = read_index ← Sender 看到 tail 前进后才能复用槽位

为什么需要 full fence 而不是 store fence？
  如果只用 store fence，CPU 可能将 tail 的 store 重排到 desc 读取之前。
  Sender 看到 tail 前进后立即覆写 desc，而 receiver 还未读完 desc，导致数据竞争。
  Full fence 确保 receiver 的所有 load（包括 desc 读取）在 tail store 之前完成。
```

---

## 11. 内存屏障与并发控制

### 11.1 aarch64 内存模型背景

aarch64（ARM64）采用**弱内存模型**，CPU 和编译器可以对 load/store 进行重排。在共享内存并发场景中，必须显式插入内存屏障。

UCX 在 aarch64 上定义：

```c
/* ucx/src/ucs/arch/aarch64/cpu.h */
ucs_memory_bus_store_fence()  → dmb oshst  (OuterShareable Store fence)
ucs_memory_bus_load_fence()   → dmb oshld  (OuterShareable Load fence)
uct_obmm_bus_full_fence()     → dmb osh    (OuterShareable full fence)
```

### 11.2 Sender 侧屏障

```
写入 elem 非 flags 字段（header, payload, am_id, length, generation）
    ↓
ucs_memory_bus_store_fence()   ← 确保上述 store 对远端可见
    ↓
elem->flags = owner_bit        ← 发布：receiver 看到 owner bit 则认为数据已就绪
```

**逻辑**：flags 是发布标志，必须在数据完全写入之后才发布，否则 receiver 读到的数据可能不完整。

### 11.3 Receiver 侧屏障

```
读取 elem->flags，发现 owner bit 匹配（slot 就绪）
    ↓
ucs_memory_bus_load_fence()    ← 确保后续 load 不被重排到 flags 读取之前
    ↓
读取 elem->am_id, elem->length, elem->header, payload/desc
    ↓
uct_iface_invoke_am()          ← 传给 UCP 回调
    ↓
uct_obmm_bus_full_fence()      ← 确保 desc 读取完成再更新 tail
    ↓
recv_ctl->tail = read_index    ← 允许 sender 复用槽位
```

### 11.4 并发写入的 CAS 协议

多个 sender 并发写入同一个 receiver 的 FIFO，通过对 `recv_ctl->head` 的原子 CAS 来争抢槽位：

```c
uint64_t old_head = ucs_atomic_fadd64(&peer_ctl->head, 0);  /* 读当前 head */
do {
    /* 检查 FIFO 是否满 */
    if (old_head - ep->cached_tail >= fifo_size) {
        ep->cached_tail = recv_ctl->tail;  /* 刷新 cached tail */
        if (old_head - ep->cached_tail >= fifo_size)
            return UCS_ERR_NO_RESOURCE;
    }
    new_head = old_head + 1;
} while (!ucs_atomic_cswap64(&peer_ctl->head, old_head, new_head, &old_head));

/* 成功：本 sender 独占 old_head % fifo_size 槽位 */
```

---

## 12. Pending Arbiter 流控设计

### 12.1 背景与问题

当 FIFO 满时，`ep_am_short/bcopy` 返回 `UCS_ERR_NO_RESOURCE`。UCP 收到后会调用 `ep_pending_add` 将操作加入 pending 队列，等待 FIFO 有空间后重试。

**早期实现的错误**：`pending_add` 在有资源时直接返回 `UCS_ERR_BUSY`（让 UCP 立即重试），在无资源时也没有正确入 arbiter。这在双向高负载场景（如 `osu_bibw`）下导致 **livelock**：双方都在旋转重试 pending，互相抢占 FIFO，实际进展为零，表现为 `size=4096` 时程序卡死。

### 12.2 正确的 Pending Arbiter 设计

```c
/* iface 结构体中 */
ucs_arbiter_t arbiter;

/* ep 结构体中 */
ucs_arbiter_group_t arb_group;

/* pending_add */
ucs_status_t uct_obmm_ep_pending_add(uct_ep_h tl_ep,
                                      uct_pending_req_t *req,
                                      unsigned flags) {
    uct_obmm_ep_t    *ep    = ucs_derived_of(tl_ep, uct_obmm_ep_t);
    uct_obmm_iface_t *iface = ucs_derived_of(ep->super.super.iface, ...);

    /* 如果当前有资源，返回 BUSY 让 UCP 直接重试（快路径） */
    if (uct_obmm_ep_has_tx_resource(ep))
        return UCS_ERR_BUSY;

    /* 无资源：加入 arbiter，等待 iface_progress 分配 */
    ucs_arbiter_group_push_req(&ep->arb_group, req);
    ucs_arbiter_group_schedule(&iface->arbiter, &ep->arb_group);
    return UCS_OK;
}

/* iface_progress 末尾触发 arbiter */
ucs_arbiter_dispatch(&iface->arbiter, 1,
                     uct_obmm_ep_process_pending, &polled);
```

**关键**：只有当真正无资源时才入 arbiter，有资源时返回 BUSY 让 UCP 直接重试，避免不必要的 arbiter 开销。

### 12.3 Pending Purge

当 ep 销毁时，需要清空该 ep 的 pending 队列：

```c
void uct_obmm_ep_pending_purge(uct_ep_h tl_ep,
                                uct_pending_purge_callback_t cb, void *arg) {
    ucs_arbiter_group_purge(&iface->arbiter, &ep->arb_group, cb, arg);
}
```

---

## 13. MD 能力标志修复：禁止 RKEY/RNDV

### 13.1 问题现象

`osu_latency` 在消息大小为 262144 字节（256K）时稳定出现 SIGSEGV，调用栈：

```
ucp_proto_rndv_progress_rkey_ptr()
  → ucp_datatype_iter_unpack()
    → ucp_dt_contig_unpack()
      → ucp_memcpy_unpack()
        → ucs_memcpy_relaxed()
          → memcpy_aarch64_sve()   ← SIGSEGV
```

### 13.2 根因分析

MD 错误地声明了 `UCT_MD_FLAG_REG | UCT_MD_FLAG_NEED_RKEY | UCT_MD_FLAG_RKEY_PTR`。UCP 看到这些标志后，对大消息选择了 `rndv/rkey_ptr` 协议，尝试通过 rkey 访问共享内存，而 OBMM 根本不支持这种访问方式，导致访问越界/非法内存。

### 13.3 修复方案

移除 MD 中上述三个标志，MD 只声明 transport 实际支持的能力（AM 发送），不声明任何内存注册/远端 key 能力。

修复后：UCP 对大消息不会再选择 rndv/rkey_ptr 路径，而是通过 AM（am\_bcopy 分片）传输，行为正确。

---

## 14. iface\_query 能力标志设计

### 14.1 关键标志说明

| 标志 | 作用 |
|------|------|
| `UCT_IFACE_FLAG_AM_SHORT` | 支持 am\_short，UCP 可用于小消息 eager 发送 |
| `UCT_IFACE_FLAG_AM_BCOPY` | 支持 am\_bcopy，UCP 可用于较大消息（有 pack callback） |
| `UCT_IFACE_FLAG_PENDING` | 支持 pending 队列，UCP 在 NO\_RESOURCE 时调用 pending\_add |
| `UCT_IFACE_FLAG_CONNECT_TO_IFACE` | 无需 ep 地址，直接连接到对端 iface 地址 |
| `UCT_IFACE_FLAG_CB_SYNC` | AM 回调在 iface\_progress 的同步上下文中调用 |
| `UCT_IFACE_FLAG_INTER_NODE` | **跨节点传输**，使 OMPI 在跨节点 worker 地址中包含该传输层 |
| `UCT_IFACE_FLAG_EP_CHECK` | 支持 ep 连接状态检查 |

### 14.2 INTER\_NODE 标志的重要性

若不设置 `UCT_IFACE_FLAG_INTER_NODE`，OMPI 在构建跨节点 worker 地址时（`UCP_WORKER_ADDRESS_FLAG_NET_ONLY` 过滤）会**静默地将 obmm 排除**，导致跨节点通信根本不使用 obmm transport，问题往往表现为"通信仍工作但走了 TCP"，极难排查。

相关代码：
- `ompi/ompi/mca/pml/ucx/pml_ucx.c:160`：对 PMIX\_REMOTE 使用 `UCP_WORKER_ADDRESS_FLAG_NET_ONLY`
- `ucx/src/ucp/core/ucp_worker.c:2962-2969`：`NET_ONLY` 过滤 `tl_bitmap` 时按 `UCT_IFACE_FLAG_INTER_NODE` 筛选

---

## 15. 测试验证

### 15.1 测试环境

- 两节点，每节点 16 进程（共 32 进程）
- 架构：aarch64（鲲鹏/昇腾服务器）
- 节点间通过 OBMM UB Memory 链路互连

### 15.2 单节点 MPI 测试（V1）

自编 MPI 测试用例（`run_mpi_tests.sh`），覆盖 am\_short 基本收发，单节点测试通过。

### 15.3 两节点 MPI 测试（V1）

`run_mpi_tests.sh` 两节点全部通过，无报错。

### 15.4 两节点 MPI 测试（V2，含 am\_bcopy）

实现 am\_bcopy 后新增 V2 测试用例，覆盖 am\_short 和 am\_bcopy，两节点一次性通过。

### 15.5 OSU Micro-Benchmark 测试结果

| 测试项 | 进程数 | 结果 | 备注 |
|--------|--------|------|------|
| `osu_latency` | 2 | ✅ 通过 | 修复 rkey/rndv bug 后 |
| `osu_bw` | 2 | ✅ 通过 | 修复 rkey/rndv bug 后 |
| `osu_bibw` | 2 | ✅ 通过 | 修复 pending livelock 后 |
| `osu_allreduce` | 32 | ✅ 通过 | |
| `osu_allgather` | 32 | ✅ 通过 | |
| `osu_gather` | 32 | ⚠️ 概率性 SIGSEGV | 见第 16 节 |
| `osu_gatherv` | 32 | ⚠️ 概率性 SIGSEGV | 见第 16 节 |

**OSU 路径**：`/osu/collective/osu_gather`、`/osu/pt2pt/osu_latency` 等。

---

## 16. 已知遗留问题

### 16.1 osu\_gather / osu\_gatherv 概率性 SIGSEGV

**现象**：
- `np=32`（两节点各 16 进程）
- size 从小到大 sweep 时，约 50% 概率触发 SIGSEGV
- 崩溃一般发生在 size ≈ 0.2M 附近（但不固定）
- 单独跑 0.2M 很少崩溃
- 使用 TCP 传输不崩溃（obmm-specific）

**崩溃调用栈**：
```
uct_iface_invoke_am()
  → ucp_wireup_msg_handler()
    → ucp_wireup_process_reply() / ucp_wireup_process_request()
      → ucp_ep_match_retrieve()   ← SIGSEGV
```

**推断根因**：
- `np=32` 多 sender 并发下，receiver 端 FIFO 存在热点竞争。
- 怀疑某些情况下收到了损坏或错位的 wireup AM 消息，UCP 将消息中的 ep id 解析为野指针，在 `ucp_ep_match_retrieve` 中访问非法内存。
- TCP 不崩说明是 OBMM 特有的问题，可能是 FIFO 写入/读取的并发时序问题。

**已加入 RX 诊断代码**：
```c
/* iface_progress 中，load fence 之后，invoke_am 之前 */
unsigned max_len = (flags & UCT_OBMM_FIFO_ELEM_FLAG_BCOPY) ?
                    iface->bcopy_seg_size :
                    (iface->fifo_elem_size - sizeof(uct_obmm_fifo_element_t));
if (ucs_unlikely(elem->length > max_len)) {
    ucs_fatal("obmm RX corruption: read_index=%lu slot_idx=%u "
              "flags=0x%02x am_id=%u length=%u > max=%u "
              "gen=%u expected_gen=%u", ...);
}
```

**待完成工作**：
1. 在测试节点 build 并运行 `osu_gather`，观察 fatal 输出（如有）。
2. 若无 fatal 触发，以 `UCX_LOG_LEVEL=data` 重跑，提取崩溃前后 200 行日志。
3. 根据日志定位是否为 FIFO 数据损坏、generation 错误、am_id 越界等。

### 16.2 pool take\_over CAS 缺失（次要 bug）

`uct_obmm_pool_try_claim()` 中的 take\_over（dead owner scavenging）路径未重新 CAS bit，理论上两个 scavenger 可能同时 claim 同一个 slot。在稳定 benchmark（无进程 churn）时不太可能触发，但在进程频繁退出的生产场景下有风险，需要后续修复。

### 16.3 超规格 AM 消息分片（暂时搁置）

当消息大小超过 `max_bcopy` 时，UCP 可能对消息进行分片（多个 am\_bcopy 调用）。目前该路径的稳定性未经系统验证，已知存在潜在 bug，视作 OSU 测试通过，后续版本处理。

---

## 17. 关键设计决策索引

| 编号 | 决策 | 原因 |
|------|------|------|
| D1 | slot 预约用 CAS 不用 FAA | FAA 失败无法回滚，留下永久 gap |
| D2 | Owner bit 每圈翻转 | 区分"本圈有效"和"下圈未写入" |
| D3 | generation 防 stale 写入 | Sender 被调度换出时防止旧数据覆盖新数据 |
| D4 | bcopy payload 存入 desc 区域 | 不占用 FIFO element 主体，保持 short 内联简单 |
| D5 | tail 更新前 full fence | 防止 CPU 重排 tail store 到 desc load 之前 |
| D6 | sender 用 store fence 发布 flags | 确保数据对 receiver 可见后才发布 owner bit |
| D7 | receiver 用 load fence 读数据 | 确保读取数据不被重排到 flags 读取之前 |
| D8 | MD 不声明 REG/NEED\_RKEY/RKEY\_PTR | 防止 UCP 选择 rndv/rkey\_ptr 路径导致 SIGSEGV |
| D9 | 声明 INTER\_NODE flag | 防止 OMPI 的 NET\_ONLY 过滤静默排除本 transport |
| D10 | pending\_add 有资源时返回 BUSY | 避免 arbiter 开销；无资源时入 arbiter 防 livelock |
| D11 | RX invoke\_am flags=0 | UCT 同步语义：UCP 必须在 invoke 返回前消费数据 |
| D12 | 不在代码中做 export/import/unimport | 生产环境提前完成，transport 内只做 mmap 访问 |

---

*文档版本：2026-05*
*维护者：OBMM UCT Transport 开发组*
