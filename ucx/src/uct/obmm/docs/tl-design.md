# OBMM UCT 阶段一设计：TL 注册接口

## 1. 职责
- 将 `obmm` TL 挂到 `uct_obmm_component` 的 TL 列表中。
- 提供 TL 设备查询入口，使 `ucx_info -d` 能通过标准组件枚举链路发现该 TL。

## 2. 关键对象与宏
- `UCT_TL_DEFINE_ENTRY(&uct_obmm_component, obmm, ...)`
  - TL 名称：`obmm`
  - TL 配置前缀：`OBMM_`
  - TL 设备查询：`uct_obmm_iface_query_tl_devices`
- `UCT_SINGLE_TL_INIT(&uct_obmm_component, obmm,,,)`
  - 静态构建初始化注册点。

## 3. 设备查询策略
- 复用共享内存通用路径：`uct_sm_base_query_tl_devices`。
- 设备名沿用 SHM 语义（`memory`），设备类型为 `UCT_DEVICE_TYPE_SHM`。

## 4. 错误处理策略
- TL 设备查询直接透传 `uct_sm_base_query_tl_devices` 的返回值。
- 不做静默 fallback。

## 5. 阶段一已实现/未实现
- 已实现：TL 入口注册、静态初始化注册、设备查询挂接。
- 未实现：OBMM 设备级拓扑/NUMA/权限等高级能力暴露。
