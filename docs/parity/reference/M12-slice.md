# M12 — Slice（切片修饰器）

- OVITO 官方文档: <https://docs.ovito.org/reference/pipelines/modifiers/slice.html>
- 分类: Modification（离线索引 `sources/modifiers.html` 原文: "Cuts the structure along an infinite plane."）
- 授权层级: OVITO Basic（无 Pro 标记）
- Python API: `ovito.modifiers.SliceModifier`

来源标记说明：**[手册]** = 在线 OVITO 3.16.1 手册页（2026-09-25 抓取，与离线快照同版本）；**[离线]** = 离线 HTML 索引；**[待核实]** = 所查来源均未记载，需对照已安装的 OVITO 3.16.1 GUI 验证。参数顺序按手册参数节的文档顺序；GUI 实际行序可能有细微差别（整体待核实）。

## 参数表（OVITO 面板顺序）

| # | 参数标签（原文） | 控件类型 | 默认值 | 有效范围 | 可见性条件 |
|---|---|---|---|---|---|
| 1 | Cartesian coordinates / Miller indices | 单选按钮（radio） | Cartesian [待核实] | 二选一 | 手册标注 Miller indices（倒易空间/晶面指数）模式为 Pro 功能 [待核实]；Basic 模式下可能隐藏 |
| 2 | Distance | 浮点字段（带关键帧动画 "A" 按钮） | 0.0 [待核实] | 有符号值（负数合法） | Miller 模式下距离改用面间距 d_hkl 的倍数表示 |
| 3 | Normal（X/Y/Z 三分量） | 向量字段（3 个浮点；分量旁的蓝色轴标签可将该分量重置为单位轴） | (0, 0, 1) [待核实] | 任意长度向量（不必为单位向量）[手册] | — |
| 4 | Slab width | 浮点字段 | 0（手册原文 "If this value is zero (the default)…"）[手册] | ≥ 0，单位为模拟长度单位 | — |
| 5 | Reverse Orientation | 复选框 | 关 [待核实]（Python 属性名 `inverse`，见 PyRef 侧栏） | — | — |
| 6 | Create selection (do not delete) | 复选框 | 关 [待核实] | — | — |
| 7 | Apply to selection only | 复选框 | 关 [待核实] | — | — |
| 8 | Visualize plane | 复选框 | 关 [待核实] | — | 勾选后生成可渲染的平面多边形几何；未勾选时平面只在交互视口中以线框指示 [手册] |
| 9 | Operate on | 多选列表（可按类限定到具体对象） | 全部启用 [待核实] | 类别：Particles / Surfaces / Voxel grids / Dislocations / Lines / Vectors [手册] | 同类对象多于一个时可限定到单个对象 |
| 10 | 对齐辅助按钮组 | 按钮（button） | — | — | 标签为 "Center in simulation cell" / "Align view to plane" / "Align plane to view" / "Pick three points" [待核实：精确文案与顺序] |

## 行为语义

- **计算内容**：沿一个无限切割平面删除（或选中）平面一侧的全部元素 [手册]。
- **平面定义**：由法向矢量 `Normal`（无需归一化）与**带符号**距离 `Distance` 定义——"the (signed) distance of the cutting plane from the origin measured parallel to the plane normal" [手册]。Cartesian 模式下原点为全局笛卡尔 (0,0,0)；Miller 模式下距离从模拟胞原点起、以面间距 d_hkl 计 [手册]。Miller 指数参照周期性晶胞矢量，"not the physical lattice possibly formed by atoms/particles within the simulation cell" [手册]。
- **Slab width 语义**：0 时删除平面一侧的所有内容；正值时切出给定厚度的切片 [手册]。切片区间的具体几何（从平面起算单侧 [d, d+w]，还是以平面为中心对称）**待核实**。
- **Reverse Orientation**：翻转切割平面的朝向 [手册]。slab width 为 0 时勾选它改为删除另一侧；slab width > 0 时语义为"挖掉 slab"而非"取出 slab"（反向保留区）[手册语义，具体边界待核实]。
- **Create selection (do not delete)**：不删除元素，改为将元素置为选中 [手册]。选中侧与删除侧的对应关系（即"通过平面判据的一侧被选中"）**待核实**。发布/写入的属性为 `Selection`（粒子级 0/1）。
- **Apply to selection only**：仅作用于当前已选中的元素子集 [手册]。
- **体素网格（Voxel grids）**：对体积场数据提取平面截面（planar cross-section），而非删除 [手册]。
- **错误条件**：手册页未记载（如 Normal 为零向量的行为需在实现中防御；OVITO 行为待核实）。

## AtomX 实现要点（gotchas）

1. 平面由 **法向 + 带符号距离** 完全定义；法向不必单位化——判据应写成 `dot(n, x) - d` 与 0 比较（及 slab 区间），而不是先归一化再比较（归一化会同时缩放 d）。
2. `Reverse Orientation` 不是"反选"，而是翻转保留半空间/slab 的方向；与 `Create selection` 组合时同样生效。
3. `Slab width = 0` 是默认状态（单侧切除）；slab 模式与 reverse 组合的边界行为必须与 GUI 核实后再定型。
4. 每个数值字段（Distance、Normal、Slab width）都带关键帧动画按钮 "A"——AtomX 若做动画需保留等价入口。
5. `Visualize plane` 生成可渲染平面几何；关闭时仅在交互视口显示——两条渲染路径都要支持。
6. 作用范围是**按数据对象类**（Particles/Surfaces/Voxel grids/Dislocations/Lines/Vectors）多选，而不是仅粒子；实现时应将裁切抽象为"对每类对象求平面侧判据"。
7. Miller 指数模式在 OVITO 中为 Pro 功能（待核实）；AtomX 计划不做 Pro 门控，可全部开放，但文档需注明与 Basic 版面板差异。

## 验证状态

参数标签、语义、slab width 默认值 0 已由 [手册] 证实；Normal/Distance 默认值、Reverse Orientation 与 Create selection 默认状态、面板精确行序、Miller 模式 Pro 标记 —— **待核实（对照安装版 OVITO 3.16.1 GUI）**。
