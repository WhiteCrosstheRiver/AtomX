# OVITO 3.16.1 修饰器参数参考表（9 张）

本目录为 AtomX 对标 OVITO 3.16.1 的逐修饰器参数参考表（Modifier Parity Reference Sheets）。

## 信息来源与可信度

| 标记 | 来源 | 说明 |
|---|---|---|
| [离线] | `vendor/ovito-reference/sources/modifiers.html`（2026-09-22 离线快照） | **仅包含修饰器索引页**（slug、分类、一句话描述、Basic/Pro 标记），无逐修饰器详情页 |
| [手册] | docs.ovito.org 在线 3.16.1 手册页（2026-09-25 抓取，与离线快照同版本） | 参数标签、语义、可见性条件的逐字依据 |
| [PyRef] | docs.ovito.org Python 参考（`ovito.modifiers.*`） | 程序化默认值（如 Centrosymmetry 12/Conventional、Affine 单位矩阵） |
| [待核实] | — | 所查来源均未记载；**需对照已安装的 OVITO 3.16.1 GUI 逐项核实**（多为默认值与面板行序） |

离线包只有索引页，因此每张表的参数细节以同版本在线手册补齐；未在任何来源出现的字段一律标注 [待核实]，未杜撰默认值。

## 参考表清单

| ID | 修饰器 | 文件 | 状态 |
|---|---|---|---|
| M12 | Slice | [M12-slice.md](M12-slice.md) | 基本完整；默认值/面板行序等 6 项**待 GUI 核实** |
| M11 | Replicate | [M11-replicate.md](M11-replicate.md) | 基本完整；Na/Nb/Nc 与 unique_ids 默认值**待 GUI 核实** |
| M05 | Edit simulation cell | [M05-edit-simulation-cell.md](M05-edit-simulation-cell.md) | 基本完整；各控件默认值**待 GUI 核实** |
| M01 | Affine transformation | [M01-affine-transformation.md](M01-affine-transformation.md) | 基本完整；模式切换控件标签/行序**待 GUI 核实** |
| Q02 | Expand selection | [Q02-expand-selection.md](Q02-expand-selection.md) | 语义完整；**全部默认值待 GUI 核实**（mode/cutoff/N/iterations） |
| Q03 | Expression selection | [Q03-expression-selection.md](Q03-expression-selection.md) | 语法/语义完整；默认表达式与 Operate on 标签**待 GUI 核实** |
| Q07 | Select type | [Q07-select-type.md](Q07-select-type.md) | 语义完整；**全部默认值与覆盖/扩展语义待 GUI 核实** |
| S02 | Centrosymmetry parameter | [S02-centrosymmetry.md](S02-centrosymmetry.md) | **最完整**（默认值 12/Conventional/False 已由 PyRef 证实）；仅控件形态与边角行为待核实 |
| A09 | Displacement vectors | [A09-displacement-vectors.md](A09-displacement-vectors.md) | 语义完整；Frame offset/MIC 默认值与 Vectors 面板**待 GUI 核实** |

## GUI 核实清单（汇总）

各表内所有 [待核实] 项的并集，按优先级：

1. **默认值**：Slice 的 Normal=(0,0,1)/Distance=0、Replicate 的 Na/Nb/Nc=1 与 Assign unique IDs、Expand selection 的 mode=Cutoff/cutoff/N/iterations=1、Expression 的初始表达式、Select type 的默认属性、Displacement 的 Frame offset 与 MIC 勾选态。
2. **面板行序与控件形态**：各表的参数行序按手册参数节文档顺序整理，与 GUI 实际排布可能有出入；Slice 的对齐按钮组文案、Affine 的模式切换控件、Centrosymmetry 的算法选择形态需逐一对齐。
3. **语义边角**：Slice 的 slab 区间几何（单侧 [d,d+w] vs 平面对称）与 Create selection 的选中侧；Select type 的覆盖 vs 扩展；Expression 是否存在手册未载的增量选择模式；Centrosymmetry 邻居数不足时的行为；Replicate 是否允许复制数为 0。
4. **Pro 门控**：Slice 的 Miller indices 模式在线手册摘要中标注为 Pro 功能，需在 Basic 版 GUI 确认其可见性（AtomX 按计划不设 Pro 门控）。
