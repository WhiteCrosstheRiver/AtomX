# S02 — Centrosymmetry parameter（中心对称参数修饰器）

- OVITO 官方文档: <https://docs.ovito.org/reference/pipelines/modifiers/centrosymmetry.html>
- 分类: Structure identification（离线索引原文: "Calculates the centrosymmetry parameter for every particle."）
- 授权层级: OVITO Basic（无 Pro 标记）
- Python API: `ovito.modifiers.CentroSymmetryModifier`

来源标记：**[手册]** = 在线 3.16.1 手册页（2026-09-25 抓取）；**[PyRef]** = Python 参考页（已确认默认值）；**[离线]** = 离线 HTML 索引；**[待核实]** = 来源未记载，需对照 OVITO 3.16.1 GUI。

## 参数表（OVITO 面板顺序，按手册参数节文档顺序）

| # | 参数标签（原文） | 控件类型 | 默认值 | 有效范围 | 可见性条件 |
|---|---|---|---|---|---|
| 1 | Number of neighbors | 整数字段 | **12**（PyRef: "Default: 12"） | "must be a positive, even integer"（正偶数）[手册] | — |
| 2 | 算法模式（两小节：Conventional CSP / Minimum-weight matching CSP） | 组合框或单选组（控件形态待核实） | **Conventional CSP**（PyRef: `mode` "Default: CentroSymmetryModifier.Mode.Conventional"） | 二选一 [手册] | — |
| 3 | Use only selected particles | 复选框 | **关**（PyRef: `only_selected` "Default: False"） | — | 有粒子选择输入时才有意义 |

## 行为语义

- **计算内容**：为每个粒子计算中心对称参数（CSP）。公式（Kelchner, Plimpton, Hamilton, PRB 58, 11085 (1998)）：
  `p_CSP = Σ_{i=1..N/2} |r_i + r_{i+N/2}|²`，对 N 个最近邻的相反矢量配对求和 [手册]。
- **Number of neighbors**："should match the ideal number of nearest neighbors in the crystal lattice at hand (12 in fcc crystals; 8 in bcc)" [手册]。必须与晶格理想配位数一致，否则物理意义错误。
- **Conventional CSP**："uses the same algorithm as LAMMPS"；对全部 N(N-1)/2 个邻居对计算权重 w = |r_i + r_j|²，贪心取 **N/2 个最低权重**求和 [手册]。
- **Minimum-weight matching CSP**：按 Larsen (arXiv:2003.08879) 的最小权重完美配对——"ensures that neighbor relationships are reciprocal"（配对互为最近邻），对 HCP 与表面原子等**非中心对称**结构区分度更好；代价是 "This algorithm is more computationally expensive" [手册]。
- **Use only selected particles**："restricts the analysis to the subset of currently selected particles only"；未选中粒子被忽略，"their own centrosymmetry values will be set to zero" [手册]。用于只分析某种原子的亚晶格。
- **发布属性**：逐粒子 `Centrosymmetry` 属性——"The calculated atomic CSP values are stored in the Centrosymmetry output particle property by the modifier" [手册]。
- **面板内嵌输出**：修饰器面板显示 CSP 值的直方图 [手册]。
- **错误条件**：手册未明确。已知约束：粒子近邻数不足 N 时（小体系/表面）行为未记载 [待核实]。
- **管道顺序要求**："The modifier needs to see the complete set of particles"——应放在任何删除粒子的修饰器**之前**（早期管道位置）[手册]。

## AtomX 实现要点（gotchas）

1. **邻居数是单一整数字段（默认 12），没有 FCC/BCC 预设下拉**——任务书中的 "matching mode" 实为算法二选一（Conventional vs Minimum-weight matching），不是邻居数预设。AtomX 可在帮助文案标注 12=FCC/HCP、8=BCC，但控件保持一个正偶数字段。
2. 正偶数约束要做输入校验（奇数、0、负数拒绝）。
3. 两种算法都要实现：Conventional（LAMMPS 等价，O(N²) 对排序取 N/2 最小）与最小权重匹配（更贵但区分度好）；默认 Conventional。
4. `only_selected` 打开时未选中粒子的 CSP **写 0**（不是保留旧值/NaN）——与 Color coding 的默认色阶范围会互相影响，注意 0 值的语义。
5. 输出属性名精确为 `Centrosymmetry`（无空格）；配套直方图建议做进修饰器面板（OVITO 如此），AtomX 数据检查器也应能看到该属性。
6. 参考下游工作流（手册建议）：Color coding 按 CSP 上色 → Expression selection 按阈值筛 → Delete selected 隐去完美晶格只显缺陷。
7. 近邻搜索要尊重 PBC；邻居数不足 N 的粒子（体系太小）建议显式告警而非静默。

## 验证状态

默认值三元组（12 / Conventional / only_selected=False）、正偶数约束、两算法语义、输出属性名、未选中置零、直方图、管道位置要求已由 [手册]+[PyRef] 证实；算法选择的控件形态、邻居不足时的行为、面板行序 —— **待核实（对照安装版 OVITO 3.16.1 GUI）**。
