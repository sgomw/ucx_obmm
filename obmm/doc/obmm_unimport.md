# obmm_unimport: 取消远端内存引入

## 名称 NAME

`obmm_unimport` - 取消远端内存引入

## 库 LIBRARY

OBMM用户态库 (libobmm)

## 摘要 SYNOPSIS

```c
#include <libobmm.h>
int obmm_unimport(mem_id id, unsigned long flags);
```

## 描述 DESCRIPTION

内存使用方根据内存编号释放引入内存。

### Input Parameters

**id**：要回收的内存的编号

**flags**：选项（预留，当前没有使用，必须配置为0）

## 返回值 RETURN VALUE

成功时返回0。

失败时，返回-1，详细的错误类型存储在`errno`中。

## 错误 ERRORS

故障码对应的部分情形如下：

* `ENOENT`：传入的 memid 没有对应的 OBMM 内存。
* `EINVAL`：传入了未定义的 flags，传入的 memid 为 OBMM_INVALID_MEMID (0) 或对应导出内存。
* `EBUSY` : region区域已被占用。
## 约束 CONSTRAINTS

暂无

## 附注 NOTES

暂无

## 样例 EXAMPLES

见 obmm_import(3) 。
