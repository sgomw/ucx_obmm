# libobmm: OBMM 用户态库

OBMM 是在单机内管理远端内存的基础组件，可以将本地内存导出（export），将其他系统导出的内存引入（import）。export、import 两方的 OBMM 组件依次完成数据通路配置后，import 侧的应用可以像使用本端内存一样，使用 `load` 、`store` 访问远端内存。

OBMM 组件包含用户态库 libobmm.so 和内核模块 obmm.ko（详见obmm(4)）。本文档包含 libobmm 的功能总览，并介绍其中的关键数据结构。libobmm 的每个函数有专有的文档展开描述。

## OBMM API 总览

### 导出内存

| API                  | 功能                                         |
| -------------------- | -------------------------------------------- |
| obmm_export          | 从 OBMM 内存池中导出内存，供远端引入         |
| obmm_export_useraddr | 指定一段进程映射的内存，将其导出，供远端引入 |
| obmm_unexport        | 取消 OBMM 内存的导出                         |

### 引入内存

| API           | 功能               |
| ------------- | ------------------ |
| obmm_import   | 从远端引入内存     |
| obmm_unimport | 取消远端内存的引入 |

### 预上线内存

| API              | 功能                                     |
| ---------------- | ---------------------------------------- |
| obmm_preimport   | 预上线一段远端内存，以加速后面的实际引入 |
| obmm_unpreimport | 取消一段的内存的预上线                   |

### 读写状态维护

| API                | 功能                                   |
| ------------------ | -------------------------------------- |
| obmm_set_ownership | 设置内存设备的读写状态，用于一致性维护 |

### 内存地址查询

| API                    | 功能                                             |
| ---------------------- | ------------------------------------------------ |
| obmm_query_memid_by_pa | 根据物理地址查询内存设备ID，用于维测             |
| obmm_query_pa_by_memid | 根据内存设备ID和地址偏移量查询物理地址，用于维测 |

## OBMM 内存 ID

OBMM 使用64位整数编码内存每段 OBMM 内存，其中 `OBMM_INVALID_MEMID` (0) 为预留 ID，用来标识错误。导出内存和引入内存在同一个 ID 空间。

每一段 OBMM 都有一个以 ID 结尾的字符设备（obmm_shmdev(4)）和 sysfs目录（obmm_shmdev_sysfs(5)）。

## OBMM 内存描述符

为了描述一段可以跨 host 访问的远端内存，libobmm 定义了通用数据结构 `struct obmm_mem_desc`：

```c
struct obmm_mem_desc {
	uint64_t addr;
	uint64_t length;
	/* 128bit eid, ordered by little-endian */
	uint8_t seid[16];
	uint8_t deid[16];
	uint32_t tokenid;
	uint32_t scna;
	uint32_t dcna;
	uint16_t priv_len;
	uint8_t  priv[];
}
```

该数据结构是组网范围内的一段支持跨host访问的内存的通用描述。

在提供方，该数据结构主要作为出参，用于获取导出内存的地址参数，少部分域段也用作配置入参。

在使用方，该数据结构为入参，用于引入远端内存。

**addr**

* 内存提供方：出参，export流程会输出{tokenid，UBA}，其中addr存储UBA，作为UB memory链路报文的核心元素。
* 内存使用方：入参，表示物理地址；

**length**

* 内存提供方：出参，会返回实际export的内存总大小。
* 内存使用方：入参，指示import内存的大小信息。

**tokenid**

* 内存提供方：出参，export流程会输出{tokenid，UBA}，作为UB memory链路报文的核心元素。
* 内存使用方：入参，忽略该值。

**seid**

* 内存提供方：忽略。
* 内存使用方：入参，指示本节点访问目标内存时，使用的IODie。

**deid**

* 内存提供方：入参，指示内存借出时，使用的IODie。
* 内存使用方：入参，仅记录。

**scna**

* 内存提供方：忽略。
* 内存使用方：入参，指示本节点访问目标内存时，使用的IODie。

**dcna**

* 内存提供方：忽略。
* 内存使用方：入参，仅记录。

**priv_len 和 priv**

内存专属的黑盒私有数据，用户可以在创建内存时传入，然后从 OBMM sysfs 中读出，读出方法详见 obmm_shmdev_sysfs(5) 。

在提供方，私有数据会被透传给UMMU driver。

priv_len: 用户私有数据的长度。

priv：指向用户的私有数据，即紧随`struct obmm_mem_desc`的、长度为`priv_len`字节的一段连续内存。

这两个域段总是OBMM的入参。用户可以通过如 `malloc(sizeof(struct obmm_mem_desc) + priv_len)` 创建能承载priv_len的内存描述符，然后向`desc->priv[i]`写入私有数据的第`i`字节的值。如果不使用私有数据，需要将 `priv_len` 设置为0，以避免非预期的校验失败和越界访问。`priv_len` 的上限是 `OBMM_MAX_PRIV_LEN`（当前为512，考虑到后续接口变动的可能，建议应用自用部分不超过 128 字节，以保证兼容性）。

## OBMM 预上线内存描述符

为在使用方描述一段预上线内存，libobmm 定义了通用数据结构 `struct obmm_preimport_info`：

```c
struct obmm_preimport_info {
	uint64_t pa;
	uint64_t length;
	int base_dist;
	int numa_id;
	uint8_t seid[16];
	uint8_t deid[16];
	uint32_t scna;
	uint32_t dcna;
	uint16_t priv_len;
	uint8_t priv[];
};
```

该数据结构仅在使用方使用。与 `struct obmm_mem_desc` 相比，`struct obmm_preimport_info` 是对远端内存的一个部分描述。通常只限定了待上线内存需使用的数据链路，但没有精确框定地址范围。是一个部分描述。

基于该部分描述，OBMM 可以提前创建一个 NUMA 节点。实际上线时仅需将实际借入的内存“注入”该 NUMA 节点，从而加速关键路径上的软件流程。

本节仅介绍字段含义。具体的配置方法和参数功能与场景强相关，详见 obmm_preimport(3), obmm_unpreimport(4)。

**pa**

入参，预上线内存的起始物理地址。

**length**

入参，预上线内存的总长度。

**scna**

入参，指示本节点访问目标内存时，使用的IODie。

**dcna**

入参，指示本节点访问目标内存时，经过的提供方IODie。

**seid**

入参，指示本节点访问目标内存时，使用的IODie。

**deid**

入参，指示内存借出时，使用的IODie。

**base_dist**

入参，表示新上线NUMA到使用方IODie的基础距离，OBMM会根据该距离和预定规则，计算预上线NUMA节点到全部NUMA节点的距离。

**numa_id**

入参和出参：用于指示预上线 NUMA 节点使用的 NUMA ID，-1 表示由系统分配，否则使用传入值作为预上线NUMA ID。由系统分配时，这一字段也作为出参，返回系统分配的 NUMA ID。

**priv_len 和 priv**

入参：用于共同描述priv数据。

## 使用模型：借用与共享

用户态应用有两种途径访问 OBMM 内存，我们称为借用与共享：

| 模型 | 特点                                                         | 映射方法                               |
| ---- | ------------------------------------------------------------ | -------------------------------------- |
| 借用 | 每段内存只支持一个使用方独占访问<br>必须为 cacheable 属性<br>使用 remote NUMA node 管理远端内存 | madvise, mbind, move_pages, numactl 等 |
| 共享 | 每段内存理论上可以让多方交替使用<br>通过 mmap 字符设备映射内存 | mmap                                   |

应用通过导出和引入的 flags 来控制自己的使用模型。

当应用使用借用模型或使用 noncacheable 属性时，用户无需关心一致性模型，所有的用户均具备读写权限，且无法使用 obmm_set_ownership(3) 调整。

当应用通过共享模型使用 cacheable 内存时。需要考虑一致性模型。

在主流配置下，一致性模型的OBMM基础粒度为 2M，详见基础粒度章节。

对每段OBMM基础粒度的内存，用户可能有空（`PROT_NONE`）、读（`PROT_READ`）、读写（`PROT_WRITE`）三种权限之一。

* 内存导出 / 引入后，权限为空
* 应用映射时，可通过 mmap(2) 的`prot` 参数改配权限
* 应用映射后，可通过 obmm_set_ownership(3) 切换当前权限
* 应用通过 munmap(2)，权限不会发生变更
* obmm_unimport(3) 时，会先自动切换为空权限，然后退出

在同一时刻，所有访问该内存的各 host（包括提供方和使用方）只能处于如下两种状态之一，否则有数据不一致的风险。

1. 所有内存访问进程均为读权限或空权限（没有访问者为写权限）
2. 只有一个 host 上存在具备写权限的进程，其他 host 上的所有映射进程均为空权限


## 操作粒度

OBMM 各项操作的粒度受多方限制：

| 位置   | 组件                            | 适用场景             | 粒度                    | 典型值                | 其他值                             |
| ------ | ------------------------------- | -------------------- | ----------------------- | --------------------- | ---------------------------------- |
| 提供方 | 内核分配器<br/>内核线性映射页表 | 全部                 | PMD_SIZE                | 2M (4K page)          | 32M (16K page)<br/>512M (64K page) |
| 提供方 | UMMU 片上翻译表                 | 全部                | 2MB                     | 2M                    | 4M, 8M, ..., 256M                  |
| 提供方 | 内存分配器粒度                 | 全部                  | 与用户指定的内存分配器器相关 | 2M (4K page)       | 1G (4K page)                        |
| 使用方 | 内存热插                        | 借用                 | memory_block_size_bytes | 128M (4K or 16K page) | 512M (64K page)                    |
| 双方   | 进程页表                        | 共享                 | PAGE_SIZE               | 4K                    | 16K, 64K                           |
| 双方   | 缓存 home agent                 | 全部                 | 128B                    | 128B                  | 128B                               |

定义**OBMM基础粒度**为提供方、使用方能一致地传递数据的最小粒度。该粒度为以下三者的最小公倍数：

* 提供方内核线性映射页大小
* 进程页大小
* 缓存更新粒度

因为后两者总小于提供方内核线性映射页的大小，因此这一粒度的值为 PMD_SIZE。

OBMM 所有接口（obmm_export，obmm_import，obmm_preimport）皆受OBMM基础粒度的限制。

导出接口（obmm_export）和引入（obmm_import, obmm_preimport）受上表中适用粒度的最大值限制。

除非函数API中特别说明，上述粒度既适用于长度，也适用于相关的地址或偏移量对齐。

此外，通过 OBMM 共用内存的提供方、使用方两端，内核的核心编译选项需保持一致，否则 OBMM 的粒度约束可能产生误拦截或漏拦截，影响使用。

## 控制面流程时序

用户需要注意按以下流程时序进行配置。违反该时序，可能导致进程崩溃、数据不一致、芯片异常等多种非预期现象。

1. 提供方export
2. 使用方import
3. 数据访问
4. 使用方unimport
5. 提供方unexport
