# Q03 — Expression selection（表达式选择修饰器）

- OVITO 官方文档: <https://docs.ovito.org/reference/pipelines/modifiers/expression_select.html>
- 分类: Selection（离线索引原文: "Selects particles and other elements based on a user-defined criterion."）
- 授权层级: OVITO Basic（无 Pro 标记）
- Python API: `ovito.modifiers.ExpressionSelectionModifier`（属性: `expression`, `operate_on` [PyRef 侧栏]）

来源标记：**[手册]** = 在线 3.16.1 手册页（2026-09-25 抓取）；**[离线]** = 离线 HTML 索引；**[待核实]** = 来源未记载，需对照 OVITO 3.16.1 GUI。

## 参数表（OVITO 面板顺序）

| # | 参数标签（原文） | 控件类型 | 默认值 | 有效范围 | 可见性条件 |
|---|---|---|---|---|---|
| 1 | Boolean expression | 表达式文本编辑框 | 待核实（新插入时建议为空或恒 false 的占位） | 任意布尔表达式；语法类 C [手册] | — |
| 2 | Operate on（作用对象：粒子 / 键） | 组合框（Particles / Bonds）[待核实精确标签] | Particles [待核实] | — | 选 Bonds 时表达式变量切换为键属性（含 `@1.`/`@2.` 前缀的原子属性）[手册] |
| 3 | （下方面板）输入变量列表 | 只读列表（帮助区） | — | — | "The lower panel of the modifier's user interface displays a list of input variables that can be included in the expression." [手册] |

**注意**：手册**未记载** Set / Add / Remove 选择模式。手册语义是覆盖式：表达式为真（非零）的元素被选中，其余 "are deselected" [手册]。若 GUI 另有增量模式，属手册未记载内容 —— 待核实。

## 行为语义

- **计算内容**：对每个输入元素求值布尔表达式，"selecting those for which the Boolean expression returns a non-zero result (true)"；表达式为 0/false 的元素被取消选中 [手册]。
- **发布属性**：写入所选元素类的 `Selection` 属性（粒子或键）。
- **可用变量**（手册示例级）：
  - 粒子属性：`Position.X/.Y/.Z`, `ReducedPosition.X/.Y/.Z`, `ParticleType`, `StructureType`, `Charge`, `Mass`, `AtomName`, `Selection` 及自定义属性（如 `MyStringProperty`）。
  - 全局量：如 `CellSize.X/.Y/.Z`、当前时间步（timestep number）等 "global quantities" [手册]。
  - 键模式：键属性（`BondLength` 等）+ 两端原子属性 `@1.ParticleType`、`@2.…`（键方向任意，必要时两种顺序都要写：`"@1.ParticleType != @2.ParticleType && BondLength > 2.8"`）[手册]。
- **变量名规则**："Spaces in property names are simply left out in the corresponding variable names"（"Particle Type" → `ParticleType`）；其他非法字符替换为下划线；解析器**区分大小写**，变量名必须与列出的完全一致 [手册]。
- **运算符与优先级**（由高到低）：`( )` 分组 → `^`（幂）→ `*` `/` → `+` `-` → 比较 `==` `!=` `<` `<=` `>` `>=`（返回 0/1）→ `&&` → `||` → 三元 `A ? B : C` [手册]。
- **函数集**：`abs, acos, acosh, asin, asinh, atan, atan2(Y,X), atanh, avg, cos, cosh, exp, fmod, rint, ln, log10, log2, max, min, sign, sin, sinh, sqrt, sum, tan`；常量 `pi`、`inf` [手册]。
- **类型按名引用**（3.12.0+）：`ParticleType == "Cu"`。重名类型自动展开为多个 ID 的 OR；不存在的类型名求值为 `inf`（恒 false）；类型名区分大小写 [手册]。
- **字符串属性**（3.16.0+）：仅支持 `==`、`!=`、`=~`；"cannot be used in arithmetic expressions or with ordered comparison operators (<, >, <=, >=)"，比较区分大小写 [手册]。
- **正则匹配**（3.16.0+）：Perl 风格 `/pattern/`，如 `AtomName =~ /^O/`；`i` 修饰符忽略大小写（`=~ /^o/i`）；`^`/`$` 锚定完整匹配 [手册]。
- **错误条件**：语法错误由解析器报错（位置信息）；同名类型在多个 typed property 间歧义时解析器 "may raise an error"；不存在的类型名→`inf` 恒 false（非错误）[手册]。

## AtomX 实现要点（gotchas）

1. **覆盖语义**：每次求值重写整个 Selection——表达式为 false 的元素明确置 0。想做"增量选择"需要在表达式中引用 `Selection`（如 `Selection && Charge > 0`），这与手册示例一致；不要发明隐藏的 Add/Remove 模式。
2. 表达式语言是 C 风格子集 + 三元运算符；`^` 是幂而不是异或。实现求值器（或嵌入类 ExprEval/muparser）时优先级表必须逐条对齐。
3. 属性名→变量名的映射规则（去空格、非法字符→下划线、大小写敏感）要在属性导入时就确定，保证所有修饰器一致。
4. 类型名匹配要支持：数字 ID 比较、双引号名称比较（重名展开 OR、未知名→inf）、大小写敏感三个语义。
5. 键模式的 `@1.`/`@2.` 语法 + "两种顺序都写"是用户侧惯例——求值器只需提供前缀取值，方向歧义留给表达式。
6. 3.16.0 的字符串/正则特性是本版本新增，AtomX 若分期实现可将其列为后期项，但 `==`/`!=` 字符串比较应与首版一起交付。
7. 错误报告要带字符位置（解析器错误）与变量名提示；未知变量、类型歧义是两类不同的错误。

## 验证状态

表达式语法、变量、函数、类型名/字符串/正则语义、覆盖式选择语义已由 [手册] 证实；默认表达式、Operate on 精确标签与默认项、GUI 是否存在手册未载的增量模式 —— **待核实（对照安装版 OVITO 3.16.1 GUI）**。
