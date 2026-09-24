# A09 — Displacement vectors（位移矢量修饰器）

- OVITO 官方文档: <https://docs.ovito.org/reference/pipelines/modifiers/displacement_vectors.html>
- 分类: Analysis（离线索引原文: "Calculates the displacements of particles based on an initial and a deformed configuration."）
- 授权层级: OVITO Basic（无 Pro 标记）
- Python API: `ovito.modifiers.CalculateDisplacementsModifier`（继承 `ReferenceConfigurationModifier`；`vis` 为 VectorVis 元素 [PyRef]）

来源标记：**[手册]** = 在线 3.16.1 手册页（2026-09-25 抓取）；**[PyRef]** = Python 参考页；**[离线]** = 离线 HTML 索引；**[待核实]** = 来源未记载，需对照 OVITO 3.16.1 GUI。

## 参数表（OVITO 面板顺序，按手册参数节文档顺序）

| # | 参数标签（原文） | 控件类型 | 默认值 | 有效范围 | 可见性条件 |
|---|---|---|---|---|---|
| 1 | Reference configuration（参考构型来源） | 组合框（三选项） | Constant reference configuration（上游管道、frame 0）[手册] | Constant reference configuration（上游管道指定帧）/ Relative to current frame（滑动参考）/ External file（外部文件）[手册] | 选 External file 时追加 "Reference: External file" 子面板选择初始位置文件 [手册] |
| 2 | Reference frame（常量模式的帧号输入） | 整数字段 [待核实控件] | 0（"frame 0 of the loaded animation sequence"，可改选其他帧）[手册] | 0 .. 最大帧 | 仅 Constant reference configuration 模式 [待核实] |
| 3 | Frame offset（相对模式的时间偏移） | 整数字段 | 待核实（手册示例 -1） | 负值 = 参考帧在当前帧之前 [手册] | 仅 Relative to current frame 模式 [待核实] |
| 4 | Affine mapping | 组合框（三选项） | **Off** [手册] | Off / To reference / To current [手册] | 面向周期体系（含晶胞形变）时才有意义 |
| 5 | Use minimum image convention | 复选框 | 待核实 | — | 对非周期方向 "never used"（自动不适用）[手册] |
| 6 | （标识匹配，Python `use_particle_identifiers`） | 复选框 [待核实 GUI 形态] | 待核实 | — | 存在 `Particle Identifier` 属性时按 ID 一一对应；否则要求两构型粒子数相等且存储顺序一致 [手册] |

## 行为语义

- **计算内容**：位移矢量 = 当前位置 − 参考位置（"computed by subtracting its reference position from its current position"）[手册]。
- **三种参考构型来源**：
  - **Constant reference configuration**：默认取加载动画序列的第 0 帧（上游管道求值），可另选帧 [手册]。
  - **Relative to current frame**：滑动参考——按相对当前帧的时间偏移取参考（offset = -1 即前一帧）；偏移为负时**第 0 帧计算失败**（无更早帧）[手册]。
  - **External file**：从单独的数据文件读初始粒子位置（子面板选文件）[手册]。
- **Affine mapping（周期体系的宏观形变处理）**：
  - **Off（默认）**："displacements are calculated simply by subtracting the initial particle position from the current position"，忽略晶胞几何变化 [手册]。
  - **To reference**：把当前粒子位置重映射进**参考晶胞**再相减，滤除宏观（仿射）形变、只留内部非仿射位移 [手册]。
  - **To current**：把参考构型粒子变换到**当前晶胞**后再相减 [手册]。
- **Use minimum image convention**：坐标在周期边界被 wrap 过时，按最小镜像约定修正位移；轨迹是**非包裹（unwrapped）坐标时应关闭**。位移超过半个盒子尺寸时最小镜像**无法正确表达**（需 unwrapped 坐标）；非周期方向上永不使用 [手册]。
- **粒子对应关系**：有 `Particle Identifier` 属性时按 ID 建立一一映射（容忍重排序）；无 ID 时要求两构型粒子数相等且**存储顺序相同**，否则位移会错乱（LAMMPS 类格式会重排原子——用数据检查器查 Particle Identifier）[手册]。
- **发布属性**：
  - `Displacement`（XYZ 三分量矢量属性；手册写作 "Displacement (XYZ)"）——可配箭头图形（Vectors 可视元素）[手册]。
  - `Displacement Magnitude`（标量属性）——配合 Color coding 使用 [手册]。
- **可视元素**：修饰器附带矢量显示（Vectors visual element / vector display）以箭头 glyph 渲染；其面板参数（箭头缩放、反转等）不在本页，**待核实**（见 visual elements 文档）。
- **错误条件**：相对模式负偏移在第 0 帧失败 [手册]；缺 ID 且存储顺序变化 → 结果静默错误（需检查器诊断）[手册]；位移 > 半盒尺寸时 MIC 失效 [手册]。

## AtomX 实现要点（gotchas）

1. **属性名精确匹配**：矢量属性 `Displacement`（3 分量）+ 标量 `Displacement Magnitude`。下游 Color coding / Expression selection 将按名引用。
2. 三种参考来源都要做：上游帧（含指定帧号）、相对偏移（含负偏移）、外部文件。相对模式的第 0 帧要产生**明确的错误状态**（OVITO 行为），不是静默置零。
3. **Affine mapping 是三态组合框而非复选框**；"Use periodic boundary conditions" 的展开需求实为两个控件：Affine mapping（形变滤除）与 minimum image convention（wrap 修正）——不要合并成一个 PBC 开关。
4. MIC 默认值需 GUI 核实后对齐；对 unwrapped 轨迹要建议用户关闭。
5. ID 匹配：有 `Particle Identifier` 必须走 ID 映射（哈希表）；无 ID 时校验两构型粒子数一致并在顺序可疑时告警。
6. 位移箭头是**独立的可视元素**（可单独开关/缩放），挂在修饰器输出上——AtomX 应把"计算"与"显示"分离成两个对象，与 OVITO 的 modifier + visual element 结构对齐。
7. 本修饰器与 Atomic strain / Wigner-Seitz 共用 ReferenceConfigurationModifier 基类语义（同样的参考构型来源/仿射映射/MIC 面板）——实现一套参考构型框架可复用到 A 序列其他分析修饰器。

## 验证状态

三种参考来源、affine mapping 三态与 Off 默认、MIC 语义与限制、输出属性名、ID 映射规则、错误条件已由 [手册] 证实；Frame offset 默认值、MIC 默认勾选态、use_particle_identifiers 的 GUI 形态与默认、Vectors 可视元素面板参数、面板行序 —— **待核实（对照安装版 OVITO 3.16.1 GUI）**。
