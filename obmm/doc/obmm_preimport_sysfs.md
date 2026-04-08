# obmm_preimport_sysfs: OBMM 预引入地址段 sysfs

obmm_preimport sysfs ，为囊括系统内所有预引入地址段的 sysfs 文本文件。

该文件为目录为 `/proc/obmm/preimport_info`。

对于每一个由 obmm_preimport(3) 创建的预引入地址段，都有在该文件中有一行描述。

从左到右，各列依次为：

* 起始物理地址：十六进制数，预引入地址段的最小有效地址。
* 结束物理地址：十六进制数，预引入地址段的最大有效地址。
* scna：十六进制数，内存使用方 bus controller 的 clan network address，含义详见 UB 协议。
* dcna：十六进制数，内存提供方 bus controller 的 clan network address，含义详见 UB 协议。仅记录，不参与通路配置
* seid：十六进制数，内存使用方 bus controller 的 entity id，含义详见 UB 协议。与scna归属同一个bus controller。以u64 : u64 格式打印
* deid：十六进制数，内存提供方 bus controller 的 entity id，含义详见 UB 协议。仅记录，不参与通路配置。以u64 : u64 格式打印
* numa_id：十进制数，预引入地址段所属 的 NUMA 节点。

注：为和/proc/iomem, /proc/\$pid/maps 等标准维测文件保持一致，物理地址段没有附带0x前缀，但仍为16进制数表示。

示意图：
```
cat /proc/obmm/preimport_info
start           - end           :       dcna    scna    deid            seid            nid
50000000000     - 50007ffffff   :       0x0     0x441   0x12:0x13       0x10:0x11       2
```