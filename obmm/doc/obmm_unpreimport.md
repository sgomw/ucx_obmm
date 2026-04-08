# obmm_unpreimport: 解除远端内存预引入

## 名称 NAME

`obmm_unpreimport` - 解除远端内存预引入

## 库 LIBRARY

OBMM用户态库 (libobmm)

## 摘要 SYNOPSIS

```c
#include <libobmm.h>
int obmm_unpreimport(const struct obmm_preimport_info *preimport_info, unsigned long flags);
```

## 描述 DESCRIPTION

解除一段内存的预引入。

仅有当预上线地址段未实际上线时，预引入才能解除。

### Input Parameters

**preimport_info**：匹配预上线地址段的信息。数据结构描述见 obmm_preimport(3)。

通过pa、length信息寻找相应的预上线信息进行释放。


对于上述场景中未使用的信息，obmm不会进行校验。

| 字段      | 描述                                              |
| --------- | ------------------------------------------------------ |
| pa        | 预上线内存的物理地址基地址，用于预上线信息匹配         |
| length    | 预上线 NUMA 节点所能容纳的最大内存，用于预上线信息匹配 |
| scna      | 忽略                                                   |
| dcna      | 忽略                                                   |
| seid      | 忽略                                                   |
| deid      | 忽略                                                   |
| base_dist | 忽略                                                   |
| numa_id   | 忽略                                                   |
| priv_len  | 忽略                                                   |
| priv      | 忽略                                                   |

**flags**：选项（预留，当前未使用，必须配置为0）。

## 返回值 RETURN VALUE

成功时返回0。

失败时，返回-1，详细的错误类型存储在`errno`中。

## 错误 ERRORS

故障码对应的部分情形如下：

* `ENOENT`：要移除的远程内存块不存在或已被释放。
* `EINVAL`：传入的 preimport_info 为空；flags 包含未定义位；物理地址 pa 对应非预导入区域；卸载区段起始地址和长度未精准匹配。
* `EAGAIN`: 预导入过程未完成，稍后再试。
* `EBUSY` : 待卸载区域正在使用，被其他进程占用。
* `EFAULT`: 未找到物理地址对应信息。
## 约束 CONSTRAINTS

暂无

## 附注 NOTES

暂无

## 样例 EXAMPLES

见 obmm_preimport(3) 。
