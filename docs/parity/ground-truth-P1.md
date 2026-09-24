# P1 波次 OVITO 真值记录（2026-09-25，live GUI 核实）

数据集：`codes\test.xyz`（30 帧，108 原子 FCC Cu/Ni，a=3.6，盒子 10.8³，帧 0）
环境：OVITO Basic 3.16.1，Windows，4K 全屏，默认浅色主题。截图存 `docs/parity/ovito/`。

## 全局性质：修饰器栈嵌套共存（用户强调的最高优先级验收项）

OVITO 的 Add modification 功能是**可组合流水线栈**，已在本会话实测：
- 多个修饰器同时存在于 Modifications 树中（本次会话同时挂了 Edit simulation cell + Slice + Centrosymmetry），按顺序串联，每个消费上一个的输出。
- 参数互不串扰：每个节点面板只编辑选中节点自己的参数；上游改参只使下游重算。
- **AtomX 验收**：Replicate→Slice（216 切半 = 108）≠ Slice→Replicate（108 复制 = 216 后切？注意顺序语义不同）；EditCell(×2)+分数重映射→Slice 的计数必须反映新晶胞；双 Slice 节点各自参数独立。对应引擎测试已加入 P1 要求。

## M12 Slice

面板控件（自上而下，与截图一致）：
1. `Normal` — 3 个浮点字段一行（默认 0.00 0.00 1.00）
2. `Distance` — 浮点 + 步进（新建时默认 = 晶胞沿法向中点，本例 5.4000）
3. `Invert slice` — 复选框（默认关）
4. `Slice width` — 浮点（默认 0.0000）
5. `Show slice plane in viewports` — 复选框（**默认开**；开启时视口绘制半透明灰色平面四边形，范围贴合晶胞）

语义（实验核实）：
- 平面定义 `n·r = distance`；`width=0` 保留 `n·r <= d` 一侧；`invert` 取反一侧。
- `width=w>0` 保留**以平面为中心的对称区间** `[d - w/2, d + w/2]`。
  实验：d=0.9, w=1.8, 不反转 → 状态栏 `Slice: 36`（= z∈[0,1.8] 两层×18；
  若为单侧区间 [0.9,2.7] 则只有 18 个）。截图 `m12-slice-width-probe*`。
- 改参 `Distance 3.0 + Invert` 后保留远侧原子（视口验证）。

截图：`m12-slice-default{,-panel}.png`、`m12-slice-tweaked{,-panel}.png`、`m12-slice-width-probe{,-panel,-count}.png`

## M11 Replicate

面板控件：
1. `Number of copies` 组：`Na` `Nb` `Nc` 三个整数字段（默认 1/1/1）
2. `Adjust box size` — 复选框（**默认开**；开启时晶胞向量按 N 缩放）

语义：Na=2 → 216 粒子、晶胞 X 向量 21.6（截图 `m11-replicate-na2*` 状态栏与视口验证）。
R1 参考单补充：唯一 ID 重排、`Periodic Image` 展开重卷绕（3.10.1+）。

截图：`m11-replicate-default{,-panel,-panel-zoom}.png`、`m11-replicate-na2{,-panel}.png`

## M05 Edit simulation cell

面板控件（自上而下）：
1. `Cell origin` — 3 浮点一行（默认 0 0 0）
2. `Cell vectors` — 3×3 矩阵（默认对角 10.8）
3. `Periodic boundary flags` — 3 复选框 x/y/z（默认 ☑☑☑）

注意：Basic 3.16 面板**没有**快捷尺寸字段；顺序是 origin 在前、矩阵在后。
AtomX 现有面板顺序为矩阵在前，需对齐。

截图：`m05-editcell-default{,-panel}.png`

## 复现命令要点

桌面自动化：`codes\_gui_auto\{activate,click,key,shot,crop}.ps1`。
Add modification 按钮位于 (3539,207)（4K 全屏）；菜单搜索框中心 (2550,418)；
过滤后 Modification 列行高约 33px，首行 y≈470。
