# obmm_import: 引入远端内存

## 名称 NAME

`obmm_import` - 引入远端内存

## 库 LIBRARY

OBMM用户态库 (libobmm)

## 摘要 SYNOPSIS

```c
#include <libobmm.h>
mem_id obmm_import(const struct obmm_mem_desc *desc, unsigned long flags, int base_dist, int *numa);
```

## 描述 DESCRIPTION

内存使用方引入一段远端内存，生成字符设备，以设备名以内存ID结尾（/dev/obmm_shmdev\${mem_id}）。如需通过mmap方式使用内存，请参考 obmm_shmdev(4)。

结束使用时，obmm_import 创建的内存，需要用 obmm_unimport(3) 释放。

内存必须满足下列要求，才能被成功引入：

**地址、长度对齐**

* 传入的地址和内存长度必须按OBMM基础粒度对齐。
* NUMA_REMOTE 模式，内存长度和物理地址（如适用）必须按128MB（内核页为4K或16K时）或512MB（内核页为64K时）对齐。

**地址不冲突**

* preimport 模式引入的内存，物理地址必须落在节点预留内存中，且该内存当前未实际上线。
* 非 preimport 模式引入的内存，物理地址互不重叠，且不能和 preimport 已预留的地址段重叠。

#### Input Parameters

**desc**：指向一个OBMM内存描述符，包含待引入内存的地址、长度、链路等信息。

| 字段      | 描述 |
| --------- | --------------------------------------------- |
| addr      | 远端内存对应的物理地址（由配置ubmem decoder的组件提供）     |
| length    | import的内存大小                              |
| tokenid   | 忽略                                          |
| deid      | 提供方提供内存的UB controller的EID，当前仅用于记账 |
| seid      | 使用方引入内存的UB controller的EID             |
| scna      | 使用方引入内存的UB controller的CNA地址          |
| dcna      | 提供方提供内存的UB controller的EID，当前仅用于记账  |
| priv_len  | 私有数据长度                                  |
| priv      | 私有数据，仅呈现在sysfs中，不影响通路         |

**flags**：引入内存的属性。

内存引入后的软件接口由以下两个 flags 决定：

*OBMM_IMPORT_FLAG_NUMA_REMOTE* ：指示import的内存需要上线到numa。
*OBMM_IMPORT_FLAG_ALLOW_MMAP*：指示import后的memid对应的字符设备，支持mmap的使用方式。

如上两个FLAG，需要指定且只能指定一个，否则引入会失败。

内存引入时，是否使用预引入加速，由以下 flag 指定：

*OBMM_IMPORT_FLAG_PREIMPORT*：指示本次import内存上线，以预上线的模式进行，以在内存上线NUMA时获得软件加速。

使用\<addr, length\>匹配预上线节点，addr 指定的物理地址必须落在预留的物理地址范围内。

指定*OBMM_IMPORT_FLAG_PREIMPORT*时，必须同时指定*OBMM_IMPORT_FLAG_NUMA_REMOTE*，否则失败。

**base_dist**：引入内存上线作为远端 NUMA 节点上线的基础距离。即该远端NUMA节点（n_r）到引入物理芯片所在本地 NUMA （基准NUMA，n_b）的距离。

* `base_dist`等于0时，新节点到其他本地节点的距离定义为100，新节点到其他远端节点的距离为255。
* `base_dist`不等于0且小于等于10，或`base_dist`大于255时，接口报非法参数错误。
* `base_dist`大于11且小于等于255时，新节点到其他远端节点的距离为255，新节点到其他本地的本地节点的距离为： dist(n, n_b) + dist(n_b, n_r) - dist(n_b, n_b)。如果该值大于 255，则定义距离为255。

OBMM引入内存并上线remote NUMA时，如果为直接上线，NUMA distance会根据最后一次上线重新计算并覆写前值。建议应用始终传入相同的base distance以保持NUMA distance稳定。

remote NUMA并不限制从特定CPU package或特定UB die引入，当一个remote包含从多个UB die引入的内存时，它到各个本地NUMA节点间的亲和性难以定义，应用可以传入base distance=0，使remote NUMA和各本地NUMA节点保持等距。

未指定*OBMM_IMPORT_FLAG_NUMA_REMOTE*时，或指定*OBMM_IMPORT_FLAG_PREIMPORT*时，base_dist参数将被忽略。

**numa**：指向一个int值，用于传递引入内存上线为NUMA节点后期望的节点ID。在下列场景中，该参数会被忽略：

1. 没有指定*OBMM_IMPORT_FLAG_NUMA_REMOTE* flag。
2. 同时指定了*OBMM_IMPORT_FLAG_NUMA_REMOTE* flag 和 *OBMM_IMPORT_FLAG_PREIMPORT* flag。

指定*OBMM_IMPORT_FLAG_NUMA_REMOTE*后，按照如下规则决定上线的NUMA节点：

* 若同时指定了 *OBMM_IMPORT_FLAG_PREIMPORT*，上线到预上线地址段对应的NUMA节点。
* 未预上线时，若指针为NULL，或指向的值为NUMA_NO_NODE(-1)时：import上线到新的NUMA节点，指针不是NULL时，通过指针返回远端NUMA节点的ID。
* 其他情形下，指针指向的值会作为期望上线的NUMA节点ID。节点ID不能配置为本地NUMA节点的ID。

如果用户使用该参数指定了已存在的远端remote NUMA进行增量 obmm_import 上线，NUMA distance 可能发生重新配置：

* 如果为预上线模式的import，不会变更先前设置的NUMA distance。
* 如果非预上线模式的import，且base_dist为0，不会变更先前设置的NUMA distance。
* 如果非预上线模式的import，且base_dist为非0值，会根据新参数重配置NUMA distance。

## 返回值 RETURN VALUE

引入成功时，返回内存编号（memid）。同时会在 /dev/ 目录下生成对应的 /dev/obmm_shmdev\${memid} 字符设备。详见 obmm_shmdev(4)。
如果*numa*参数不是空指针，引入内存实际上线到的NUMA ID会被写入其*numa*指向的内存。如果不涉及NUMA上线，会写入-1。

失败时，返回 `OBMM_INVALID_MEMID`(0)，详细的错误类型存储在`errno`中。

## 错误 ERRORS

故障码对应的部分情形如下：

* `EPERM`：NUMA REMOTE相关错误，较常见的是参数错误、资源占用，以及本地内存不足等，例如：
  * 传入的 *numa 指定了非法的NUMA ID；
  * 模块在加载过程中被卸载；
  * 创建 Remote NUMA节点失败。
* `ENODEV`：传入的scna不对应任何UB controller。
* `EINVAL`：
  * desc 或 flags 为 `OBMM_INVALID_MEMID(0)`；
  * priv_len 的长度超出 `OBMM_MAX_PRIV_LEN`；
  * 申请内存大小不能为零；
  * flags 含有无效标志位；参数 `ALLOW_MMAP`和 `NUMA_REMOTE` 必须且只能指定一个;在指定 `OBMM_IMPORT_FLAG_PREIMPORT` 时，必须指定 `OBMM_IMPORT_FLAG_NUMA_REMOTE`；
  * `base_dist` 不等于0且小于等于10，或大于255；
  * 传入的地址和内存长度没有按照OBMM基础粒度对齐；pa 为 0 或 pa + size 溢出；
  * preimport 模式引入的内存，物理地址没有落在节点预留内存中；
  * 导入的 scna、seid、dcna、deid 和预引入时使用 scna、seid、dcna、deid 不匹配。
* `ENOMEM`: 系统内存不足。
* `EBUSY`: 物理地址对应内存区间已被占用或冲突。
* `ENOSPC`：指定范围内无可用 `memid`。
* `EEXIST`: region 已存在。
## 约束 CONSTRAINTS

**地址校验约束**

需要用户保证传入的物理地址范围正确落在UB远端内存的地址范围内。

## 附注 NOTES

暂无

## 样例 EXAMPLES

以下函数引入了一段物理基地址为 `pa` ，长度为 `size` 的内存。该内存在本机可通过 obmm_shmdev(4) 设备映射访问。随后函数使用 obmm_unimport(3) 停止了这段内存的引入。

```c
#include <stdio.h>
#include <stdlib.h>
#include <libobmm.h>

int import_demo(unsigned long pa, size_t size, unsigned int scna, uint8_t *seid)
{
	int ret;
	mem_id id;
	/* Import the remote memory in such a way that we can open and mmap its char device. */
	unsigned long flags = OBMM_IMPORT_FLAG_ALLOW_MMAP;
	/* Specify that this memory device has no private data. */
	struct obmm_mem_desc desc = {
		.addr = pa,
		.length = size,
		.scna = scna,
	};
	memcpy(desc.seid, seid, 16);
	id = obmm_import(&desc, flags, 0, NULL);
	if (id == OBMM_INVALID_MEMID) {
		perror("obmm_import() failed.\n");
		exit(EXIT_FAILURE);
	}
	/* Import succeeded. */

	/* Do your work here... */

	/* Unimport memory. */
	ret = obmm_unimport(id, 0);
	if (ret) {
		perror("obmm_unimport() failed.\n");
		exit(EXIT_FAILURE);
	}

	return 0;
}
```

预引入相关的样例，请见 obmm_preimport(3) 。
