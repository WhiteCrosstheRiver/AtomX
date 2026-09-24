# M05 — Edit simulation cell（编辑模拟胞修饰器）

- OVITO 官方文档: <https://docs.ovito.org/reference/pipelines/modifiers/edit_simulation_cell.html>
- 分类: Modification（离线索引原文: "Sets boundary conditions or changes the cell geometry."）
- 授权层级: OVITO Basic（无 Pro 标记）
- 引入版本: 3.15.0 [手册]
- Python API: `ovito.modifiers.EditSimulationCellModifier`（属性: `cell_matrix`, `pbc_x`, `pbc_y`, `pbc_z`, `is2D`, `replace_cell` [PyRef]）

来源标记：**[手册]** = 在线 3.16.1 手册页（2026-09-25 抓取）；**[离线]** = 离线 HTML 索引；**[待核实]** = 来源未记载，需对照 OVITO 3.16.1 GUI。

## 参数表（OVITO 面板顺序，按手册 Modifier settings 节）

| # | 参数标签（原文） | 控件类型 | 默认值 | 有效范围 | 可见性条件 |
|---|---|---|---|---|---|
| 1 | Dimensionality | 组合框/选择器（2D / 3D） | 3D [待核实] | 二选一 | 选 2D 时 Z 方向周期性"automatically disabled and cannot be toggled" [手册] |
| 2 | Periodic boundary conditions — X | 复选框 | 跟随输入数据（不覆盖则保持原 PBC）[待核实：GUI 初始勾选态] | — | 2D 模式下 Z 项禁用 [手册] |
| 3 | Periodic boundary conditions — Y | 复选框 | 同上 [待核实] | — | 同上 |
| 4 | Periodic boundary conditions — Z | 复选框 | 同上 [待核实] | — | 2D 模式下禁用 [手册] |
| 5 | Edit cell geometry | 分组框启用开关（group box enable） | 关 [待核实] | — | 启用后下述矢量/原点输入生效；"the modifier will override the input cell geometry with the specified cell vectors and origin" [手册] |
| 6 | Cell vectors（a、b、c 三个矢量） | 3×3 矩阵表（每矢量 3 个笛卡尔分量 X/Y/Z） | 跟随输入晶胞 [待核实] | 任意实数；需线性无关才能构成有效晶胞 [待核实：OVITO 是否校验行列式] | 仅当 Edit cell geometry 启用 [手册] |
| 7 | Cell origin（o） | 向量字段（X/Y/Z） | 跟随输入晶胞 [待核实] | 任意实数 | 仅当 Edit cell geometry 启用 [手册] |

注：手册明确本页**不存在**以下选项（任务书中提到的候选，均被排除）："Adjust to box size of reference cell"、"Adjust lattice vectors"、"连同粒子一起变换晶胞"（粒子随胞变换要用 Affine transformation 或 Replicate 完成）[手册 "See also"]。

## 行为语义

- **计算内容**：直接改写模拟胞——周期边界条件标志、2D/3D 维度、以及（可选的）晶胞矢量与原点 [手册]。
- **仅改 PBC/维度**：Edit cell geometry 关闭时，修饰器只改维度与周期性，不动几何 [手册]。
- **Cell vectors/origin 语义**："The three cell vectors a, b, and c define the parallelepiped shape of the simulation cell"（各以笛卡尔 X/Y/Z 分量给出）；"The origin o specifies the position of one corner of the simulation cell in 3d space"，即晶胞矩阵的平移分量 [手册]。
- **管道位置语义**：改动"apply only at the position in the pipeline where the modifier is inserted"——只有该修饰器下游的阶段看到新晶胞，上游不变 [手册]。
- **不移动粒子**：本修饰器不改粒子坐标（这是与 Affine transformation 的关键区别）。
- **发布对象**：更新 `SimulationCell` 数据对象（cell matrix 3×4：3 列矢量 + 原点，加 3 个 PBC 标志）。
- **错误条件**：手册未记载。
- **配套 UI**：数据检查器的 Simulation Cell 页提供 "Edit in pipeline…" 按钮，一键把本修饰器插入管道 [手册]。

## AtomX 实现要点（gotchas）

1. 三个 PBC 是**每个方向独立**的布尔；2D 模式强制 Z 非周期且控件置灰——AtomX 的 2D 支持要复刻这个联动。
2. 晶胞几何覆盖是一个**启用开关 + 矩阵表 + 原点向量**的组合；关闭开关时仅 PBC/维度生效。AtomX 不要把 PBC 和几何覆盖绑成一个开关。
3. Python 属性 `replace_cell` 提示存在"替换 vs 修改"晶胞矩阵的两种模式 [PyRef 仅见属性名，语义待核实]。
4. 矩阵输入应提供与 OVITO 一致的 3 行矢量（每行一个晶胞矢量）布局，而不是 3×3 转置混淆；行列式为 0（退化晶胞）应报错。
5. 由于粒子不动，缩小晶胞会让粒子落到胞外；配合下游 Wrap at periodic boundaries 是标准工作流——文档/教程要写清。
6. 插入位置影响下游所有阶段的 PBC 判定（邻域搜索、wrap 等），AtomX 管道求值必须遵守同样的位置语义。

## 验证状态

参数集、2D-PBC 联动、覆盖语义、管道位置语义已由 [手册] 证实；各控件默认值（Dimensionality 初始 3D、PBC 初始勾选态、Edit cell geometry 初始关）、矢量/原点字段是否带动画按钮 —— **待核实（对照安装版 OVITO 3.16.1 GUI）**。
