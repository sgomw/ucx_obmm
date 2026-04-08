# obmm_set_ownership: 变更进程对 OBMM 内存的权限

## 名称 NAME

`obmm_set_ownership` - 变更进程对 OBMM内存的权限

## 库 LIBRARY

OBMM用户态库 (libobmm)

## 摘要 SYNOPSIS

```c
#include <libobmm.h>
int obmm_set_ownership(int fd, void *start, void *end, int prot);
```

## 描述 DESCRIPTION

变更进程对 OBMM 内存的权限，仅对通过字符设备映射的 cacheable 内存有效，即fd 创建时未指定 O_SYNC 的内存。

在 OBMM cacheable 模型中，当本机有至少一个进程以可写方式映射时，本机即具备 host 级的写权限，其他 host 上不应有任何可读或可写的 cacheable 映射。

当本机没有进程以可写方式映射，但存在至少一个进程以只读方式映射时，本机即具备 host 机 的读权限，其他 host 上不应有任何可写的 cacheable 映射。

请注意：

* 如果 fd 在创建时指定了 O_SYNC flag，其对应的NC映射不能使用`obmm_set_ownership`。
* 如果同一个页面被多个进程以可写方式映射，OBMM 仅保证在最后一个具备写权限的进程释放写权限时（转入读、空权限或者解除映射）发起硬件层面的缓存写回。
* 如果同一个页面被多个进程以只读或读写权限映射，OBMM 仅保证在最后一个具备访问权限的进程释放权限（转入空权限或者解除映射）时会发起硬件的缓存无效化。
* obmm_set_ownership 不是缓存回刷的唯一触发因素，在系统运行中，缓存被持续使用，硬件会自发地进行缓存逐出，dirty cache 被写入远端内存的时间并不固定。

### Input Parameters

**fd**：obmm_shmdev(4) 内存字符设备的文件描述符。

**start**：权限变更的起始虚拟地址，该地址应按照PAGE_SIZE对齐。

**end**：权限变更的终止虚拟地址（该地址本身不在变更范围内），按PAGE_SIZE对齐。
[start, end) 所表示的地址区间应落在 mmap 映射的地址区间内，且长度大于零。

**prot**：目标权限状态。

- PROT_NONE：无权限
- PROT_READ：读权限
- PROT_WRITE (或 PROT_READ | PROT_WRITE)：读写权限

## 返回值 RETURN VALUE

成功时返回0。

失败时，返回-1，详细的错误类型存储在`errno`中。

## 错误 ERRORS

故障码对应的部分情形如下：

* `EINVAL`: 
    * 目标权限只有 PROT_NONE 、 PROT_READ 和 PROT_WRITE；
    * 更新操作非法：该映射为 non-cacheable 映射;起始、终止地址未对齐 PAGE_SIZE；start < end。
* `EBUSY`:
    * 目标为 PROT_READ：区间内某个PAGE的读权限映射数量达到最大值；
    * 目标为 PROT_WRITE: 区间内某个PAGE的写权限映射数量达到最大值；
* `EFAULT`: 对应的更新区域 vma not found ;待更新内存区域映射的文件和目标设备不一致；更新区域超出 VMA 范围。
* `ENOTRECOVERABLE`: 缓存刷新失败。


## 约束 CONSTRAINTS

见 libobmm(3) 所描述的一致性模型。
当前不允许对NC映射的obmm内存更改一致性状态。

## 附注 NOTES

暂无
 