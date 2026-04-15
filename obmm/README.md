# OBMM: 基于所有权的内存管理组件

[![License](https://img.shields.io/badge/License-GPL--2.0-blue.svg)](https://opensource.org/licenses/GPL-2.0)
[![Kernel Version](https://img.shields.io/badge/Kernel-6.x%2B-green.svg)](https://www.kernel.org/)
[![Unified Bus](https://img.shields.io/badge/SIG--Long-%E7%81%B5%E8%A1%A2(UnifiedBus)-green.svg)](https://www.kernel.org/)
[![CXL](https://img.shields.io/badge/SIG--Long-CXL-red.svg)](https://www.kernel.org/)

---

**中文版** | **[🌐 English Version](README.en.md)**

---

OBMM (Ownership Based Memory Management) 是面向超节点环境的内核内存管理系统，支持跨节点的物理内存共享。该系统通过内核模块 (`obmm.ko`) 和用户态库 (`libobmm.so`) 提供高效的远程内存访问能力。

**重要背景**: 当前很多 sig-Long 总线技术（如 Unified Bus、CXL）中，跨节点数据一致性支持存在重大限制，导致多节点同时访问同一内存区域时可能出现数据竞争和不一致问题。OBMM 的**所有权机制**正是为了解决这一关键挑战而设计，通过确保任意时刻只有一个节点拥有内存的写入权限，从而在缺乏硬件一致性保证的环境中实现安全、可靠的跨节点内存共享。

## 🚀 核心特性

- **所有权管理**: 基于所有权的内存访问控制，确保在缺乏硬件一致性保证的多节点环境中实现数据一致性
- **跨节点内存共享**: 在单机内管理远端内存，支持内存导出 (export) 和导入 (import)
- **透明访问**: 应用可使用普通的 `load`、`store` 指令访问远端内存
- **NUMA 支持**: 将远端内存作为远程 NUMA 节点上线
- **高性能**: 通过预导入 (preimport) 优化降低热路径时延
- **设备接口**: 提供字符设备接口 (`/dev/obmm_shmdev{mem_id}`) 用于内存映射

## 🎯 主要应用场景

### 场景一：单节点内存扩展

在单节点环境中扩展可用内存容量，突破本地物理内存限制。

**具体应用实例**：

- **大内存数据库**: 数据库服务器需要处理超过本地物理内存容量的数据集，通过 OBMM 导入远程内存作为 NUMA 节点，透明扩展内存容量
- **内存密集型计算**: 科学计算、机器学习训练等场景需要大量内存，通过 OBMM 动态扩展内存资源
- **内存层次优化**: 将冷数据迁移到远程内存，热数据保留在本地内存，实现智能的内存分层管理
- **容器内存扩展**: 为容器提供额外的内存资源，突破单机内存配额限制

### 场景二：节点间共享内存

在多节点环境中实现高效的内存数据共享和协作。

**具体应用实例**：

- **分布式缓存**: 多个应用节点共享同一份缓存数据，避免重复存储，提高缓存命中率
- **实时数据共享**: 金融交易系统中，多个交易节点共享实时行情数据，确保数据一致性
- **协同处理流水线**: 多个处理节点共享输入数据和中间结果，减少数据传输开销
- **高可用性集群**: 主备节点共享状态数据，实现快速故障切换和状态同步

## 🚀 未来应用可能性

OBMM 的所有权机制和跨节点内存共享能力为未来更多创新应用场景提供了技术基础：

### 边缘计算与云边协同
- **边缘节点缓存**: 边缘设备共享缓存数据，减少云端访问延迟
- **分布式推理**: AI 推理任务在边缘节点间共享模型和数据
- **内容分发网络**: CDN 节点间共享热门内容，提高访问效率

### 异构计算加速
- **GPU/FPGA 内存共享**: CPU 与加速器间共享大容量内存池
- **异构内存管理**: 统一管理不同类型的内存介质（DDR、HBM、持久内存）
- **计算-存储融合**: 存储节点直接参与计算，减少数据搬移

### 新型计算范式
- **内存中心计算**: 以内存为中心的计算架构，突破传统 CPU 中心限制
- **近数据计算**: 在数据存储位置附近执行计算，最小化数据移动
- **分布式内存计算**: 将计算任务分布到内存所在的各个节点

### 系统软件创新
- **新型虚拟化**: 基于内存共享的轻量级虚拟化方案
- **分布式操作系统**: 跨节点的统一内存视图和进程管理
- **智能内存调度**: AI 驱动的智能内存分配和迁移策略

> **技术愿景**: OBMM 不仅解决当前的一致性问题，更为未来的计算架构创新提供基础支撑。随着硬件技术的发展，OBMM 将持续演进，支持更丰富的应用场景和更高的性能要求。

## 📚 文档

### API 文档
- **[用户态库总览](doc/libobmm.md)** - libobmm 接口总览、核心数据结构说明、使用模型说明
- **[内存导出](doc/obmm_export.md)** - `obmm_export()` 接口说明
- **[内存取消导出](doc/obmm_unexport.md)** - `obmm_unexport()` 接口说明
- **[内存导入](doc/obmm_import.md)** - `obmm_import()` 接口说明
- **[内存取消导入](doc/obmm_unimport.md)** - `obmm_unimport()` 接口说明
- **[内存预导入](doc/obmm_preimport.md)** - `obmm_preimport()` 接口说明
- **[取消预导入](doc/obmm_unpreimport.md)** - `obmm_unpreimport()` 接口说明
- **[所有权设置](doc/obmm_set_ownership.md)** - `obmm_set_ownership()` 接口说明
- **[内存查询](doc/obmm_query.md)** - `obmm_query_memid_by_pa()`, `obmm_query_pa_by_memid()` 接口说明

### 设备文档
- **[OBMM 内核模块](doc/obmm.md)** - obmm.ko 参数说明与 /dev/obmm 设备说明
- **[共享内存设备](doc/obmm_shmdev.md)** - obmm_shmdev\${mem_id} 设备使用说明

### 系统接口文档
- **[共享内存设备 Sysfs](doc/obmm_shmdev_sysfs.md)** - /sys/devices/obmm/obmm_shmdev\${mem_id}/ 维测目录内容说明
- **[预导入 Sysfs](doc/obmm_preimport_sysfs.md)** - /proc/obmm/preimport_info 维测文件格式说明

### 其他文档
- **[文档目录说明](doc/README.md)** - 文档组织结构说明
- **[发布说明](doc/RELEASE-NOTES.md)** - 版本发布信息

## 🤝 社区支持

- 问题报告: [Gitee Issues](https://gitee.com/openeuler/obmm/issues)
- 文档 Wiki: [项目 Wiki](https://gitee.com/openeuler/obmm/wikis)

## 📄 许可证

本项目内核态驱动OBMM采用 [GPL-2.0](LICENSE/GPL-2.0) 许可证。

本项目用户态库Libobmm采用 [Mulan-PSL-v2](LICENSE/Mulan-PSL-v2) 许可证。

## 🏷️ 开源
| 时间 | 事件 | 里程碑 | 
|------|------|------------|
| 2025.09.18 | 华为全联接大会| UB协议发布 |
| 2025.11.14 | openEuler Summit 2025 | OBMM专题讨论 |
| 2025.11.18 | Gitee 开源 | OBMM提交首个社区 Pull Request |
| 2025.12.30 | openEuler 24.03 SP3发布| 作为核心组件集成 |

## 🔗 相关链接

- [Linux 内核官网](https://www.kernel.org/)
- [openEuler 操作系统](https://www.openeuler.org/)
- [灵衢互连总线](https://unifiedbus.com/)
- [openEuler 内核开发文档](https://gitee.com/openeuler/kernel/wikis/Home)

---

**注意**: 本项目需要相应的硬件平台支持和内核态修改支持（相关代码已提交至 [openEuler Kernel PR #19100](https://gitee.com/openeuler/kernel/pulls/19100)）。在部署前请确认您的环境满足所有依赖要求。