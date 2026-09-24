# Q07 — Select type（按类型选择修饰器）

- OVITO 官方文档: <https://docs.ovito.org/reference/pipelines/modifiers/select_particle_type.html>
- 分类: Selection（离线索引原文: "Selects all elements of a particular type, e.g. all atoms of a chemical species."）
- 授权层级: OVITO Basic（无 Pro 标记）
- Python API: `ovito.modifiers.SelectTypeModifier`

来源标记：**[手册]** = 在线 3.16.1 手册页（2026-09-25 抓取）；**[离线]** = 离线 HTML 索引；**[待核实]** = 来源未记载，需对照 OVITO 3.16.1 GUI。

## 参数表（OVITO 面板顺序，按手册参数节文档顺序）

| # | 参数标签（原文） | 控件类型 | 默认值 | 有效范围 | 可见性条件 |
|---|---|---|---|---|---|
| 1 | Operate on | 组合框（元素类别选择） | Particles [待核实] | Particles / Bonds 等元素类："Selects the class of elements (particles, bonds, etc.) the modifier should operate on." [手册] | 切换类别后 Property/Types 列表随之刷新 [待核实] |
| 2 | Property | 组合框（typed 属性选择） | Particle Type [待核实] | "The type property to be used as a source for the selection."——下拉"lists all available properties that store type information"，如粒子的 Particle Type、Structure Type，键的 Bond Type [手册] | 数据中无 typed 属性时列表为空 [待核实：空态行为] |
| 3 | Types | 复选框列表（当前源属性定义的全部类型） | 全不勾选 [待核实] | "Check one or more of them to let the modifier select all matching data elements." [手册] | 列表内容随 Property 选择联动 [手册] |

## 行为语义

- **计算内容**："selects particles, bonds, and other data elements on the basis of a type property"，即选中类型属性取值匹配所勾选类型的全部元素 [手册]。
- **多类型选择**：复选框列表天然支持多选——多个勾选项之间是**并集**（OR）关系 [手册]。
- **发布属性**：写入所选元素类的 `Selection` 属性（页面本身未直述输出属性名，按 OVITO 选择类修饰器惯例为 `Selection`；待核实）。
- **覆盖还是扩展**：手册未记载。按 OVITO 惯例应为覆盖式重写 Selection（与 Expression selection 相同）；待核实。
- **错误条件**：手册未记载。

## AtomX 实现要点（gotchas）

1. 三段式联动面板：**元素类别 → typed 属性 → 类型复选列表**。前两级的候选项完全由当前管道数据动态决定——AtomX 需要在修饰器面板每次激活时按上游数据刷新（OVITO 的 modifier 面板就是随求值结果刷新的）。
2. 不要把 Property 写死为 Particle Type：Structure Type、Bond Type（乃至任何 typed property）都要可选。候选判定标准 = "属性带类型信息"（类型列表非空的枚举型属性）。
3. 多选语义是并集；未勾选任何类型时结果为空选择（非错误）——建议与 OVITO 核实空态行为后对齐。
4. **类型改名/重编号的稳定性**（AtomX 计划项）：选择结果应绑定到类型对象（ID/名称）而不是列表索引，上游 Rename/Delete type 后勾选状态不漂移。OVITO 面板按名称勾选；实现上以类型 ID 为准、名称做展示。
5. 该修饰器是"类型 → Selection"的写入口，与下游 Color by type（读类型上色）、Expression selection 的 `ParticleType == "Cu"`（按名比较）互补；类型名比较语义要在三处保持一致（大小写敏感）。

## 验证状态

三参数的语义、多选并集、候选属性判定已由 [手册] 证实；全部默认值、覆盖/扩展语义、空态行为、面板行序 —— **待核实（对照安装版 OVITO 3.16.1 GUI）**。
