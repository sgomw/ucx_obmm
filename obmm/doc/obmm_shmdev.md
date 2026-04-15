# obmm_shmdev: OBMM 内存设备

obmm_shmdev 是 OBMM 为管理远端内存创建的字符设备。若一段 OBMM 内存的 memid 为 `${id}`，其设备所在的路径为`/dev/obmm_shmdev${id}` 。

obmm_shmdev 设备为用户态库（libobmm）提供了配置内存设备的 ioctl 接口，为一般用户态应用提供了映射该 OBMM 内存的接口。

obmm_shmdev 可以为 export 设备或 import 设备。其详细属性可以通过 obmm_shmdev_sysfs(5) 查看。

## 创建与销毁

应用应通过 obmm_export(3) 或 obmm_import(3) 创建 OBMM 内存设备。通过 obmm_unexport(3) 或 obmm_unimport(3) 销毁内存设备。

## 映射访问

obmm_shmdev 仅支持使用 POSIX 接口进行映射。一次标准的映射使用过程应包括对该设备的 open(2)、mmap(2)、load/store访问、munmap(2)、close(2)。

### open

```C
int open(const char *pathname, int flags, ...
				  /* mode_t mode */ );
```

在操作 obmm_shmdev 时，flags 可以影响 shmdev 的下列行为：

* 操作权限：O_RDONLY 代表请求只读权限，O_WRONLY 代表请求只写权限（当前无法基于此flag做映射），O_RDWR 代表请求读写权限，三者配置仅配置一项。
* 同步模式：在 flags 中对 O_SYNC 置位时，使用类似同步IO的语义，远端内存将被以 non-cacheable 方式映射；O_SYNC 未置位时，远端内存将被 cacheable 方式映射。如果芯片对物理地址段的映射模式有限制，O_SYNC 的配置应与之匹配。

### mmap

```C
void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
```

在操作 obmm_shmdev 时，各参数还具有如下含义：

* addr：用户态应用期望的虚拟地址，默认为 hint（内核不保证能从分配到该地址）。一般可置为 NULL。
* length：映射内存的长度，此参数必须按页大小对齐。
* prot：映射内存的访问权限。PROT_NONE表示空权限（无映射、无缓存），PROT_READ表示只读权限，PROT_WRITE 和 PROT_READ | PROT_WRITE 表示读写权限。
* flags：根据使用规范，用户需配置 MAP_SHARED flag，不应配置 MAP_ANONYMOUS flag。MAP_PRIVATE语义上不适合配置，如果用户仍要配置MAP_PRIVATE，请注意访存行为仍和MAP_SHARED一致，进程的写入对其它进程都是可见的。
* fd：需为 open(2) 打开 obmm_shmdev 设备所创建的文件描述符
* offset：映射域段的起始偏移量，此参数必须按页大小对齐。

此外，如果fd在打开时指定了O_SYNC的flag，那么该次mmap会以NC的方式进行映射。此时，prot最终会呈现为NC+PROT_NONE/PROT_READ/PROT_WRITE的模式。该映射模式下不允许用户进行一致性变更；且不允许与CC映射的方式混用。

mmap 失败时，返回 MAP_FAILED（-1）；成功时，会返回一个虚拟地址。应用可基于该虚拟地址进行 load, store 访问。用户所做的访问必须与当前该段内存的权限一致。如果不一致，可能产生 bus error。

OBMM支持部分映射，同一进程mmap和munmap的范围必须保持一致，不同进程可以使用不同的页范围进行映射。

在访问过程中，用户可通过 obmm_set_ownership(3) 切换权限，实现细粒度的数据跨机共享。注意，NC映射不能使用obmm_set_ownership(3)切换权限。

映射时，应用还需要满足以下限制：

* obmm_shmdev 对应的内存具备 allow_mmap 属性：allow_mmap 属性由创建 OBMM 内存设备的 flags（ *OBMM_EXPORT_FLAG_ALLOW_MMAP* 或 *OBMM_IMPORT_FLAG_ALLOW_MMAP*） 配置。
* 对每一个 cacheable 映射的页，obmm同时允许最多2^16-1个写权限访问者，2^16-1个读权限访问者进行映射以及任意数量的空权限映射；没有 ownership 概念的 non-cacheable 页不受前述限制。
* 进程实际的映射数上限还受内核文件描述符上限、进程映射数量上限等配置的制约，不仅由 OBMM 的状态上限决定。

当 obmm_shmdev 设备被打开或 mmap 时，内存设备均无法销毁。销毁内存设备时，用户应先解除映射，关闭文件描述符。

#### PMD映射
obmm_shmdev字符设备mmap提供PMD映射方式，通过增加映射粒度来降低Page Table Walk耗时，降低TLB Miss率，从而降低平均访存时延。

##### 使用方式
`#define OBMM_MMAP_FLAG_HUGETLB_PMD (1UL << 63)`
在mmap时，通过指定偏移为(`OBMM_MAP_FLAG_HUGETLB_PMD` | offset)实现指定PMD映射的效果。

mmap其余参数描述有所变化：
* addr：置为 NULL时由内核申请PMD_SIZE对齐的虚拟地址，不为NULL时则以用户传入值为引导值，内核自动调整到PMD_SIZE对齐。
* length：映射内存的长度，此参数必须按PMD_SIZE大小对齐。

##### 使用约束
1. 一个obmm_shmdev不允许混合不同粒度映射。设备首次映射时会记录映射粒度。
2. 通过PMD方式映射的虚拟地址不支持使用`obmm_set_ownership`接口维护一致性。

## 样例

以下函数展示了应用通过 POSIX 标准接口访问 OBMM 内存设备的过程。函数使用 open(2), mmap(2) 映射内存设备。使用 cacheable 内存时，需要使用 obmm_set_ownership(3) 接口维护 libobmm(3) 中描述的一致性模型。访问结束后，函数使用 munmap(2) 和 close(2) 解除了对设备的映射和占用。

```c
#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <libobmm.h>

#define SZ_2M	(1UL << 21)

#define MAX_OBMM_MEMDEV_PATH	128
int memdev_demo(mem_id id, size_t size)
{
	int ret, fd, *value;
	void *ptr;
	char memdev_path[128];

	ret = snprintf(memdev_path, sizeof(memdev_path), "/dev/obmm_shmdev%lu", id);
	if (ret < 0 || ret >= (int)sizeof(memdev_path)) {
		fprintf(stderr, "Failed to construct OBMM memdev path.\n");
		exit(EXIT_FAILURE);
	}

	fd = open(memdev_path, O_RDWR);
	if (fd == -1) {
		perror("open() failed on OBMM memdev.\n");
		exit(EXIT_FAILURE);
	}

	/* Map the memory device with NONE access right. */
	ptr = mmap(NULL, size, PROT_NONE, MAP_SHARED, fd, 0);
	if (ptr == MAP_FAILED) {
		perror("mmap() failed on OBMM memdev.\n");
		exit(EXIT_FAILURE);
	}
	/* Map char device succeeded. */

	/* Do your work here. Here is a simple non-atomic increment example. */
	ret = obmm_set_ownership(fd, ptr, (void*)((uintptr_t)ptr + SZ_2M), PROT_WRITE);
	if (ret == -1) {
		perror("obmm_set_ownership() failed.\n");
		exit(EXIT_FAILURE);
	}
	value = (int*)ptr;
	*value = *value + 1;
	ret = obmm_set_ownership(fd, ptr, (void*)((uintptr_t)ptr + SZ_2M), PROT_NONE);
	if (ret == -1) {
		perror("obmm_set_ownership() failed.\n");
		exit(EXIT_FAILURE);
	}

	/* Cleanup.*/
	ret = munmap(ptr, size);
	if (ret == -1) {
		perror("munmap() failed on OBMM memdev pointer.\n");
		exit(EXIT_FAILURE);
	}

	ret = close(fd);
	if (ret == -1) {
		perror("close() failed on OBMM memdev.\n");
		exit(EXIT_FAILURE);
	}

	return 0;
}
```

以下函数展示了应用通过 POSIX 标准接口访问 OBMM 内存设备的过程。函数使用 open(2), mmap(2) 以PMD方式映射non_cachable属性内存设备，访问结束后，函数使用 munmap(2) 和 close(2) 解除了对设备的映射和占用。

```c
#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <libobmm.h>

#define SZ_2M	(1UL << 21)

#define MAX_OBMM_MEMDEV_PATH	128
int memdev_demo(mem_id id, size_t size)
{
	int ret, fd, *value;
	void *ptr;
	char memdev_path[128];

	ret = snprintf(memdev_path, sizeof(memdev_path), "/dev/obmm_shmdev%lu", id);
	if (ret < 0 || ret >= (int)sizeof(memdev_path)) {
		fprintf(stderr, "Failed to construct OBMM memdev path.\n");
		exit(EXIT_FAILURE);
	}

	fd = open(memdev_path, O_RDWR|O_SYNC);
	if (fd == -1) {
		perror("open() failed on OBMM memdev.\n");
		exit(EXIT_FAILURE);
	}

	/* Map the memory device with NONE access right. */
	ptr = mmap(NULL, size, PROT_NONE, MAP_SHARED, fd, OBMM_MMAP_FLAG_HUGETLB_PMD);
	if (ptr == MAP_FAILED) {
		perror("mmap() failed on OBMM memdev.\n");
		exit(EXIT_FAILURE);
	}
	/* Map char device succeeded. */
	value = (int*)ptr;
	*value = *value + 1;
	/* Cleanup.*/
	ret = munmap(ptr, size);
	if (ret == -1) {
		perror("munmap() failed on OBMM memdev pointer.\n");
		exit(EXIT_FAILURE);
	}

	ret = close(fd);
	if (ret == -1) {
		perror("close() failed on OBMM memdev.\n");
		exit(EXIT_FAILURE);
	}

	return 0;
}
```