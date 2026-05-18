# V2 实现检视：从 MPI 调用一路走到 obmm FIFO / desc

这份文档不是再讲一遍抽象设计，而是把 **V2 真正在跑时的通信路径** 拆开看：一次 `MPI_Send/MPI_Recv` 或 `MPI_Isend/Irecv`，最后是怎样落到 `ucx\src\uct\obmm\base\` 里的哪些代码上，以及每块代码到底在干什么。

如果只记一句话，应该是：

> **OMPI 负责把 MPI 调用变成 UCP tag send/recv；UCP 负责决定 short / bcopy / multi / rndv；obmm UCT 只负责“把一个 AM fragment 放进 peer 的 slot 里”以及“从自己的 slot 里把 AM fragment 取出来”。**

---

## 1. 先建立脑图：V2 的三层分工

| 层 | 主要职责 | 关键代码 |
| --- | --- | --- |
| OMPI / PML UCX | 把 `MPI_Send` / `MPI_Recv` / `MPI_Isend` / `MPI_Irecv` 变成 UCP tag API | `ompi\ompi\mca\pml\ucx\pml_ucx.c:636-670, 899-1060` |
| UCP | 根据消息大小、协议阈值、lane 能力，决定走 `am_short`、`am_bcopy`、多片 bcopy，或 rendezvous | `ucx\src\ucp\tag\tag_send.c:150-307`, `ucx\src\ucp\tag\eager_snd.c:117-186`, `ucx\src\ucp\proto\proto_am.inl:75-198`, `ucx\src\ucp\rndv\rndv.c:1828-1852` |
| UCT obmm | 管理 OBMM region / pool / slot / FIFO / paired desc，真正发布和轮询 AM fragment | `ucx\src\uct\obmm\base\obmm_{sysfs,region,md,pool,fifo,iface,ep}.*` |

V2 最重要的认识点是：**“大消息怎么发”主要是 UCP 的工作，obmm transport 只实现了 UCP 需要的几个底层原语。**  
所以理解 V2 时，不要把“4096B/64KiB/1MiB 消息”直接等价成“obmm 有 3 套发送代码”；真正变化的是 **UCP 如何重复调用 `uct_ep_am_short` / `uct_ep_am_bcopy`**。

---

## 2. 消息发送前，obmm 先做了什么

这一段如果没看懂，后面的 `am_short` / `am_bcopy` 很容易看成“凭空知道 peer 地址”。

### 2.1 MD 打开时：扫描 sysfs，识别 export/import，再把 shmdev mmap 进来

1. `uct_obmm_sysfs_discover()` 扫描 `/sys/devices/obmm/` 下的 `obmm_shmdev*`。  
   这里把每个设备解析成 `uct_obmm_dev_info_t`，包含：
   - `memid`
   - `type`（export / import）
   - `exporter_dcna`
   - `exporter_deid`
   - `allow_mmap`
   - `dev_path`
   
   关键代码：`ucx\src\uct\obmm\base\obmm_sysfs.c:127-210, 306-335`，结构定义在 `obmm_sysfs.h:34-56`。

2. `uct_obmm_md_open()` 调 `uct_obmm_md_map_devices()`，把所有可用 shmdev 都映射成 `uct_obmm_region_t`。  
   关键代码：`ucx\src\uct\obmm\base\obmm_md.c:68-117, 119-178`。

3. `uct_obmm_region_open()` 用 `open(..., O_RDWR | O_SYNC | O_CLOEXEC)` + `mmap(MAP_SHARED)` 建立 NC 映射。  
   关键代码：`ucx\src\uct\obmm\base\obmm_region.c:23-67`，结构定义在 `obmm_region.h:16-29`。

**理解点：**  
到这里为止，obmm MD 只是把“本进程能看到的 export/import region”都映射好了。它还没有创建任何 FIFO slot，也还没有和某个 peer 建立 ep。

### 2.2 iface 创建时：在本地 export region 里占一个 slot，作为“我的接收邮箱”

`UCS_CLASS_INIT_FUNC(uct_obmm_iface_t)` 做了 V2 数据面的本地初始化，关键代码在  
`ucx\src\uct\obmm\base\obmm_iface.c:317-457`。

它做的事可以直接按顺序看：

1. 校验几何参数：`FIFO_SIZE`、`FIFO_ELEM_SIZE`、`BCOPY_SEG_SIZE`  
   见 `obmm_iface.c:329-362`。
2. 找到本地 export region；没有 export region 就不能创建 iface  
   见 `obmm_iface.c:364-369`。
3. 用 `uct_obmm_slot_stride()` 计算单个 slot 的总大小：  
   `ctl + fifo_size * elem_size + fifo_size * bcopy_seg_size`  
   见 `obmm_fifo.h:63-76` 与 `obmm_iface.c:376-395`。
4. `uct_obmm_pool_attach()` 把 region 头部视作共享 pool；  
   `uct_obmm_pool_alloc_slot()` 从里面占一个 slot。  
   见 `obmm_iface.c:423-437`，pool 结构在 `obmm_pool.h:41-143`。
5. 从 slot base 派生出：
   - `recv_ctl`
   - `recv_elems`
   - `recv_descs`
   
   见 `obmm_iface.c:439-442` 和 `obmm_fifo.h:79-122`。

**这时可以把 iface 理解成：**

- `recv_ctl`：我的 FIFO 的 `head/tail`
- `recv_elems`：我的 FIFO 元素数组
- `recv_descs`：V2 新增的 paired-desc 数组
- `slot_index + generation`：我的“邮箱编号”和“这一代邮箱令牌”

### 2.2.1 这里真正的地基不是 FIFO，而是 `obmm_pool`

如果只看 `obmm_ep.c` / `obmm_iface.c`，很容易产生一种错觉：  
好像 obmm 一上来就已经“天然拥有一个 slot”了。

实际上不是。**slot 先由 `obmm_pool` 在 region 头部统一管理，然后 iface 只是向 pool 申请其中一个 slot。**

`obmm_pool.h` 定义得非常直白：`obmm_pool` 占据 export region 的头部，顺序是：

1. `uct_obmm_pool_hdr_t`
2. `alloc_bitmap[bitmap_words]`
3. `slot_meta[slot_count]`
4. `slot[0..slot_count-1]`

关键定义：`ucx\src\uct\obmm\base\obmm_pool.h:17-87`

这里每一层各自回答不同的问题：

- `pool_hdr`
  - 这个 region 里的 pool 是否已经初始化
  - pool version / slot_count / slot_size 是多少
  - slot 数组从 region 的哪个 offset 开始
- `alloc_bitmap`
  - 哪些 slot 当前已经被占用
- `slot_meta`
  - 这个 slot 当前是 `FREE / IN_USE / DEAD`
  - 当前是哪一代 `generation`
  - 谁拥有这个 slot（`owner_pid / owner_starttime`）
- `slot[]`
  - 真正给 iface / ep 使用的数据区，也就是后面说的
    `recv_ctl / recv_elems / recv_descs`

**所以理解顺序最好是反过来的：**

> 不是“先有 FIFO，然后顺便加个 pool”；而是“先有 pool 管理 region 里的 slot 生命周期，然后某个 slot 的内部布局才是 FIFO + desc”。

### 2.2.2 `uct_obmm_pool_attach()` 在本地 export region 上到底干了什么

这一步对应 `obmm_iface.c:423-437` 调进去的  
`ucx\src\uct\obmm\base\obmm_pool.c:147-199`。

它不是简单地“记几个指针”，而是完成了下面这套跨进程初始化协议：

1. 先算 `required_size`
   - `uct_obmm_pool_required_size(slot_count, slot_size)`
   - 如果 region 连 pool 头 + bitmap + meta + slot 数组都放不下，直接报错  
     见 `obmm_pool.c:49-53, 159-165`

2. 再走 `uct_obmm_pool_init_or_wait()`
   - `hdr->state == UNINIT`：当前进程 CAS 抢到 `INITING`
   - `hdr->state == READY`：直接 load fence 后复用现成 pool
   - `hdr->state == INITING`：说明别的进程正在初始化，这时不能乱读半成品  
     见 `obmm_pool.c:77-144`

3. 如果自己赢了 `INITING`
   - 清零 `bitmap + slot_meta`
   - 写 `magic / version / slot_count / slot_size / slot_array_offset`
   - 写 `initializer_pid / initializer_starttime`
   - `ucs_memory_bus_store_fence()` 之后，最后才把 `state` 置成 `READY`  
     见 `obmm_pool.c:90-107`

4. 如果发现 pool 卡在 `INITING`
   - 会周期性检查 `initializer_pid + initializer_starttime` 对应的初始化者是不是还活着
   - 如果初始化者已经死了，就把状态重置回 `UNINIT`，再重新竞争初始化  
     见 `obmm_pool.c:126-143`

这里 `initializer_pid` / `initializer_starttime` 不是可有可无的调试字段。  
它们的真实作用是：**防止一个崩掉的初始化者把整个 pool 永久卡死在 `INITING`。**

### 2.2.3 `alloc_slot()` 不是“挑个空位”，而是建立 slot 所有权和世代号

`uct_obmm_pool_alloc_slot()` 最终调用 `uct_obmm_pool_try_claim()`，关键代码在  
`obmm_pool.c:202-249, 252-275`。

它做的事比“bitmap 里找个 0”复杂得多：

1. 先看 bitmap 对应 bit 是否已经置位
2. 如果没置位，就 CAS 抢占这个 slot
3. 如果已经置位，再看 `slot_meta`
   - `state == IN_USE` 且 owner 还活着：不能抢
   - owner 已死：允许 scavenge/take-over
4. 无论是新占用还是接管 dead owner，都会：
   - `generation += 1`
   - 重写 `owner_pid / owner_starttime`
   - `memset(slot bytes, 0, slot_size)`
   - store fence 后，`state = IN_USE`

**这一步为什么重要：**

- `generation` 不是装饰，它是“这一代 slot 令牌”
- 新 owner 接管 slot 前先 bump generation，意味着旧 owner 留下的 in-flight 写入会被后续 receiver 当作 stale message 丢掉
- `memset(slot bytes)` 放在 generation bump 之后，也是为了防止竞争中的旧写入“看起来像是新 slot 的合法内容”

换句话说，`obmm_pool` 真正解决的是：

> 这个 slot 现在归谁、已经换过几代 owner、如果远端 sender 还握着旧 ep 往这里写，会不会污染新 owner。

### 2.3 地址交换时：不是只交换一个 memid，而是交换“设备身份 + slot 身份 + 几何”

obmm 不是把一个裸指针发给 peer，而是交换两类地址：

1. `device_addr`：哪块 region  
   `uct_obmm_device_addr_t` 包含 `(exporter_dcna, exporter_deid_hi, exporter_deid_lo)`  
   见 `obmm_iface.h:29-33`，填充代码在 `obmm_iface.c:129-140`。

2. `iface_addr`：region 里的哪个 slot，以及 slot 几何  
   `uct_obmm_iface_addr_t` 包含  
   `(slot_index, generation, pid, fifo_size, fifo_elem_size, bcopy_seg_size)`  
   见 `obmm_iface.h:39-49`，填充代码在 `obmm_iface.c:143-156`。

**理解点：**

- `device_addr` 解决“去哪个 region 里找 peer”
- `iface_addr` 解决“进了 region 以后，去哪个 slot 找 peer 的 FIFO / desc”
- 几何字段让双方在 ep_create 时做二次校验，避免用错 slot 算法

### 2.4 ep 创建时：把 peer 的 slot 翻译成本地可 dereference 的指针

`UCS_CLASS_INIT_FUNC(uct_obmm_ep_t)` 的工作，本质上是把对端地址翻译成几个本地缓存指针。关键代码：  
`ucx\src\uct\obmm\base\obmm_ep.c:26-133`。

它做的事是：

1. 先校验 peer 的几何是否和本地一致  
   见 `obmm_ep.c:47-59`。
2. 用 `device_addr` 去 MD 里找对应 region：
   - self-loopback 优先匹配 export region
   - 否则去 imports 里找  
   
   见 `obmm_ep.c:61-89`，辅助函数在 `obmm_md.c:180-207`。
3. `uct_obmm_pool_open()` 打开 peer region 里的 pool  
    见 `obmm_ep.c:91-112`。
4. 用 `slot_index` 算出 peer slot 的 base，再缓存：
   - `peer_ctl`
   - `peer_elems`
   - `peer_descs`
   - `cached_tail`
   - `expected_generation`
    
    见 `obmm_ep.c:114-132`。

这里要特别注意：`uct_obmm_pool_open()` 和前面的 `uct_obmm_pool_attach()` 不是一回事。

- `attach()` 是“我要在本地 export region 上拥有/初始化这个 pool”
- `open()` 是“我只是 sender，我要确认 peer pool 已经 READY，然后把它映射成只读语义下可访问的结构指针”

`uct_obmm_pool_open()` 的关键代码在 `obmm_pool.c:305-359`，它做的事是：

1. 先读 `hdr->state`
2. `ucs_memory_bus_load_fence()`
3. 如果不是 `READY`，直接返回 `UCS_ERR_NO_RESOURCE`
4. 校验 `magic / version`
5. 校验 `(slot_count, slot_size)` 推出来的 `required_size` 没有超过 region
6. 最后只缓存：
   - `pool->bitmap`
   - `pool->meta`
   - `pool->slots`

**这里没有任何“占 slot”“改 bitmap”的动作。**  
所以可以把 `open()` 理解成：**peer pool 的只读 attach。**

**从这里开始，send path 就不再“找地址”了。**  
发送时直接操作 `ep->peer_ctl / peer_elems / peer_descs`。

### 2.4.1 为什么 `generation` 会在 pool 和 FIFO 两边都出现

这一点如果没想清楚，后面 recv progress 里那段 stale-check 会显得很突兀。

逻辑其实是这样的：

1. `obmm_pool` 在 slot meta 里维护“这个 slot 当前是哪一代 owner”
2. sender 在发每个 FIFO 元素时，把 `expected_generation` 填到 `elem->generation`
3. receiver 在 `iface_progress()` 里读取 elem 后，会检查：
   - 这个 elem 携带的 generation
   - 是否和“我当前这代 slot”一致
4. 如果不一致，说明这是旧 owner 时代残留的 stale 写入，直接跳过

所以：

- `obmm_pool.c` 决定 slot 生命周期
- `obmm_ep.c` 把 lifecycle token 写进每个发送元素
- `obmm_iface.c` 在接收路径真正执行“按世代丢弃旧消息”

这三块必须放在一起理解，`generation` 才不会看起来像一个凭空冒出来的字段。

---

## 3. 先看启动期一个关键坑：为什么 cross-node MPI_Init 以前会失败

这个问题很适合帮你建立对“capability bit 不是装饰”的直觉。

### 现象

单机 MPI 正常，跨节点 `MPI_Init` 失败。最终修复不是改 send/recv 逻辑，而是给 obmm iface 补了 `UCT_IFACE_FLAG_INTER_NODE`。

### 原因链

1. OMPI 对 **远端节点** 发布 worker address 时，使用的是 `UCP_WORKER_ADDRESS_FLAG_NET_ONLY`  
   见 `ompi\ompi\mca\pml\ucx\pml_ucx.c:133-170`，尤其是 `:159-166`。

2. UCP 在 `NET_ONLY` 模式下，只会把带 `UCT_IFACE_FLAG_INTER_NODE` 的 transport 放进地址里  
   见 `ucx\src\ucp\core\ucp_worker.c:2962-2969`。

3. 如果 remote address 里根本没有 obmm，wireup 选择阶段 `addr_index_map` 就是空的，最终打印 `"Unsupported operation"`  
   见 `ucx\src\ucp\wireup\select.c:478-481`。

4. 现在 obmm 在 `iface_query()` 里显式宣告了 `INTER_NODE`  
   见 `ucx\src\uct\obmm\base\obmm_iface.c:89-96`。

**理解点：**  
你以后看到“一个 capability bit 改了，整个 MPI_Init 都活了”不要惊讶。  
在 UCX 里，**上层协议是否愿意把你当成候选 transport**，首先看的是 capability 和 address packing，而不是你底层 send 函数写得多漂亮。

---

## 4. 例子一：`mpi_pingpong_v2.c` 的 64B / 2032B 消息，怎样走到 `am_short`

测试代码入口：`mpi_pingpong_v2.c:19-25, 62-91`。  
这里最适合观察 **小消息** 路径。

### 4.1 OMPI 层：MPI 调用变成 UCP tag send/recv

`MPI_Send` / `MPI_Recv` 最终分别落到：

- `mca_pml_ucx_send()`：`ompi\ompi\mca\pml\ucx\pml_ucx.c:1022-1060`
- `mca_pml_ucx_recv()`：`ompi\ompi\mca\pml\ucx\pml_ucx.c:673-720`

`mca_pml_ucx_send()` 会继续调用 `ucp_tag_send_nbx()` / `ucp_tag_send_nbr()`，  
`mca_pml_ucx_recv()` 会调用 `ucp_tag_recv_nbx()`。

### 4.2 UCP 层：小到可以 inline，就直接走 `uct_ep_am_short`

关键代码是 `ucp_tag_send_inline()`：  
`ucx\src\ucp\tag\tag_send.c:150-177`

这里如果消息足够小，就直接调用：

```c
uct_ep_am_short(..., UCP_AM_ID_EAGER_ONLY, tag, buffer, length)
```

也就是 `tag` 作为 8 字节 `header`，用户 buffer 作为 payload，直接下推给 UCT。

这就是为什么 obmm short 路径里那 8 字节 `header` 不是“自定义控制头”，而是 **UCP 的 tag header**。  
这个关系在 `tag_send.c:156-161` 的两个 `UCS_STATIC_ASSERT` 也说得很直白：`ucp_tag_t` 就是 8 字节。

### 4.3 obmm send 侧：`uct_obmm_ep_am_short()`

关键代码：`ucx\src\uct\obmm\base\obmm_ep.c:174-241`

它做的事情按顺序非常清楚：

1. `UCT_CHECK_AM_ID` / `UCT_CHECK_LENGTH`
2. 读 `peer_ctl->head`
3. 如果 `(head - cached_tail) >= fifo_size`，刷新 `tail`
4. 用 **load+CAS** 保留一个 slot，而不是 FAA  
   见 `obmm_ep.c:190-216`
5. 通过 `uct_obmm_slot_elem()` 算出 `elem[N]`
6. 写入：
   - `elem->am_id`
   - `elem->length`
   - `elem->generation`
   - `elem->header = tag`
   - `memcpy(elem + 1, payload, length)`
7. `ucs_memory_bus_store_fence()`
8. 最后写 `elem->flags = owner_bit`

### 4.4 这里最关键的两个理解点

#### A. short 的 payload 真的是写在 FIFO elem body 里的

这一点和 bcopy 不一样。short 直接把用户 payload 拷到 `elem + 1` 后面。  
所以 short path 的“数据区”就是 FIFO 元素本身。

#### B. **当前代码里** `elem->length` 对 short 存的是 `[8B tag][payload]` 总长度

这点要以代码为准：

- send 侧：`payload_total = sizeof(header) + length`，然后写入 `elem->length`  
  见 `obmm_ep.c:180-223`
- recv 侧：`uct_iface_invoke_am(..., &elem->header, elem->length, 0)`  
  见 `obmm_iface.c:264-268`

所以你阅读时可以直接把 short 元素理解成：

```text
[flags | am_id | length | generation | 8B tag][payload]
```

### 4.5 obmm recv 侧：`uct_obmm_iface_progress()`

关键代码：`ucx\src\uct\obmm\base\obmm_iface.c:221-295`

short 分支看这几行就够了：

1. 根据 `read_index` 计算当前应该期待的 owner bit  
   见 `obmm_iface.c:234-243`
2. 看 owner bit 是否已经翻转；没翻转就说明没有新消息
3. `ucs_memory_bus_load_fence()`
4. 用 `generation` 过滤 stale slot 写入  
   见 `obmm_iface.c:248-254`
5. short 分支把 `&elem->header` 和 `elem->length` 交给 AM handler  
   见 `obmm_iface.c:264-268`
6. 如果这轮有进展，`uct_obmm_bus_full_fence()` 后再推进 `tail`  
   见 `obmm_iface.c:275-283`

### 4.6 从 UCT 回到 UCP：`uct_iface_invoke_am()` -> `ucp_eager_only_handler()`

1. `uct_iface_invoke_am()` 只是根据 `am_id` 找到注册 handler 并调用  
   见 `ucx\src\uct\base\uct_iface.h:934-956`
2. 对于 `UCP_AM_ID_EAGER_ONLY`，UCP 注册的是 `ucp_eager_only_handler()`  
   见 `ucx\src\ucp\tag\eager_rcv.c:138-147, 554-556`
3. `ucp_eager_only_handler()` 最终走到 `ucp_eager_tagged_handler()`，它会：
   - 解析 eager 头里的 tag
   - 查找是否已有 posted receive
   - 有则拷贝到用户 buffer；没有则转 unexpected  
   
   见 `ucx\src\ucp\tag\eager_rcv.c:78-136`
4. `ucp_tag_recv_nbx()` 就是 posted receive 入口  
   见 `ucx\src\ucp\tag\tag_recv.c:224-252`

**把这一整条链串起来，你就能理解：**

`MPI_Send(64B)` 并不是“MPI 直接把 64B 写进 obmm FIFO”。  
它实际上是：

```text
MPI_Send
  -> mca_pml_ucx_send
    -> ucp_tag_send_nbx
      -> ucp_tag_send_inline
        -> uct_ep_am_short
          -> uct_obmm_ep_am_short
            -> 写 peer elem[N]
...
peer progress
  -> uct_obmm_iface_progress
    -> uct_iface_invoke_am(UCP_AM_ID_EAGER_ONLY)
      -> ucp_eager_only_handler
        -> 匹配 posted recv / unexpected
          -> MPI_Recv 返回
```

---

## 5. 例子二：`mpi_correctness_v2.c` 的 2033B / 4096B 消息，怎样走到 V2 paired-desc `am_bcopy`

测试代码入口：`mpi_correctness_v2.c:28-38, 85-137`。  
这个测试专门扫了：

- `2031 / 2032 / 2033`
- `4095 / 4096 / 4097`

所以它是理解 V2 的最佳样例。

### 5.1 UCP 层：short 不够，就走 eager bcopy

关键代码：`ucx\src\ucp\tag\eager_snd.c:130-146`

单片 bcopy 路径是：

```text
ucp_tag_eager_bcopy_single
  -> ucp_do_am_bcopy_single
    -> uct_ep_am_bcopy
```

其中 `ucp_do_am_bcopy_single()` 很直白：  
`ucx\src\ucp\proto\proto_am.inl:75-98`

它最终就是调用：

```c
uct_ep_am_bcopy(..., am_id, pack_cb, req, 0)
```

### 5.2 obmm send 侧：`uct_obmm_ep_am_bcopy()` 并不把 payload 写进 elem，而是写进 `desc[N]`

关键代码：`ucx\src\uct\obmm\base\obmm_ep.c:273-331`

它和 short 的差别主要只有两处：

1. slot 保留还是同一套逻辑：`uct_obmm_ep_reserve_slot()`  
   见 `obmm_ep.c:245-270`
2. 真正的 payload 不再写到 `elem+1`，而是写到：

```c
desc = uct_obmm_slot_desc(ep->peer_descs, head, ep->fifo_mask,
                          ep->bcopy_seg_size);
length = pack_cb(desc, arg);
```

见 `obmm_ep.c:298-313`

elem 本身只保存元数据：

- `am_id`
- `length`
- `generation`
- `flags = OWNER | BCOPY`

见 `obmm_ep.c:314-327`

### 5.3 paired-desc 的真正意义

V2 不是额外引入了一套“独立 desc allocator”。  
它做的是 **“FIFO 元素 N 和 desc N 一一配对”**：

- slot 中同时有 `fifo_elem[fifo_size]`
- 也有 `bcopy_desc[fifo_size]`
- sender 抢到第 N 个 FIFO 元素时，也就同时抢到了第 N 个 desc
- receiver 把 `tail` 推过 N 时，elem[N] 和 desc[N] 一起释放

代码入口：

- slot 布局：`obmm_fifo.h:63-122`
- 设计说明：`ucx\src\uct\obmm\DESIGN.md:137-198, 290-301`

**这就是 V2 最核心的抽象：**  
`am_bcopy` 不再受 `FIFO_ELEM_SIZE` 限制，但也没有引入 mm 那种独立 desc free-list 的跨主机复杂性。

### 5.4 obmm recv 侧：看到 BCOPY 标记，就去 paired desc 取 payload

关键代码：`ucx\src\uct\obmm\base\obmm_iface.c:248-263`

流程是：

1. 先和 short 一样检查 owner bit、做 bus load fence、检查 generation
2. 如果 `flags & UCT_OBMM_FIFO_ELEM_FLAG_BCOPY`
3. 通过同样的 `read_index & mask` 算出 `desc[N]`
4. 调用：

```c
uct_iface_invoke_am(&iface->super, elem->am_id, desc, elem->length, 0)
```

注意最后那个 `0` 很重要：它表示 **这个 buffer 只在 callback 生命周期内有效**。  
也就是说，UCP handler 必须在 callback 期间把需要的数据取走，不能把 desc 指针长期保留。

这个语义在设计文档里写得很清楚：  
`ucx\src\uct\obmm\DESIGN.md:290-307`

### 5.5 为什么 tail 发布前必须是 `uct_obmm_bus_full_fence()`

关键代码：`obmm_iface.c:275-283` 和 `obmm_fifo.h:125-156`

short 路径里你可能不太容易体会这一点；但 bcopy 路径非常直观：

- receiver 刚刚从 `desc[N]` 读 payload
- 如果现在只做 store-store fence 就把 `tail = N+1` 发布出去
- sender 看到 tail 前进，可能立刻复用 `desc[N]`
- 这时 receiver 这边对 `desc[N]` 的 load 还没完全被顺序化，就会出现生命周期破坏

所以 V2 这里不是普通的 `ucs_memory_bus_store_fence()`，而是本地定义的  
`uct_obmm_bus_full_fence()`。

**理解点：**  
V2 paired-desc 之所以安全，不只靠“desc 和 elem 1:1 配对”，还靠 **receiver 在 release tail 前把对 desc 的读取完整排序完**。

---

## 6. 例子三：`4097B / 64KiB / 1MiB` 不是 obmm 新增很多发送接口，而是 UCP 在重复调用同一个 `am_bcopy`

这一点很容易误解。

### 6.1 大于 `max_bcopy` 时，分片发生在 UCP，不发生在 obmm UCT

关键代码：`ucx\src\ucp\tag\eager_snd.c:138-146` 和  
`ucx\src\ucp\proto\proto_am.inl:120-198`

`ucp_do_am_bcopy_multi()` 的逻辑是：

1. 第一片用 `am_id_first`
2. 后续片用 `am_id_middle`
3. 每一片都调用一次 `uct_ep_am_bcopy`
4. 直到最后一片

换句话说，**obmm transport 根本不知道“这是一个 64KiB 的 MPI 消息”**。  
它只知道自己被 UCP 连续调用了很多次 `uct_obmm_ep_am_bcopy()`，每次往 peer slot 里塞一个 fragment。

### 6.2 所以 `mpi_bw_v2.c` / `mpi_pingpong_v2.c` 里 4097、16384、65536、262144、1048576 这些 size，真正锻炼的是两件事

1. **UCP 选协议和分片是否正确**  
   代码看 `eager_snd.c` 和 `proto_am.inl`
2. **obmm FIFO / paired-desc / pending 在高并发下是否稳定**  
   代码看：
   - `obmm_ep.c:245-375`
   - `obmm_iface.c:221-295`

特别是：

- `mpi_bw_v2.c` 用 `WINDOW=32`，见 `mpi_bw_v2.c:17, 74-118`
- `mpi_correctness_v2.c` 用 `WINDOW=16`，见 `mpi_correctness_v2.c:23-25, 85-137`

这两个测试不是单纯测带宽/正确性，它们也在逼你验证：

- slot 回收是否及时
- `cached_tail` 刷新是否正确
- `pending_add` / arbiter 是否能在背压下继续推进

### 6.3 当前代码里，背压恢复不只靠 UCP retry，还靠 obmm 自己的 arbiter

关键代码：

- `uct_obmm_ep_pending_add()`：`obmm_ep.c:352-375`
- `uct_obmm_ep_process_pending()`：`obmm_ep.c:378-407`
- `iface_progress()` 末尾 dispatch arbiter：`obmm_iface.c:286-293`

这意味着当 peer FIFO 满时：

1. send path 返回 `NO_RESOURCE`
2. UCP 会把请求挂到 pending
3. receiver progress 推进了 `tail` 以后
4. `iface_progress()` 末尾顺手 dispatch pending

**这也是为什么你在理解高并发测试时，不能只看 `am_short` / `am_bcopy` 两个函数。**  
真正让 bidirectional/bandwidth 场景不死锁的，往往是 `pending` 和 `iface_progress()` 最后那几行。

---

## 7. 例子四：为什么 262144B 的 OSU 曾经会炸到 `rkey_ptr`，而修复点却在 `obmm_md.c`

这是理解“UCT capability 会反过来控制 UCP 协议选择”的另一个典型例子。

### 7.1 如果 transport 说自己支持 `rkey_ptr`，UCP rendezvous 可能直接把远端地址当本地指针 memcpy

关键代码：`ucx\src\ucp\rndv\rndv_rkey_ptr.c:145-230`

路径是：

1. `uct_rkey_ptr(...)`
2. 得到一个“可直接解引用的远端地址”
3. `ucp_proto_rndv_progress_rkey_ptr()` 里直接做 unpack / memcpy

### 7.2 但 obmm 实际只 mmap 了预导出的 128MiB region，没有 mmap peer 用户堆

所以如果 obmm 错误宣告了 `rkey_ptr`，UCP 就会对一个 **根本没有映射到本地地址空间的 peer user buffer** 做 memcpy，直接崩。

当前代码明确把这件事关掉了：

- `uct_obmm_md_query()` 不再宣告 `NEED_RKEY / REG`  
  `ucx\src\uct\obmm\base\obmm_md.c:27-45`
- component `.rkey_ptr` 也明确设成 unsupported  
  `obmm_md.c:220-243`

### 7.3 对你理解 V2 的意义

这个例子说明：

> **有些“看起来和 FIFO/desc 完全无关”的代码，其实直接决定 UCP 会不会走到你根本没实现的路径上。**

所以读 obmm transport 时，不能只盯着 `obmm_ep.c`。  
`obmm_md.c` 的能力边界同样是数据路径的一部分。

---

## 8. 把几个关键文件按“在通信里扮演什么角色”重新看一遍

| 文件 | 真正作用 |
| --- | --- |
| `obmm_sysfs.h/.c` | 把系统里有哪些 shmdev、哪些是 export/import、它们属于谁，翻译成 UCX 能理解的元数据 |
| `obmm_region.h/.c` | 真的去 `open + mmap` 这些 shmdev，拿到本进程可访问的虚拟地址 |
| `obmm_md.h/.c` | 持有所有 region 映射；决定 obmm 对外宣告哪些 MD 能力，不宣告哪些 |
| `obmm_pool.h/.c` | 在 export region 头部维护 pool header / bitmap / slot_meta / slot[]；负责 pool 初始化、dead-initializer 恢复、slot 分配/接管/释放，以及 generation 生命周期 |
| `obmm_fifo.h` | 定义 slot 内部布局、FIFO elem 头、paired-desc 布局，以及最关键的 fence / pointer arithmetic |
| `obmm_iface.h/.c` | 站在“我这边的接收端”视角工作：发布地址、做 reachable 判断、轮询我的 slot、推进 tail、唤醒 pending |
| `obmm_ep.h/.c` | 站在“我要往 peer 发消息”视角工作：定位 peer slot、保留 slot、写 elem/desc、发布 flags |
| `DESIGN.md` | 最接近“官方协议说明”的文档，但理解当前行为时仍应以代码为准 |
| `mpi_pingpong_v2.c` | 观察 short / bcopy / protocol boundary 最直观的 2-rank 示例 |
| `mpi_correctness_v2.c` | 最适合看边界值和并发窗口压力 |
| `mpi_bw_v2.c` | 最适合观察多 in-flight 请求时 UCP+obmm 的配合 |

---

## 9. 阅读时请优先相信“当前代码”，不是所有注释/设计文字都完全同步

有两处最容易把人带偏：

1. **short 路径的 `elem->length`**  
   `obmm_fifo.h` 里的旧注释容易让人以为 short 只记录 payload 长度；但当前代码实际记录的是  
   **`8B tag header + payload` 总长度**，请以 `obmm_ep.c:180-223` 和 `obmm_iface.c:264-268` 为准。

2. **`DESIGN.md` 更像“V2 设计主线 + 一部分历史快照”**  
   它非常有价值，但不是每一句都和当前代码逐字符同步。比如你关心 pool layout / version / fence 细节时，最好同时对照：
   - `obmm_pool.h:17-25`
   - `obmm_fifo.h:125-156`
   - `obmm_iface.c:275-283`

所以我的建议是：**先把 `DESIGN.md` 当设计地图，再用 `obmm_*.c/.h` 校准当前行为。**

---

## 10. 阅读时建议抓住的 6 个“不要再抽象化”的点

1. **“peer 地址”不是一个地址，而是三段信息：**
   - device identity
   - slot identity
   - geometry

2. **short 和 bcopy 的区别不在 UCP handler，主要在 obmm 如何存放 payload：**
   - short：payload 在 `elem+1`
   - bcopy：payload 在 `desc[N]`

3. **大于 4096B 以后，不是 obmm 变复杂了，而是 UCP 开始多次调用 obmm 的同一个 bcopy primitive。**

4. **很多诡异问题其实来自 capability bits：**
   - 没有 `INTER_NODE` -> cross-node 地址里压根没有 obmm
   - 错宣 `rkey_ptr` -> 大消息直接走到错误的 rndv 协议

5. **先有 `obmm_pool`，后有 FIFO：**
   - pool 决定 slot 归谁、哪一代有效
   - FIFO / desc 只是这一代 slot 里的具体数据布局

6. **V2 paired-desc 能成立，靠的是“slot 生命周期 = desc 生命周期” + “tail 发布前的 full bus fence”。**

---

## 11. 如果你想按“端到端例子”继续深挖，推荐这个顺序

### 路径 A：先吃透 small eager short

1. `mpi_pingpong_v2.c:62-91`
2. `ompi\ompi\mca\pml\ucx\pml_ucx.c:1022-1060`
3. `ucx\src\ucp\tag\tag_send.c:150-177, 231-307`
4. `ucx\src\uct\obmm\base\obmm_ep.c:174-241`
5. `ucx\src\uct\obmm\base\obmm_iface.c:221-295`
6. `ucx\src\ucp\tag\eager_rcv.c:78-147`
7. `ucx\src\ucp\tag\tag_recv.c:224-252`

### 路径 B：再吃透 single-frag bcopy

1. `mpi_correctness_v2.c:28-38, 85-137`
2. `ucx\src\ucp\tag\eager_snd.c:130-146`
3. `ucx\src\ucp\proto\proto_am.inl:75-98`
4. `ucx\src\uct\obmm\base\obmm_ep.c:273-331`
5. `ucx\src\uct\obmm\base\obmm_iface.c:248-283`
6. `ucx\src\uct\obmm\DESIGN.md:137-198, 268-307`

### 路径 C：最后看 multi-frag / rndv

1. `mpi_bw_v2.c:20-34, 95-120`
2. `ucx\src\ucp\tag\eager_snd.c:138-146`
3. `ucx\src\ucp\proto\proto_am.inl:120-198`
4. `ucx\src\ucp\rndv\rndv.c:1828-1852`
5. `ucx\src\ucp\rndv\rndv_rkey_ptr.c:145-230`
6. `ucx\src\uct\obmm\base\obmm_md.c:27-45, 220-243`

---

## 12. 最后给你一个简化版“通信全过程”口令

你之后读代码时，可以一直拿下面这段话对照：

> **本地 iface 先通过 `obmm_pool` 在 export region 里拿到一个 slot 当接收邮箱；地址交换把“哪块 region + 哪个 slot + 什么几何”发给 peer；ep_create 再去 open peer pool，把这些线索翻译成 `peer_ctl/peer_elems/peer_descs` 指针；UCP 小消息调用 `uct_ep_am_short`，中等消息调用 `uct_ep_am_bcopy`，更大消息则由 UCP 自己分片或切到 rendezvous；obmm send 侧只负责往 peer slot 发布一个 fragment，recv 侧只负责轮询自己的 slot、校验 generation、调用 `uct_iface_invoke_am()`，然后推进 tail。**

如果你能把这句话和上面的 3 条例子逐行对上，V2 的实现就已经基本吃透了。
