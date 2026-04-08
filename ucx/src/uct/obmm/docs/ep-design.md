# OBMM UCT 阶段一设计：ep 接口

## 1. 职责
- 提供 endpoint 的创建/销毁骨架。
- 在连接检查路径上完成与 iface 地址标识的一致性判定。
- 为后续 OBMM 数据路径（AM/RMA/原子）保留可扩展承载点。

## 2. 结构体与构造
- `uct_obmm_ep_t`
  - 继承 `uct_base_ep_t`
- `UCS_CLASS_INIT_FUNC(uct_obmm_ep_t, const uct_ep_params_t *params)`
  - 要求 `DEV_ADDR + IFACE_ADDR` 参数齐全（`UCT_EP_PARAMS_CHECK_DEV_IFACE_ADDRS`）
  - 绑定到 `uct_obmm_iface_t` 的 base ep

## 3. 关键行为
- `ep_create/ep_destroy`
  - 使用 UCX class 机制注册到 iface ops
- `ep_is_connected`
  - 先走 `uct_base_ep_is_connected` 通用检查
  - 再校验 `params->iface_addr` 对应的 `id` 与本 iface `id` 一致

## 4. 错误处理策略
- 参数缺失由 `UCT_EP_PARAMS_CHECK_DEV_IFACE_ADDRS` 直接失败。
- 连接判定失败返回 0，不做隐式重试。

## 5. 阶段一已实现/未实现
- 已实现：创建销毁、最小 connected 判定。
- 未实现：发送队列、远端地址映射、OBMM 句柄缓存、pending 流控与重试。
