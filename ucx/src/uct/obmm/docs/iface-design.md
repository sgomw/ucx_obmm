# OBMM UCT 阶段一设计：iface 接口

## 1. 职责
- 承载 TL 的运行时入口：`iface open/query/address/reachability/fence/flush`。
- 在不实现真实数据通路的前提下，提供可被 UCT/ucx_info 消费的最小 iface 能力模型。

## 2. 结构体与配置
- `uct_obmm_iface_t`
  - 继承 `uct_base_iface_t`
  - 内置 `bandwidth` 配置缓存（语义与 `sm` 保持一致）
  - 增加 `id` 作为本 iface 连接标识（用于 v2 reachable 与 ep connected 判定）
- `uct_obmm_iface_config_t`
  - 继承 `uct_iface_config_t`
  - 增加 `BW` 配置项（默认值与 `sm` 一致：`12179MBs`）

## 3. 回调与行为
- `iface_query`
  - 暴露 `CONNECT_TO_IFACE / CB_SYNC / EP_CHECK`
  - 数据面能力（AM/PUT/GET）上限统一置 0，明确“仅骨架”
- `iface_get_device_address`
  - OBMM 内部独立实现（与 `sm` 语义一致）
- `iface_get_address`
  - 返回 `id`
- `iface_is_reachable_v2`
  - 校验参数完整性
  - 校验本地 `id` 与远端 `iface_addr` 一致
  - 叠加 `obmm` 本地可达性检查 + `uct_iface_scope_is_reachable`
- `iface_fence/iface_flush`
  - `fence` 为 OBMM 内部独立实现（与 `sm` 语义一致），`flush` 复用 `base` 通用实现

## 4. 错误处理策略
- 数据面 API 全部显式绑定 `UCS_ERR_UNSUPPORTED`。
- reachable 参数不合法或地址不匹配时返回不可达并写入 info buffer。

## 5. 阶段一已实现/未实现
- 已实现：iface 生命周期、属性查询、地址/可达性、TL 绑定。
- 未实现：OBMM 映射状态驱动的真实带宽/时延模型，事件通知与异步进度。
