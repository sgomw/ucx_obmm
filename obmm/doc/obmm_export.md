# obmm_export: 导出本地内存

## 名称 NAME

`obmm_export`, `obmm_export_useraddr` - 导出本地内存

## 库 LIBRARY

OBMM用户态库 (libobmm)

## 摘要 SYNOPSIS

```c
#include <libobmm.h>
mem_id obmm_export(const size_t length[OBMM_MAX_LOCAL_NUMA_NODES], unsigned long flags, struct obmm_mem_desc *desc);
mem_id obmm_export_useraddr(int pid, void* va, size_t length, unsigned long flags, struct obmm_mem_desc *desc);
```

## 描述 DESCRIPTION

### obmm_export

内存提供方申请并导出一段内存，供其他机器进行访问。函数返回时，导出的内存已经清零，不会有历史数据泄漏。内存的分配方式详见 obmm(4)。

结束使用时，obmm_export 创建的内存，需要用 obmm_unexport(3) 释放。

**地址、长度对齐**

* 传入的内存长度必须按OBMM基础粒度与内存分配器粒度对齐。

#### Input Parameters

**length**：指向一个长度为 `OBMM_MAX_LOCAL_NUMA_NODES` 的数组，数组的第 *i* 个元素的值表示此次export需要从NUMA 节点 *i* 申请的内存大小。

length需要满足如下要求：

1. length[i]必须按OBMM基础粒度对齐，同时满足内存分配器与UMMU的粒度约束。
2. length[i]非零时，其对应的 NUMA 节点 i 必须为有效的本端 NUMA 节点，且所有这样的 NUMA 节点 i 必须属于同一个CPU Socket。
3. length的所有元素之和大于零。

**flags**：导出内存的属性，支持以下 flag
*OBMM_EXPORT_FLAG_FAST*：仅从内存缓冲池中申请内存进行export操作。若内存缓冲池内存不足，不会向系统申请内存，直接返回错误。
*OBMM_EXPORT_FLAG_ALLOW_MMAP*: 允许通过mmap对应memid的字符设备的方式，使用该内存。

**desc**: 指向一个OBMM内存描述符，用于传入内存的属性参数，同时接收地址信息。其中deid、priv_len、priv域段为入参，addr，length，tokenid域段为出参，其他参数会被忽略。

```c
struct obmm_mem_desc {
	uint64_t addr; // 出参：返回此次export生成的uba
	uint64_t length; // 出参：返回length中各元素之和
	/* 128bit eid, ordered by little-endian */
	uint8_t seid[16]; // export流程忽略
	uint8_t deid[16];  // 入参：指定借出内存所在bus controller的eid
	uint32_t tokenid; // 出参：返回此次export生成的tokenid
	uint32_t scna; // export流程忽略
	uint32_t dcna; // export流程忽略
	uint16_t priv_len; // 入参：指定priv[]的长度
	uint8_t  priv[]; // 入参：可选，用户私有数据
}
```

### obmm_export_useraddr

内存提供方对指定进程的一段地址空间调用，pin住并导出这段内存，供其他机器进行访问。

结束使用时，obmm_export_useraddr 创建的内存，需要用 obmm_unexport(3) 释放。释放时，这段内存中的数据不会被清理。

**限制**
- 该接口调用的目标地址段须按照PMD_SIZE对齐，并且其中的映射粒度需要最小是PMD_SIZE，且不低于UMMU粒度约束。目前可以支持hugetlb，或者THP方式的映射。
- 该接口调用后，目标内存会被pin住。
- 该接口调用后，直到unexport执行前，内核态访问目标内存会造成宿主机panic。
- 该接口只能导出权限为可读可写的地址空间。
- 该接口导出的内存，必须在同一个socket内。
#### Input Parameters

**pid**: 指定进程的pid。pid=0 时指定当前调用接口的进程。
**va**: 目标内存虚拟地址段的首指针。
**length**: 目标内存的长度。
length需要满足如下要求：
1. length必须按OBMM基础粒度对齐，同时满足内存分配器与UMMU的粒度约束。

**flags**: 导出内存属性。（预留，当前未使用，必须配置为0）

**desc**: 指向一个OBMM的内存描述符，用于传入内存的属性参数，同时接收地址信息。其中deid、priv_len、priv域段为入参，addr，length，tokenid域段为出参，其他参数会被忽略。

```c
struct obmm_mem_desc {
	uint64_t addr; // 出参：返回此次export生成的uba
	uint64_t length; // 出参：返回导出内存的总大小
	/* 128bit eid, ordered by little-endian */
	uint8_t seid[16]; // export流程忽略
	uint8_t deid[16];  // 入参：指定借出内存所在bus controller的eid
	uint32_t tokenid; // 出参：返回此次export生成的tokenid
	uint32_t scna; // export流程忽略
	uint32_t dcna; // export流程忽略
	uint16_t priv_len; // 入参：指定priv[]的长度
	uint8_t  priv[]; // 入参：可选，用户私有数据
}
```

## 返回值 RETURN VALUE

导出成功时，返回内存编号（memid），导出内存的详细属性将被填入desc中。会在 /dev/ 目录下生成对应的 /dev/obmm_shmdev\${memid} 字符设备。详见 obmm_shmdev(4)。

失败时，返回 `OBMM_INVALID_MEMID`(0)，详细的错误类型存储在`errno`中。

## 错误 ERRORS

故障码对应的部分情形如下：
* `EINVAL` :
  * `length`、`desc` 输入参数不能为 NULL；
  * 私有数据长度超出`OBMM_MAX_PRIV_LEN`限制(`OBMM_MAX_PRIV_LEN` == 512)；
  * 每个 NUMA 节点内存大小`OBMM_BASIC_GRANU` == 2 MB整数倍；
  * 确保所有 NUMA 节点属于同一 cpu socket 中；
  * 每个非零节点内存没有对齐到`OBMM_MEMSEG_SIZE`；
  * 导出内存总和大小不能为 0；
  * obmm_export_user 不允许配置flags为OBMM_EXPORT_FLAG_ALLOW_MMAP或OBMM_EXPORT_FLAG_FAST；
  * 不允许使用未经OBMM定义的export flags（以OBMM_EXPORT_FLAG开头）；
* `ENODEV`: 只允许从已上线的本地NUMA节点分配内存。
* `ENOMEM`：系统内存不足。
* `EEXIST:` 申请`region` 已存在。
* `E2BIG`：请求的 NUMA 数量大于系统支持最大值。
* `ENOSPC`：指定范围内无可用 `memid`。
* `EOVERFLOW`：内存溢出，请求的内存总大小超出`unsigned long`的范围。
* `EPERM`: UMMU 设备过多，超出 `MAX_NUM_UMMU_DEVICES`。
## 约束 CONSTRAINTS

暂无

## 附注 NOTES

暂无

## 样例 EXAMPLES

以下程序导出了一段长度为 2MB 的内存。该内存在本机可通过 obmm_shmdev(4) 设备映射访问。随后程序通过 obmm_unexport(3) 接口回收了这段内存。

```c
#include <stdio.h>
#include <stdbool.h>
#include <libobmm.h>

#define SZ_2M	(1UL << 21)

int export_interface_demo(uint8_t *deid)
{
	int ret;
	mem_id id;
	/* Export 2M memory from node 0 and no memory from other nodes. */
	size_t length[OBMM_MAX_LOCAL_NUMA_NODES] = { SZ_2M };
	/* Allocate memory only from OBMM buffer and create a mappable memory device. */
	unsigned long flags = OBMM_EXPORT_FLAG_FAST | OBMM_EXPORT_FLAG_ALLOW_MMAP;
	/* Specify that this memory device has no private data. */
	struct obmm_mem_desc desc = {
		.priv_len = 0
	};
	memcpy(desc.deid, &deid, 16);
	/* Export memory from OBMM. */
	id = obmm_export(length, flags, &desc);
	if (id == OBMM_INVALID_MEMID) {
		/* Export failed. */
		perror("obmm_export() failed.\n");
		return -1;
	}
	/* Export succeeded. Key parameters written to @desc. */

	/* Do your work here... */

	/* Unexport memory. */
	flags = 0;
	ret = obmm_unexport(id, flags);
	if (ret) {
		perror("obmm_unexport() failed.\n");
		return -1;
	}

	return 0;
}
```

下面示例展示如何在当前进程中申请一页 2MB 的大页（hugetlb/THP），
然后调用 `obmm_export_useraddr` 导出该地址空间。示例中对可能的失败情况做了基本检查；在真实运行环境中，使用者需确保系统已为大页做好配置（例如挂载 hugetlbfs 或启用 THP），且有足够权限。

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>
#include <libobmm.h>

#define SZ_2M (1UL << 21)

int export_useraddr_interface_demo(uint8_t *deid)
{
	size_t size = SZ_2M;
	void *ptr = MAP_FAILED;
	mem_id id;
	int ret;

	ptr = mmap(NULL, size, PROT_READ | PROT_WRITE,
			   MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
	if (ptr == MAP_FAILED) {
		perror("mmap(hugepage) failed");
		return -1;
	}

	struct obmm_mem_desc *desc = malloc(sizeof(*desc));
	if (!desc) {
		perror("malloc desc failed");
		munmap(ptr, size);
		return -1;
	}
	memset(desc, 0, sizeof(*desc));
	desc->priv_len = 0;
	memcpy(desc->deid, &deid, 16);

	/* 导出当前进程的这段地址空间（pid=0 表示当前进程） */
	id = obmm_export_useraddr(0, ptr, size, 0, desc);
	if (id == OBMM_INVALID_MEMID) {
		perror("obmm_export_useraddr() failed");
		free(desc);
		munmap(ptr, size);
		return -1;
	}

	printf("exported mem id: %llu, uba: 0x%llx, size: 0x%llx\n",
		   (unsigned long long)id,
		   (unsigned long long)desc->addr,
		   (unsigned long long)desc->length);

	/* 结束使用：先 unexport，再释放本地映射 */
	ret = obmm_unexport(id, 0);
	if (ret) {
		perror("obmm_unexport() failed");
		/* 继续释放本地资源 */
	}

	free(desc);
	munmap(ptr, size);
	return 0;
}
```
