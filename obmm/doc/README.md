# OBMM 文档

本目录存放 OBMM 文档，和主线代码保持同步。

综合考虑易用性和社区规范一致性之后，使用中文markdown管理，分成三部分组织：

1. 用户态库接口文档，对应 UNIX man pages section 3
2. 设备文档，对应 UNIX man pages section 4
3. sysfs文档，对应 UNIX man pages section 5

用户态库接口文档

| 文档                  | 内容                                                         |
| --------------------- | ------------------------------------------------------------ |
| libobmm.md            | libobmm 接口总览、核心数据结构说明、使用模型说明、粒度说明   |
| obmm_export.md        | `obmm_export()`接口说明                                      |
| obmm_unexport.md      | `obmm_unexport()` 接口说明                                   |
| obmm_import.md        | `obmm_import()` 接口说明                                     |
| obmm_unimport.md      | `obmm_unimport()` 接口说明                                   |
| obmm_preimport.md     | `obmm_preimport()` 接口说明                                  |
| obmm_unpreimport.md   | `obmm_unpreimport()` 接口说明                                |
| obmm_set_ownership.md | `obmm_set_ownership()` 接口说明                              |
| obmm_query.md         | `obmm_query_memid_by_pa()`, `obmm_query_pa_by_memid()` 接口说明 |


设备文档

| 文档           | 内容                                  |
| -------------- | ------------------------------------- |
| obmm.md        | obmm.ko 参数说明与 /dev/obmm 设备说明 |
| obmm_shmdev.md | obmm_shmdev\${mem_id} 设备使用说明    |


sysfs 文档

| 文档                    | 内容                                                      |
| ----------------------- | --------------------------------------------------------- |
| obmm_shmdev_sysfs.md    | /sys/devices/obmm/obmm_shmdev\${mem_id}/ 维测目录内容说明 |
| obmm_preimport_sysfs.md | /proc/obmm/preimport_info 维测文件格式说明                |

目前各文档已初步成型。errno、编程 demo 持续补充中。
