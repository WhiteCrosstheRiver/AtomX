# M01 — Affine transformation（仿射变换修饰器）

- OVITO 官方文档: <https://docs.ovito.org/reference/pipelines/modifiers/affine_transformation.html>
- 分类: Modification（离线索引原文: "Applies an affine transformation to the system."）
- 授权层级: OVITO Basic（无 Pro 标记）
- Python API: `ovito.modifiers.AffineTransformationModifier`

来源标记：**[手册]** = 在线 3.16.1 手册页（2026-09-25 抓取）；**[PyRef]** = Python 参考页；**[离线]** = 离线 HTML 索引；**[待核实]** = 来源未记载，需对照 OVITO 3.16.1 GUI。

## 参数表（OVITO 面板顺序，按手册参数节文档顺序）

| # | 参数标签（原文） | 控件类型 | 默认值 | 有效范围 | 可见性条件 |
|---|---|---|---|---|---|
| 1 | 变换指定方式：显式矩阵 / 目标晶胞模式 | 模式切换（radio/combo；精确标签**待核实**） | 手册区分两种方式：显式 "a 3-by-3 matrix plus a translation vector" 与 "Transform to target cell" 隐式计算 [手册]；PyRef `relative_mode` 默认 `True` [PyRef] | 二选一 | — |
| 2 | Transformation matrix M（3×3）+ Translation vector t（3 分量） | 矩阵表 + 向量字段 | 单位矩阵 + 零平移（PyRef: `transformation` = `[[1,0,0,0],[0,1,0,0],[0,0,1,0]]`）[PyRef] | 任意实数 | 仅显式矩阵模式可见 [待核实] |
| 3 | Enter rotation | 按钮（打开对话框：3D 旋转轴、旋转角、旋转中心） | — | — | 对话框确认后 OVITO 代算出对应仿射变换写入 M [手册] |
| 4 | In reduced cell coordinates | 复选框 | 关（PyRef `reduced_coords` = `False`）[PyRef] | — | 勾选后平移向量 t 按约化（分数）坐标解释，公式变为 x′ = M·(x + H·t)，H 为晶胞矩阵 [手册] |
| 5 | Transform to target cell（目标晶胞模式的参数组） | 目标晶胞矩阵输入组 [待核实控件细节] | 无默认（需指定目标形状；PyRef `target_cell` 无默认值）[PyRef] | — | 仅目标晶胞模式可见；修饰器 "dynamically computes the affine transformation… from the current shape of the simulation cell and the given target shape" [手册]，除非某类元素的变换被关闭 |
| 6 | Transform only selected particles/vertices | 复选框 | 关（PyRef `only_selected` = `False`）[PyRef] | — | 仅作用于当前选中的粒子或网格/线对象顶点 [手册] |
| 7 | Operate on — 元素类别开关 | 多选列表（9 类，同类多对象可限定到单个） | 全部 9 类启用（PyRef `operate_on` 默认集合）[PyRef] | 见下 | 某类输入不存在时对应项自然无效 [手册] |

Operate on 类别清单（9 项，[手册] 逐项记载）：
**Simulation cell**（原点做完整变换，三个晶胞矢量只乘线性部分 M）/ **Particles**（Position 与 Orientation 属性）/ **Vector properties**（只施加线性部分 M 的矢量属性，如 Velocity、Force、Displacement——带 Vectors 可视元素、三分量的属性）/ **Voxel grids**（域形状）/ **Surfaces**（表面网格顶点）/ **Triangle meshes**（三角网格顶点）/ **Lines**（线对象顶点）/ **Vectors**（矢量对象的位置与方向）/ **Dislocations**（位错线与 Burgers 矢量）。

## 行为语义

- **计算内容**：对系统施加仿射变换 x′ = M·x + t（列矢量约定）[手册]。
- **目标晶胞模式**："the modifier dynamically computes the affine transformation to be applied to the system" —— 由当前晶胞形状与目标形状之差自动算出变换，把盒内内容映射到新形状（除非该类元素变换被关）。典型用途：把恒压 MD 的时变晶胞替换为固定形状 [手册]。
- **旋转输入**：按钮打开对话框输入旋转轴/角度/旋转中心，OVITO 计算相应仿射变换 [手册]。角度单位（度）与方向约定手册未写明 [待核实]。
- **发布对象**：不发布新分析属性；原地改写 Position/Orientation/矢量属性/各类几何。晶胞矢量只受线性部分 M 影响（平移不缩放盒矢量）。
- **错误条件**：手册未记载（M 奇异时的行为待核实）。

## AtomX 实现要点（gotchas）

1. **手册中不存在** "Rotate to fit" 这类旋转预设组合（任务书中的候选被否定）；显式矩阵 + "Enter rotation" 对话框 + 目标晶胞模式才是全部输入面。AtomX 不必造预设。
2. 区分**完整变换**（点类数据：Position、晶胞原点）与**线性部分**（方向类数据：Velocity/Force/Displacement、晶胞矢量、Burgers 矢量）——平移量绝不能加到矢量属性上。
3. `In reduced cell coordinates` 改变公式为 x′ = M·(x + H·t)：t 是作用后晶胞下的分数坐标平移。实现时注意 H 取变换后的晶胞矩阵。
4. 目标晶胞模式是"相对模式"（PyRef `relative_mode = True` 为默认）：从当前形状动态计算，因此可跟随时变输入；固定矩阵模式则是绝对的一次性变换。
5. `Transform only selected particles/vertices` 只约束粒子与网格/线顶点；晶胞等其他类别不受选择限制。
6. 9 个类别开关默认全开——AtomX 的默认面板应同样全开，且对缺失类别自动无操作而非报错。
7. Orientation（四元数）的变换需要用旋转部分构造四元数旋转，不能直接乘矩阵；手册只说 "and their orientations if present"。

## 验证状态

参数集、语义、9 类元素开关及其各自语义、reduced_coords/only_selected/默认单位矩阵已由 [手册]+[PyRef] 证实；模式切换控件精确标签、Enter rotation 对话框内字段细节、目标晶胞组控件、面板行序 —— **待核实（对照安装版 OVITO 3.16.1 GUI）**。
