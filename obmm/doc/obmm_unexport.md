# obmm_unexport: 取消本地内存导出

## 名称 NAME

`obmm_unexport` - 取消本地内存导出

## 库 LIBRARY

OBMM用户态库 (libobmm)

## 摘要 SYNOPSIS

```c
#include <libobmm.h>
int obmm_unexport(mem_id id, unsigned long flags);
```

## 描述 DESCRIPTION

内存提供方根据内存编号回收导出内存。

### Input Parameters

**id**：要回收的内存的编号。

**flags**：选项（预留，当前没有使用，必须配置为0）。

## 返回值 RETURN VALUE

成功时返回0。

失败时，返回-1，详细的错误类型存储在`errno`中。

## 错误 ERRORS

故障码对应的部分情形如下：

* `ENOENT`：传入的 memid 没有对应的 OBMM 内存。
* `EINVAL`：传入了未定义的 flags，传入的 memid 为 OBMM_INVALID_MEMID (0) 或对应引入内存。
* `EBUSY` : region 区域已被占用。

## 约束 CONSTRAINTS

作为单机组件，OBMM无法确认导出的内存是否还有远端使用者，提供方回收内存时，**用户需要保证该远端使用者已停止使用该内存**。否则，使用该内存的远端进程将处于不可预测的状态，可能遭遇进程崩溃、数据不一致、硬件故障上报、kernel panic等非预期后果。

## 附注 NOTES

暂无

## 样例 EXAMPLES

见 obmm_export(3) 。
