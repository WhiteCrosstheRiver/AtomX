# P1 波次 OVITO 真值记录（2026-09-25，live GUI 核实）

数据集：`codes\test.xyz`（30 帧，108 原子 FCC Cu/Ni，a=3.6，盒子 10.8³，帧 0）
环境：OVITO Basic 3.16.1，Windows，4K 全屏，默认浅色主题。截图存 `docs/parity/ovito/`。

## 全局性质：修饰器栈嵌套共存（用户强调的最高优先级验收项）

OVITO 的 Add modification 功能是**可组合流水线栈**，已在本会话实测：
- 多个修饰器同时存在于 Modifications 树中（本次会话同时挂了 Edit simulation cell + Slice + Centrosymmetry），按顺序串联，每个消费上一个的输出。
- 参数互不串扰：每个节点面板只编辑选中节点自己的参数；上游改参只使下游重算。
- **AtomX 验收**：Replicate→Slice（216 切半 = 108）≠ Slice→Replicate（108 复制 = 216 后切？注意顺序语义不同）；EditCell(×2)+分数重映射→Slice 的计数必须反映新晶胞；双 Slice 节点各自参数独立。对应引擎测试已加入 P1 要求。

## M12 Slice（2026-09-25 第二次核实，替换早前被污染会话的错误记录）

> 重要教训：强杀 OVITO 后它会恢复上次会话的流水线状态；且点击 "Add modification..."
> 按钮后搜索框已自动聚焦，**不要**再点击菜单内部（会误点中某一行把随机修饰器加进流水线）。
> 正确操作：点按钮 → 直接打字 → 点过滤/高亮行。干净会话截图 `v-*.png`。

面板控件（自上而下，见 `v-slice-added.png` 全图）：
1. 模式单选：`Cartesian`（默认）/ `Miller indices (OVITO Pro)`（Basic 灰显）
2. `Distance:` 浮点，默认 = 晶胞沿法向中点（10.8 盒子 → 5.4）
3. `Normal (x):` `Normal (y):` `Normal (z):` 三个带标签浮点字段，**默认 (1,0,0)，X 轴**
4. `Slab width:` 浮点（不是 "Slice width"），默认 0
5. `Reverse orientation` 复选框（不是 "Invert slice"），默认关
6. `Create selection (do not delete)` 复选框，默认关——开启时不删除，改为选中保留侧
7. `Apply to selection only` 复选框，默认关
8. `Visualize plane` 复选框，**默认关**
9. 按钮：`Center in simulation cell` / `Align view to plane` / `Align plane to view` / `Pick three points`
10. 结果文本框：`108 input particles / 44 particles deleted / 64 particles remaining`
11. `Operate on` 区：Particles ☑（<all>）+ Surfaces/Voxel grids/Dislocations/Lines/Vectors <not present>

语义（live 实测计数）：
- h = n·r；保留 **h < d 严格**（d=5.4, 法向 x → 64 remaining；热振动使 5.4 层原子被劈开，
  3.6 层以下全留 = 54 + 5.4 层低于 5.4 的 10 个 = 64）。
- `Reverse orientation` → h > d 严格。
- `Slab width w>0`：保留 **|h−d| ≤ w/2 闭区间、以平面为中心**（d=5.4, w=1.8 →
  **90 deleted / 18 remaining**，恰为整个 5.4±0.12 层；截图 `v-slice-slab-count.png`）。
  Reverse+slab → 严格在区间外。

截图：`v-clean-start.png`、`v-slice-menu2.png`、`v-slice-added.png`（面板全图）、
`v-slice-slab{,-crop,-count}.png`。早前的 `m12-slice-*` 系列截图来自被污染会话，仅存档勿引用。

## M11 Replicate（干净会话核实，替换早前作废记录）

面板控件（见 `v-repl-panel.png`）：
1. `Number of images:` **三个 spinner**（默认 1/1/1）——控件名是 images 不是 copies
2. `Adjust simulation box size` 复选框（默认开）
3. `Assign unique IDs` 复选框
4. `Operate on`：`Particles & bonds <all>` + 其余 <not present> 行

## M05 Edit simulation cell（干净会话核实，替换早前作废记录）

面板顺序（见 `v-editcell-panel.png`，**与 R1 文档版不同，以本图为准**）：
1. `Dimensionality`：2D / 3D 单选（默认 3D）
2. `Periodic boundary conditions`：X / Y / Z 三个复选框（默认全开）
3. `Cell geometry`：`Cell vectors:` 3×3 在前，`Cell origin:` 3 字段在**后**

## S02 Centrosymmetry parameter（干净会话核实）

面板（见 `v-csp-panel.png`）：
1. `Number of neighbors:` spinner，默认 12
2. 单选：`Conventional CSP`（默认）/ `Minimum-weight matching CSP`
3. `Only selected particles` 复选框
4. **实时 CSP 分布直方图**（橙色柱状，随参数即时刷新）——AtomX N1 的视觉验收目标

## M01 Affine transformation（干净会话核实）

面板标题 `Transformation`（见 `v-affine-panel.png`）：
1. `Transformation matrix:` → `Translate/Scale/Shear:` 3×3（默认单位阵）+ 右侧 `Enter rotation` 按钮
2. `Translation:` 3 字段 + `In reduced cell coordinates` 复选框
3. `Transform simulation cell:` → `Transform cell vectors:` 3×3 只读联动显示 + `Transform cell origin:` 3 只读字段
4. `Operate on`：`Transform only selected particles/vertices` + 各对象类 combo（Simulation cell <all>/Particles <all>/vector properties 等 <not present>）

## Q02 Expand selection（干净会话核实）

面板（见 `v-expand-panel.png`）：
- `Expansion mode`：二选一/四选一 radio 组："...within the range: cutoff distance: 3.2" /
  "...among the N nearest neighbors: N" / "...bonded to a selected particle." / "...of the same molecule."
- `Iteration settings`：`Number of iterations: 1`
- 无输入选区时红色错误框：`This operation requires an input particles selection.`
- 默认 cutoff 3.2

## Q03 Expression selection（干净会话核实）

面板（见 `v-expr-panel.png`）：
- `Operate on:` combo + **多行** `Boolean expression` 编辑器
- 黄色提示：`Enter a Boolean expression.`
- `Expression variables` 分节列出可用变量：ParticleType / Position.X,Y,Z /
  ParticleIndex (zero-based) / ReducedPosition.X,Y,Z / …

## Q07 Select type（干净会话核实）

面板（见 `v-seltype-panel.png`）：
- `Operate on:` combo + 属性 combo（Particle Type）
- **类型复选框列表**（Name | Id 列头，Id 1/2 各一行）——多选，不是单一 index 输入

## 复现命令要点

桌面自动化：`codes\_gui_auto\{activate,click,key,shot,crop}.ps1`。
Add modification 按钮位于 (3539,207)（4K 全屏）；菜单搜索框中心 (2550,418)；
过滤后 Modification 列行高约 33px，首行 y≈470。
