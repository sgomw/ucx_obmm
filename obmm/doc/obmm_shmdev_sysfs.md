# obmm_shmdev_sysfs: OBMM 内存设备 sysfs

obmm_shmdev sysfs ，为 obmm_shmdev(4) 的 sysfs 文件目录。若一段 OBMM 内存的 memid 为 `${id}`，其sysfs 目录所在的路径为`/sys/devices/obmm/obmm_shmdev${id}/` 。

通过 sysfs 目录下有多个只读文本文件和只读二进制文件。每个文件展示了该内存设备的一项属性。可用于维测与调试，不建议用于控制面功能和性能路径。

## 属性总览

根据内存属性的适用场景，我们可将其分为了三类：

* 通用信息：提供方、使用方共有的属性，属性文件位于*根路径*下，例如内存长度，类型等。
* 提供方信息：专属于提供方的信息，属性文件位于`export_info`*子目录*下，如内存在各本地 NUMA 节点上的分布情况。
* 使用方信息：专属于使用方的信息，属性文件位于`import_info`，如远端映射到本端后的物理地址等。

有些属性可能仅在特定条件满足时适用。例如 import_info/numa_id 文件仅在以 NUMA 方式引入时才会出现。

```
# export 内存目录结构示意
obmm_shmdev${id}/
├── type
├── ...
├── priv
└── export_info/
    ├── node_mem_size
    ├── ...
    └── uba
```

```
# import 内存目录结构示意
obmm_shmdev${id}/
├── type
├── ...
├── priv
└── import_info/
    ├── pa
    ├── ...
    └── scna
```

这些属性多数都是文本（ASCII）文件，可通过shell 的 `cat`、标准 C 的 `fscanf` 等进行直接操作。少数例外为二进制文件，如用户定义的私有数据 `priv` ，用户可通过 shell 的 `xxd`， POSIX C 的 `read` 进行读取。

## 通用信息

通用信息位于 `/sys/devices/obmm/obmm_shmdev${mem_id}/` 的根层级，包含如下内容：

**type**
类型：文本、字符串（`export`、`import`二者之一）
描述：`export` 代表该内存为提供方内存，DRAM位于本地，`import` 代表该内存为从远端引入的内存。

**size**
类型：文本、十六进制数
描述：数值代表该内存的总大小，以字节为单位。如 `0x200000` 代表该内存共 2MB。

**priv_len**
类型：文本、十进制数，不大于`OBMM_MAX_PRIV_LEN`的非负数
描述：内存私有元数据的大小，以字节为单位，由用户创建内存设备时指定。

**priv**
类型：二进制文件
描述：内存私有元数据，内容由用户创建内存设备时传入，文件大小为`OBMM_MAX_PRIV_LEN`，其中偏移量超过`priv_len`的部分仍可读取，但其二进制值恒为0。

**allow_mmap**
类型：文本、十进制数
描述：`0` 代表内存 obmm_shmdev(4) 字符设备无法通过 mmap(2) 映射，`1` 代表可以通过 mmap 映射。

## 提供方信息

**export_info/memory_from_user**
类型：文本、十进制数
描述：`0` 代表导出内存是由 OBMM 分配，`1` 代表导出内存来自导出进程。

**export_info/node_mem_size**
类型：文本、十六进制CSV数组（由`,`分隔的十六进制值）
描述：内存的在各本地 NUMA node 上的分布情况，数值出现的顺序和NUMA节点编号对应，如`0x400000,0x0,0x200000`代表从 node 0、和 node 2 分别上分别申请出了4MB和2MB；数组长度是变长的，总长度由实际提供内存的节点的ID决定，如在4节点机器上如果只有node 1提供内存，那么数组长度是2。

**export_info/tokenid**
类型：文本、十六进制数
描述：该内存所属的 tokenid，含义详见 UB 协议。

**export_info/uba**
类型：文本、十六进制数
描述：该内存对应的 UBA 基地址，含义详见 UB 协议。

**export_info/deid**
类型：文本、十六进制数, 以u64 : u64 格式打印
描述：内存提供方 bus controller 的 entity id，含义详见 UB 协议。

## 使用方信息

**import_info/pa**
类型：文本、十六进制数
描述：远端内存引入后，在本端对应的物理地址基地址。

**import_info/scna**
类型：文本、十六进制数
描述：内存使用方 bus controller 的 clan network address，含义详见 UB 协议。

**import_info/dcna**
类型：文本、十六进制数
描述：内存提供方 bus controller 的 clan network address，含义详见 UB 协议。

**import_info/seid**
类型：文本、十六进制数, 以u64 : u64 格式打印
描述：内存使用方 bus controller 的 entity id，含义详见 UB 协议。

**import_info/deid**
类型：文本、十六进制数, 以u64 : u64 格式打印
描述：内存提供方 bus controller 的 entity id，含义详见 UB 协议。

**import_info/numa_id**
类型：文本、十进制数
描述：远端内存对应的 remote NUMA ID。如果 obmm_import(3) 时未指定*OBMM_IMPORT_FLAG_NUMA_REMOTE*，没有该文件。

**import_info/preimport**
类型：文本、十进制数
描述：`0` 代表该内存未经过 preimport 加速上线，`1` 代表该内存通过 preimport 方式上线。如果 obmm_import(3) 时未指定*OBMM_IMPORT_FLAG_NUMA_REMOTE*，没有该文件。

使用方信息是否适用受 OBMM 工作模式、import flags 配置等多方面的影响。下表为其总览表：

| 属性      | 适用场景                   |
| --------- | ------------------------ |
| pa        | 所有场景                  |
| scna      | 所有场景                  |
| dcna      | 所有场景，仅记录，不参与通路配置      |
| seid      | 所有场景                  |
| deid      | 所有场景，仅记录，不参与通路配置      |
| numa_id   | 内存以 NUMA 方式引入       |
| preimport | 内存以 NUMA 方式引入       |

# 常见问题

使用 C 程序访问时 sysfs接口时，如果出现报错 `Too many open files`，一般有两种情况：

1. 系统的文件描述符上限过低：可使用 `ulimit -n` 查看当前配置值，通过 `ulimit -n $new_limit` 调高上限
2. 程序存在文件描述符泄漏：可通过 `ls /proc/$pid/fd | wc -l` 等方法验证进程占用的描述符数量是否合理，然后排查文件中是否有遗漏了 `close(fd)` 语句。
