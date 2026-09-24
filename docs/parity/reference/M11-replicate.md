# M11 — Replicate（周期镜像复制修饰器）

- OVITO 官方文档: <https://docs.ovito.org/reference/pipelines/modifiers/show_periodic_images.html>
- 分类: Modification（离线索引原文: "Duplicates particles and other data elements to visualize periodic images of the system."）
- 授权层级: OVITO Basic（无 Pro 标记）
- Python API: `ovito.modifiers.ReplicateModifier`（属性侧栏可见 `num_x/num_y/num_z`, `adjust_box`, `unique_ids` [PyRef 侧栏]）

来源标记：**[手册]** = 在线 3.16.1 手册页（2026-09-25 抓取）；**[离线]** = 离线 HTML 索引；**[待核实]** = 来源未记载，需对照 OVITO 3.16.1 GUI。面板顺序按手册参数节文档顺序；GUI 实际行序待核实。

## 参数表（OVITO 面板顺序）

| # | 参数标签（原文） | 控件类型 | 默认值 | 有效范围 | 可见性条件 |
|---|---|---|---|---|---|
| 1 | Number of images - X（沿晶胞矢量 a 的复制数） | 整数微调框（integer spinner） | 1 [待核实] | ≥ 1 [待核实：是否允许 0] | — |
| 2 | Number of images - Y | 整数微调框 | 1 [待核实] | ≥ 1 [待核实] | — |
| 3 | Number of images - Z | 整数微调框 | 1 [待核实] | ≥ 1 [待核实] | — |
| 4 | Adjust simulation box size | 复选框 | **开**（手册: "By default, the modifier extends the simulation cell appropriately"）[手册] | — | — |
| 5 | Assign unique IDs | 复选框 | 待核实（建议按"开"实现） | — | — |
| 6 | Operate on（复制的对象类别列表） | 多选列表 | 待核实（默认粒子+键等全部） | 手册概述: 复制 "all particles, bonds, and other data elements" [手册] | 同类对象多于一个时可限定 |

## 行为语义

- **计算内容**："This modifier copies all particles, bonds, and other data elements multiple times to visualize periodic images of a system." [手册] 沿三个晶胞矢量方向各复制 (Na-1)(Nb-1)(Nc-1) 份镜像。
- **晶胞尺寸联动**：勾选 Adjust simulation box size 时，"the modifier extends the simulation cell appropriately" —— 新晶胞矢量 = 原矢量 × 对应方向复制数（a' = Na·a 等），使晶胞恰好包住所有镜像 [手册语义]。
- **关闭 Adjust box 的后果**：产生不一致状态——"the periodicity length no longer fits" 已显式复制出来的内容（粒子数是 Na·Nb·Nc 倍，而盒子没变大）[手册]。
- **Assign unique IDs**：给副本分配新的唯一标识符；否则副本与原件共享 Identifier，"which may cause problems with other modifiers"（如依赖标识唯一性的 Manual selection）[手册]。开启时还会同步修正副本的 `Molecule Identifier` 属性（若存在）[手册]。
- **跨边界分子（3.10.1+ 行为）**：若粒子带有 `Periodic Image` 属性且 Adjust box 保持开启，修饰器会在复制前隐式解包（unwrap）坐标、复制后重新包裹（rewrap），保证跨周期边界的分子在镜像中标识正确（类比 LAMMPS `replicate` 的 image flag 语义）[手册]。
- **键与其他元素**：随粒子一并复制（受 Operate on 列表控制）；跨周期边界的键会被扩展到镜像副本。
- **发布属性**：不发布新的分析属性；粒子/键数量成倍增长，`Position`、键端点等随之更新。
- **错误条件**：手册未记载（关闭 Adjust box 的"不一致状态"是唯一明确警告）。

## AtomX 实现要点（gotchas）

1. **Na/Nb/Nc 默认 1/1/1**（即无操作）；任意方向 >1 时需自动扩展晶胞（乘以复制数）——这是默认行为，AtomX 必须把复数联动做成默认开启的选项，并允许用户关闭（关闭即进入手册所述不一致状态，UI 应给出警告而非阻止）。
2. 复制的坐标生成公式：镜像 (i,j,k) 的粒子位置 = 原位置 + i·a + j·b + k·c（i∈[0,Na) 等）。若上游有 `Periodic Image` 属性，先解包再复制、复制后重包裹（3.10.1+ 语义）。
3. Identifier 策略二选一：保留原 ID（默认关闭 Assign unique IDs 时）或重编 ID + 同步 Molecule Identifier。AtomX 应两者都支持；下游依赖 ID 唯一性的功能（手动选择、位移匹配）要在 ID 重复时明确报错或提示。
4. PBC 方向本身不门控复制数——X/Y/Z 三个 spinner 与该方向是否 periodic 无绑定（手册未记载任何联动）[待核实 GUI]。非周期方向同样可以复制（用于"显式铺开"）。
5. 键复制时注意跨边界键的图像展开：原键的 `Periodic Image` 偏移要加到镜像副本的键上，否则键会拉错位置。
6. Replicate 放在需要完整邻域的分析（CNA/CSP 等）之前会成倍放大计算量；放在删除类修饰器之后则只复制剩余元素——管道位置语义与 OVITO 一致即可。

## 验证状态

两个选项的语义与 Adjust box 默认开启已由 [手册] 证实；Na/Nb/Nc 默认值与取值下界、Assign unique IDs 默认状态、Operate on 类别清单 —— **待核实（对照安装版 OVITO 3.16.1 GUI）**。
