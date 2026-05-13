# OBMM UCT 阶段一设计：MD 接口

## 1. 职责
- 提供 `obmm` 组件的 MD 生命周期：`query_md_resources -> md_open -> md_query -> close`。
- 对上游暴露“可注册主机内存 + 需要 rkey”的最小语义，满足 `ucx_info -d` 枚举与属性查询。
- 为后续 OBMM 真正映射/导入导出路径预留扩展位。

## 2. 关键对象与回调映射
- `uct_component_t uct_obmm_component`
  - `query_md_resources = uct_md_query_single_md_resource`
  - `md_open = uct_obmm_md_open`
  - `rkey_unpack = uct_obmm_md_rkey_unpack`
  - `rkey_ptr = uct_obmm_rkey_ptr`（OBMM 内部独立实现，语义与 `sm` 一致）
- `uct_md_ops_t md_ops`
  - `query = uct_obmm_md_query`
  - `mem_reg/mem_dereg = uct_md_dummy_mem_*`
  - `mkey_pack = success stub`

## 3. 生命周期与时序
1. `uct_query_components()` 返回 `obmm` 组件。
2. 上层请求 `md_open`，创建 `uct_obmm_md_t` 并绑定 `md_ops`。
3. `md_query` 返回最小 capability。
4. 关闭时释放 `uct_obmm_md_t`。

## 4. 错误处理策略
- `md_open` 内存申请失败返回 `UCS_ERR_NO_MEMORY`。
- 不支持的能力走 `UCS_ERR_UNSUPPORTED`（通过标准 stub 函数）。
- 不做隐式降级，不吞错误。

## 5. 阶段一已实现/未实现
- 已实现：组件注册、MD 打开关闭、最小 capability、rkey unpack 骨架。
- 未实现：OBMM 专用内存映射、导入导出、真实 mkey/rkey 编解码策略。
