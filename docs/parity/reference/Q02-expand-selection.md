# Q02 — Expand selection（扩展选择修饰器）

- OVITO 官方文档: <https://docs.ovito.org/reference/pipelines/modifiers/expand_selection.html>
- 分类: Selection（离线索引原文: "Selects particles that are neighbors of already selected particles."）
- 授权层级: OVITO Basic（无 Pro 标记）
- Python API: `ovito.modifiers.ExpandSelectionModifier`（属性: `mode`, `cutoff`, `num_neighbors`, `iterations` [PyRef 侧栏]）

来源标记：**[手册]** = 在线 3.16.1 手册页（2026-09-25 抓取）；**[离线]** = 离线 HTML 索引；**[待核实]** = 来源未记载，需对照 OVITO 3.16.1 GUI。

## 参数表（OVITO 面板顺序，按手册参数节文档顺序）

| # | 参数标签（原文） | 控件类型 | 默认值 | 有效范围 | 可见性条件 |
|---|---|---|---|---|---|
| 1 | Expansion mode（扩展模式选择） | 组合框（combo） | Cutoff [待核实] | 4 选 1: Cutoff / Nearest / Bonded / Molecule [手册] | — |
| 2 | Cutoff distance | 浮点字段（长度量纲） | 待核实 | > 0 | 仅 Cutoff 模式可见 [待核实] |
| 3 | N | 整数字段 | 待核实 | ≥ 1 | 仅 Nearest 模式可见 [待核实] |
| 4 | Number of iterations | 整数字段 | 1 [待核实] | ≥ 1 | 全模式可用 [待核实] |

注：手册把 Cutoff/Nearest/Bonded/Molecule 写成参数节的四个小节标题，即**同一模式选择器的四个选项**（组合框或单选组，控件形态待核实）。

## 行为语义

- **计算内容**：把已选粒子的邻居并入选择，扩展粒子 `Selection` 属性 [手册]。
- **Cutoff 模式**："A distance threshold can be specified to select all particles that are within range of an already selected particle." [手册]
- **Nearest 模式**："Selects those particles that are among the N nearest neighbors of an already selected particle. The number N is adjustable." 修饰器"sorts the neighbor list of already selected particles by ascending distance"并取前 N 项 [手册]。
- **Bonded 模式**："Extends the selection to particles connected by a bond to at least one already selected particle." [手册] 需要键（bonds）作为输入。
- **Molecule 模式**："Expands the selection to all particles that belong to the same molecule(s) as already selected particles." [手册] 需要分子归属信息（Molecule Identifier 属性）。
- **迭代语义**：Number of iterations 允许多步递归扩展——"This parameter allows you to expand the selection in multiple recursive steps"；设为 2 等价于把修饰器连用两次（扩到第二近邻壳层）[手册]。
- **发布属性**：写入粒子 `Selection`（0/1）。无新数据对象、无全局属性。
- **错误条件**：手册未记载（Bonded 模式无键、Molecule 模式无分子信息时的报错行为待核实）。

## AtomX 实现要点（gotchas）

1. 四种模式的输入依赖不同：Cutoff/Nearest 只需粒子坐标（邻域查找）；Bonded 需要键；Molecule 需要 Molecule Identifier。AtomX 应在模式切换时检查前置数据并禁用/提示。
2. Nearest 模式的排序判据是"与已选粒子的距离升序"，对每个已选粒子取前 N；多个已选粒子的前 N 并集去重。
3. 迭代 = 递归：第 k 轮在 k-1 轮结果之上再扩展；k=1 即单步。实现为循环即可，注意每轮邻域查询可缓存。
4. 邻域搜索应尊重周期边界（OVITO 的 Cutoff/Nearest 邻域查找天然走 PBC；手册本页未明说——待核实）。
5. 本修饰器**只作用于粒子**（手册通篇针对粒子选择）；不要把它泛化到键选择。
6. 无输入选择时（全未选）结果为空选择，属正常行为而非错误。

## 验证状态

四种模式语义、迭代语义、参数标签已由 [手册] 证实；全部默认值（mode=Cutoff、cutoff、N、iterations）与控件可见性联动 —— **待核实（对照安装版 OVITO 3.16.1 GUI）**。
