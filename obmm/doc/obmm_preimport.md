# obmm_preimport: 预引入远端内存

## 名称 NAME

`obmm_preimport` - 预引入远端内存

## 库 LIBRARY

OBMM用户态库 (libobmm)

## 摘要 SYNOPSIS

```c
#include <libobmm.h>
int obmm_preimport(struct obmm_preimport_info *preimport_info, unsigned long flags);
```

## 描述 DESCRIPTION

以NUMA方式引入远端内存时，创建 NUMA 节点会产生较长耗时。

预引入可以提前完成NUMA节点创建的工作，它不依赖内存导出时生成的完整信息，因此可以在内存导出之前完成。内存实际上线时，将该段内存”注入“NUMA即可，可以加速关键路径上的软件流程。

物理地址段\<pa, length\>为匹配索引。

实际引入内存时，obmm_import 需要配置 *OBMM_IMPORT_FLAG_PREIMPORT* flag ，同时其参数要命中索引，才能实现引入加速：

obmm_import 的物理地址段必须为预引入地址段的子集。

预引入参数需要满足以下要求：

**索引唯一**

每次预上线的物理地址段不能重叠，也不能与非预上线模式引入的内存物理地址重叠。

**NUMA有效**

传入的NUMA ID不为 -1时，NUMA ID必须合法（为非负数）且不能指向本地NUMA。

预引入内存需使用 obmm_unpreimport(3) 释放。

#### Input Parameters

**preimport_info**:

预上线地址段信息。包含组网和通路信息，是组网范围内、通过某一通路引入的内存的部分描述。

| 字段      | 描述                                             |
| --------- | ------------------------------------------------------------ |
| pa        | 预上线内存的物理地址基地址                                   |
| length    | 预上线 NUMA 节点所能容纳的最大内存                           |
| scna      | 指示本节点访问目标内存时，使用的IODie<br/>用于NUMA distance计算和记账 |
| dcna      | 指示访问目标内存时，经过的提供方IODie<br/>仅用于记账，不参与通路配置 |
| seid      | 指示本节点访问目标内存时，使用的IODie<br/>仅记录，不参与通路配置 |
| deid      | 指示访问目标内存时，经过的提供方IODie<br/>仅记录，不参与通路配置 |
| base_dist | 表示新上线NUMA到使用方IODie的基础距离                        |
| numa_id   | 指示预上线 NUMA 节点使用的 NUMA ID<br/>配置为-1时，用于接收自动分配的 NUMA ID |
| priv_len  | 忽略                                                         |
| priv      | 忽略                                                         |

**flags**：选项（预留，当前未使用，必须配置为0）。

## 返回值 RETURN VALUE

成功时，返回0。

失败时，返回-1，详细的错误类型存储在`errno`中。

## 错误 ERRORS

故障码对应的部分情形如下：

* `EPERM`：NUMA REMOTE相关错误，较常见的是参数错误、资源占用等，例如：
  * 传入的物理地址或长度不满足NUMA REMOTE的对齐要求；
  * 传入的*numa指定了非法的NUMA ID；
  * 物理地址资源已被占用。
* `EINVAL`: 传入参数不符合要求，例如：
  * preimport 传入参数错误，如：preimport_info 为空，base_dist 不在合法范围内；
  * flags 中含有非法标志位；length 不为零；无效的CNA对。
* `ENODEV`: scna 无效、seid无效或 scna - seid 不匹配（不属于同一 UB entity）。
* `ENOMEM`: 系统内存不足导致的错误。
* `EEXIST`: 内存范围已被占用或冲突。

## 约束 CONSTRAINTS

**地址校验约束**

需要用户保证传入的物理地址范围正确落在UB远端内存的地址范围内。

## 附注 NOTES

暂无

## 样例 EXAMPLES

以下函数预引入了一段物理基地址为 `pa` ，长度为 `size` 的内存。函数创建了一个新的 remote NUMA 节点，但节点起初并没有可用内存。
调用 obmm_import(3) 时，内存被注入了 NUMA 节点。最后，程序调用 obmm_unimport(3) 和 obmm_unpreimport(3) 解除了这段内存的上线和预上线。
现实场景中，预上线一般发生在内存导出之前，很少像本示例一样，在同一函数中顺次调用预上线和上线接口。

```c
#include <stdio.h>
#include <stdlib.h>
#include <libobmm.h>

int preimport_demo_decoder(unsigned long pa, size_t size, unsigned int scna, uint8_t *seid)
{
	int ret;
	mem_id id;
	unsigned long import_flags;
	/* Preimport [pa, pa+size) range and use a new NUMA node to hold the preimport info. */
	struct obmm_preimport_info info = {
		.pa = pa,
		.length = size,
		.scna = scna,
		.base_dist = 0,
		.numa_id = -1,
		.priv_len = 0

	};
	memcpy(info.seid, seid, 16);
	struct obmm_mem_desc desc = {};

	ret = obmm_preimport(&info, 0);
	if (ret) {
		perror("obmm_preimport() failed.\n");
		exit(EXIT_FAILURE);
	}
	/* Preimport succeeded. The associated NUMA node id is stored in info.numa_id. */

	/* Import all memory prepared in the preimport phase. OBMM would match the import request
	 * with preimport range using PA range. */
	desc.addr = pa;
	desc.length = size;
	desc.scna = scna;
	memcpy(desc.seid, seid, 16);
	desc.priv_len = 0;
	import_flags = OBMM_IMPORT_FLAG_NUMA_REMOTE | OBMM_IMPORT_FLAG_PREIMPORT;
	id = obmm_import(&desc, import_flags, 0, NULL);
	if (id == OBMM_INVALID_MEMID) {
		perror("obmm_import() failed.\n");
		exit(EXIT_FAILURE);
	}
	/* Import succeeded. */

	/* Do your work here... */

	/* Unimport memory. */
	ret = obmm_unimport(id, 0);
	if (ret) {
		perror("obmm_unimport failed.\n");
		exit(EXIT_FAILURE);
	}

	/* To unpreimport a range, info.pa and info.length must exactly
	 * match the preimport parameters. All other fields have no effect. */
	info.pa = pa;
	info.length = size;
	ret = obmm_unpreimport(&info, 0);
	if (ret) {
		perror("obmm_unpreimport() failed.\n");
		exit(EXIT_FAILURE);
	}

	return 0;
}
```
