# AtomX 修改器与界面对齐实施计划

本计划以用户提供的 OVITO 3.16.1「Add modification」截图为逐项范围基准，同时以 [OVITO 官方修改器目录](https://docs.ovito.org/reference/pipelines/modifiers/index.html)、各功能说明和本机 OVITO Basic 实际界面作为行为参照。目标是让 AtomX 的按钮、参数、计算、输出和数据检查形成完整闭环。参考界面与行为，不复制 OVITO 源代码、图标资产或私有实现。Pro 标记只表示 OVITO 的发行版本差异，AtomX 不设置授权门槛。用户已确定暂不兼容 `Python script`，本轮实施范围因此为截图中的其余 **66 项修改器和 Modifier templates**；脚本项保留在清单中并始终标为“暂缓”，不能计入完成率。

## 完成的定义与当前基线

**一个实施范围内的菜单项只有同时满足以下六项，才记为完成：**①入口与分类位置正确且可操作；②默认值、控件类型、参数依赖和错误提示有参照记录；③修改器在实际 Pipeline 中运行，能取消、报错、缓存、禁用、重排和撤销；④结果以正确的粒子属性/键/表/网格/线/全局属性发布，并在视口或数据检查器可见；⑤固定科学样本的数值或拓扑验证通过；⑥参考与 AtomX 截图、键盘和 DPI 回归通过。禁用项、点击后仅显示“准备中”、把近似算法冒充正式算法，均计为**未完成**。对于模板管理命令，用同等严格的操作/持久化/回归验收替代数值项；M09 单列为暂缓，不参与本轮分母。

现有代码审计是本轮开始实施前的基线；工作树已有未提交改动，实施中保留并分别确认归属，不重置、不覆盖。截图范围共有 **67 个修改器**，另有 **Modifier templates** 管理入口；基线时 AtomX 仅 4 项有基础可执行行为，13 项部分实现或行为不符，48 项禁用占位，2 项菜单缺失。4 项也尚未完成上述六项验收。

| 分类 | 截图项数 | 基础可执行 | 部分/有误 | 占位 | 缺失 |
|---|---:|---:|---:|---:|---:|
| Analysis | 23 | 0 | 4 | 18 | 1 |
| Modification | 15 | 1 | 5 | 9 | 0 |
| Structure identification | 7 | 0 | 0 | 7 | 0 |
| Selection | 7 | 3 | 1 | 3 | 0 |
| Visualization | 6 | 0 | 1 | 4 | 1 |
| Python modifiers | 5 | 0 | 0 | 5 | 0 |
| Coloring | 4 | 0 | 2 | 2 | 0 |
| **合计** | **67** | **4** | **13** | **48** | **2** |

当前尤其不能混淆以下状态：`dxaApproximate()` 只是按邻居数寻找疑似缺陷核心，未生成 Burgers 矢量与位错线；Create bonds 目前为 GPU 线段，尚无键圆柱。Color coding 按每节点属性着色，支持当前帧和受限的精确全帧范围；图例现已绘制于视口并写入活动视口 PNG，调色板仍是 AtomX 自身的近似颜色表，尚未按 OVITO 标准色表进行数值级核对。Pipeline 分析节点已可组合执行，但节点级缓存和完整结果数据对象检查器尚未完成。以上项目应在可验收记录中显式标为“部分/有误”，不得出现在“已对齐”汇总里。

## 参考资料采集与逐项台账

行为判定优先查 [Pipeline 工作方式](https://docs.ovito.org/usage/pipeline.html)、[Color coding 参数](https://docs.ovito.org/reference/pipelines/modifiers/color_coding.html)、[DXA 输出与参数](https://docs.ovito.org/reference/pipelines/modifiers/dislocation_analysis.html) 等官方页面，再以固定数据集核对 GUI。官方页面描述的是 OVITO 行为目标；AtomX 的具体算法、默认值与硬件能力必须分别验证，不能从文档文字直接推断已实现。

1. 固定 OVITO 版本、Windows 缩放、窗口尺寸、语言、示例数据和当前帧。对每项保存：菜单行、首次添加后的默认面板、修改关键参数后的面板、Pipeline 树、结果数据标签与视口截图。对多帧、选择、颜色/图例及输出对象再拍状态变化图。AtomX 用同一数据和分辨率保存对应截图，并记录测量尺寸，而非靠主观印象比较。
2. 本机 OVITO Basic 能实点的功能以实际界面和官方文档双重核对。Basic 中不能打开的 Pro 面板以官方手册、教程和公开示例为准；资料不足的参数标“待核实”，不臆造默认值。每张参考图记录出处、版本、数据集与帧号。已有实际核对样例：Color coding 面板包含对象、输入属性、渐变、起止值、自动/对称范围、离散化、当前帧/所有帧范围、反转、仅选中与保留选择；其结果应同时进入粒子颜色、Pipeline 和图例。
3. 建立一条记录对应一个条目的台账（建议 `docs/modifier-parity.csv`），字段为 `id/category/name/reference_url/reference_shots/menu/panel/engine/output/numeric_test/ui_test/owner/commit/limitations`。前六个完成字段独立记为 `todo/partial/pass/fail`；只有全为 `pass` 才显示“完成”。每轮交付从台账自动生成剩余项和失败原因。
4. 为所有面板保存可复现的参数清单：标签、控件类型、单位、缺省值、范围、可见性条件、帮助文字、确认/取消行为、是否触发重新计算。跨版本差异在台账中记录，不凭截图推断不存在的计算功能。

## 必须先打通的架构

1. **类型化数据对象。** 把粒子、稳定粒子 ID、类型、键、晶胞、体素网格、表面网格、位错线/轨迹线、矢量场、表格、全局属性和时间步纳入统一所有权与生命周期。属性带类型、分量、单位、来源、缺失值标记；完整数据与 LOD/采样预览分开标识。邻域、拓扑和积分算法拒绝把采样预览当全量数据。
2. **真正的 Pipeline。** 将当前 UI 的 `mods` 状态迁移到执行图。每个节点定义参数 schema、输入/输出对象、依赖帧、前置条件、进度、取消、诊断、缓存键和序列化。参数变更只使下游失效；禁用/删除/前移/后移/复制及撤销/重做恢复数据与面板状态。界面选中节点后，下方独立展示参数和结果，而不是把所有参数塞进当前约 205 px 的列表。
3. **科学计算公共层。** 实现三斜晶胞的分数坐标、最小镜像、周期复制/边界处理、可扩展邻居索引、稳定 ID 跨帧匹配、键拓扑、直方图/统计、网格与 FFT 基元。明确定义双精度计算与单精度显示边界，避免大尺寸晶胞引起精度损失。每项算法可选全量后台任务，进度、取消、内存预算和具体错误位置统一展示。
4. **输出与渲染接口。** 结果对象注册到 Pipeline 可见元素和底部检查器；渲染器分别处理粒子、键、线、网格、矢量、体素和标签。导出对话框只出现当前对象及格式适用的选项。科学数据导出后重新导入，对数量、属性、晶胞和帧数做往返校验。
5. **渐进重构。** 将 `src/main.cpp` 内的菜单、参数、执行代码逐步移至 UI、modifier、data、render 模块；每个小变更保持构建可运行。已有未提交改动先归档/核验，避免这份计划覆盖现有工作。

## 67 项逐项台账：66 项实施，1 项暂缓

下列“最小可验收结果”只是结果契约，不代替参数采集和数值验证；任何条目都仍须满足前述六项完成条件。序号与截图一一对应。含 Pro 的条目在 AtomX 中无付费限制；用户界面可去掉 `(Pro)` 文案，但保留分类与排序映射。

### Analysis（23）

| ID | 菜单项 | 最小可验收结果与关键依赖 |
|---|---|---|
| A01 | Atomic strain | 参考构型、ID/邻居匹配与局部变形梯度；输出应变张量及失配统计。 |
| A02 | Bader charge integration | 体素电荷密度与原子关联；输出原子盆地电荷、体积分割及积分守恒检查。 |
| A03 | Bond angle distribution | 显式键图/邻域及角度直方图，输出可导出数据表。 |
| A04 | Bond length distribution | 键长统计与分布表，明确周期键长度。 |
| A05 | Bond order | 局部取向序参量及按类型统计，公开归一化和邻居规则。 |
| A06 | Cluster analysis | 距离/键连通性、Cluster ID、簇大小表和周期跨边界一致性。 |
| A07 | Difference between frames | 以稳定 ID 对齐两帧并输出指定属性差值与缺失 ID 诊断。 |
| A08 | Dislocation analysis (DXA) | 真正的晶格判定、Burgers 回路/矢量、位错线网络、缺陷网格与长度/类型统计；不得沿用 `dxaApproximate` 名称冒充。 |
| A09 | Displacement vectors | 参考帧、周期展开后的位移属性和可单独控制的箭头对象。 |
| A10 | Elastic strain calculation | 参考晶格、局部拟合和弹性应变张量，非晶/失败点明确标记。 |
| A11 | Find rings | 基于键图的环搜索、尺寸分布、环对象/表；定义重复环去重。 |
| A12 | Grain segmentation | 局部结构/取向、晶粒聚类、晶粒 ID/取向/大小表。 |
| A13 | Histogram | 任意数值粒子/键属性、分量、过滤、箱数与频数表。 |
| A14 | Radial distribution function (RDF) | 全量邻居统计、体积/PBC 归一化及类型对的 g(r) 表。 |
| A15 | Reduce property | 对选定对象/属性执行 min/max/mean/sum 等聚合并发布全局属性。 |
| A16 | Scatter plot | 双属性/分量散点数据、选择过滤、图表与导出。 |
| A17 | Spatial binning | 按轴/网格分箱的统计场、空箱规则与体素/表格结果。 |
| A18 | Spatial correlation function | 指定属性相关函数、周期边界、归一化及误差/统计表。 |
| A19 | Structure factor | 倒空间采样/FFT、波矢单位与 S(q) 数据/图。 |
| A20 | Time averaging | 跨帧属性平均及时间范围、ID 消失/新增策略。 |
| A21 | Time series | 跨帧全局/约简属性时间表、时间戳与缺帧处理。 |
| A22 | Voronoi analysis | 三斜 PBC 下体积/面数/邻居、退化点与有限单元处理。 |
| A23 | Wigner-Seitz defect analysis | 参考位点占据、空位/间隙原子数与可视化标记。 |

### Modification（15）

| ID | 菜单项 | 最小可验收结果与关键依赖 |
|---|---|---|
| M01 | Affine transformation | 粒子/晶胞/矢量的矩阵变换，坐标系和对象选择明确。 |
| M02 | Combine datasets | 两个数据源按对象和属性合并，类型/ID 冲突策略可见。 |
| M03 | Compute property | 安全的原生表达式解析器，支持分量、条件、邻域变量与错误定位。 |
| M04 | Delete selected | 对目标对象删除并正确重映射 ID、键及属性；撤销往返。 |
| M05 | Edit simulation cell | 2D/3D、PBC、向量、原点以及是否连同粒子变换。 |
| M06 | Edit types | 类型 ID、名称、颜色、半径、质量、形状与批量映射。 |
| M07 | Freeze property | 指定帧的属性快照，后续帧以 ID 对齐。 |
| M08 | Load trajectory | 拓扑+轨迹合并，ID 映射、帧索引、缺失原子诊断。 |
| M09 | Python script | **暂缓，不计入本轮 66 项。** 菜单明确显示“暂不支持任意 Python 脚本”，不能点后假装执行；不嵌入 Python 运行时。 |
| M10 | Remove property | 粒子/键/网格等适用属性删除，依赖节点报清晰错误，支持撤销。 |
| M11 | Replicate | 三方向复制、晶胞扩展、稳定 ID/键复制与 PBC。 |
| M12 | Slice | 平面/厚度/反选、粒子与适用网格裁切及可视平面。 |
| M13 | Smooth trajectory | 跨帧滤波、端点策略、ID/周期展开、延迟和帧窗口。 |
| M14 | Unwrap trajectories | 按稳定 ID 跨 PBC 连续展开并处理跳帧。 |
| M15 | Wrap at periodic boundaries | 任意三斜晶胞的分数坐标取模与非周期轴处理。 |

### Structure identification（7）

所有项须发布算法定义的结构 ID/名称及数量和占比；需要局部取向的算法再输出取向。结构分类不能用配位数阈值替代签名/模板算法。

| ID | 菜单项 | 最小可验收结果与关键依赖 |
|---|---|---|
| S01 | Ackland-Jones analysis | 局部角分布判定与参考晶体/缺陷样本分类。 |
| S02 | Centrosymmetry parameter | 对向邻居配对最小化、参数属性及 BCC/FCC 测例。 |
| S03 | Chill+ | 水分子/键拓扑与局部氢键环境分类；缺水模型时清晰拒绝。 |
| S04 | Common neighbor analysis | 真正的 common-neighbor 签名与 adaptive cutoff，FCC/HCP/BCC/ICO/Other 验证。 |
| S05 | Identify diamond structure | 第一、二近邻拓扑与立方/六方金刚石识别。 |
| S06 | Polyhedral template matching | 模板拟合、RMSD、局部取向、尺度与模板匹配结果。 |
| S07 | VoroTop analysis | Voronoi 拓扑指纹、过滤规则和分类结果。 |

### Selection（7）

| ID | 菜单项 | 最小可验收结果与关键依赖 |
|---|---|---|
| Q01 | Clear selection | 清除指定对象选择，输出数量和撤销。 |
| Q02 | Expand selection | 邻居/键图扩展步数与 PBC 处理。 |
| Q03 | Expression selection | 安全表达式、类型检查、分量/时间变量与错误位置。 |
| Q04 | Find overlapping particles | 距离阈值、重复对去重和被选数量。 |
| Q05 | Invert selection | 当前对象范围内取反，数量一致。 |
| Q06 | Manual selection | 点击/框选/套索到稳定 ID 的持久选择，跨帧策略。 |
| Q07 | Select type | 多类型选择、类型改名后的稳定行为。 |

### Visualization（6）

| ID | 菜单项 | 最小可验收结果与关键依赖 |
|---|---|---|
| V01 | Add text labels | 属性绑定、位置、字体、遮挡、颜色及导出叠层。 |
| V02 | Construct surface mesh | 粒子表面/空腔网格、面积/体积统计与独立可见对象。 |
| V03 | Create bonds | 距离/类型规则、周期跨边界键、实际线/圆柱渲染和键表。 |
| V04 | Create isosurface | 体素标量阈值、网格法线与表面数据/渲染。 |
| V05 | Coordination polyhedra | 以邻居集合生成多面体、类型/颜色/透明度及网格检查。 |
| V06 | Generate trajectory lines | 稳定 ID、帧范围/步长、周期展开和可独立隐藏的线对象。 |

### Python modifiers（5）

这五个命令按已确定的原生 C++ 原则实现等价功能，不通过 Python 桥接或授权门槛；保留截图中的菜单分组位置作为导航参照。它们与暂缓的任意脚本入口 M09 是不同范围。

| ID | 菜单项 | 最小可验收结果与关键依赖 |
|---|---|---|
| P01 | Assign shared visual element | 多对象共享外观引用，修改同步与序列化。 |
| P02 | Calculate local entropy | 类型/邻域局部熵属性，归一化和参考样本。 |
| P03 | Identify fcc planar faults | 依赖可靠结构识别，层错类型/层对象与统计。 |
| P04 | Render LAMMPS regions | 解析支持的 region 命令、几何预览与不支持语法诊断。 |
| P05 | Shrink-wrap simulation box | 粒子外包边界生成/更新晶胞，边距和 PBC 策略。 |

### Coloring（4）

| ID | 菜单项 | 最小可验收结果与关键依赖 |
|---|---|---|
| C01 | Ambient occlusion | 可重现的遮蔽强度/半径设置及颜色或渲染输出。 |
| C02 | Assign color | 目标对象/选择掩码、常量颜色及覆盖顺序。 |
| C03 | Color by type | 按类型外观表着色，不借用 Position 渐变路径。 |
| C04 | Color coding | 任意数值属性/分量、对象类型、渐变、自动/对称/手动/全帧范围、反转、离散、选中限定、图例和 GPU/导出一致。 |

**Modifier templates（单独管理入口）**：保存当前修改器及参数为模板；列表搜索、重命名、应用、覆盖/删除、导入/导出、版本迁移；重新启动后保持。菜单“Get more modifiers”如果继续保留，应明确指向真实可安装来源或关闭，不显示无效入口。

## UI 与按钮对齐验收

- 「Add modification...」位于 Pipeline 节点树上方；展开后按截图的四列组卡片、组标题和顺序排列，可搜索、滚动、键盘导航、Esc/外部点击关闭。灰色只表示因当前数据缺少前提而不可用，并显示具体原因；未实现项在开发版明确标状态，不提供假动作。宽度、行高、字体、组间距、选中/悬停反馈在固定 100%、125%、150% DPI 和浅/深主题下比对，不裁切文字或将按钮挤出屏幕。
- 右侧采用「可见对象 + 修改器树 + 数据源」及「当前选中节点的参数/结果」分区；勾选、拖拽重排、上移/下移、删除、复制、错误/忙碌状态与撤销可见。修改器输出的缺陷网格、位错线、键、轨迹线等作为独立可见对象。参数变化只刷新受影响的下游节点。
- 底部检查器至少提供 Particles、Bonds、Dislocations、Simulation Cell、Global Attributes、Data Tables、Surfaces；有体素或线对象时显示对应检查页。表格虚拟化、列类型/单位、选择同步、复制/CSV 导出；菜单项产生的每个结果都能找到。
- Color coding、DXA、Simulation Cell、粒子类型、渲染/导出面板分别拍默认与有结果状态；四视口、活动黄色边框、黑色背景、时间轴和工具提示保持统一。截图对比只验证可测的布局/文案/状态，不把抗锯齿、字体渲染差异误判为功能错误。

## 实施顺序与阶段出口

| 阶段 | 可回退交付包 | 阶段出口 |
|---|---|---|
| 0. 基线与证据 | 保留现有工作树；67 项台账、固定参考工程/截图、构建与测试命令、现状标记 | 每个条目有来源和验收样本；无虚假“已实现”。 |
| 1. 公共数据与执行图 | 类型化对象、ID、属性 schema、Pipeline 节点/缓存/任务、三斜 PBC 与邻居引擎 | 测试证明重排/禁用/撤销、跨帧映射、取消及采样拒绝。 |
| 2. 完整工作区壳 | 四列菜单、当前节点参数区、可见对象树、底部检查器和结果导出 | 对参考截图逐页核对，67 项均有可追踪状态；不宣称算法完成。 |
| 3. 低依赖闭环 | Selection 7、Coloring 4、基础 Modification 与模板 | 每项都从输入数据经 Pipeline 产生结果，含 UI/数值回归。 |
| 4. 轨迹与图表 | ID/多帧、差帧/位移/时间平均/序列、Histogram/RDF/Scatter/Reduce/Spatial binning | 多帧拖动、重算和表格/CSV 往返一致。 |
| 5. 邻域与结构 | Bonds、Cluster、角/长/序参量、CNA、Ackland-Jones、CSP、Diamond、PTM、Grain、Voronoi/VoroTop | 理想晶体、热扰动、缺陷和三斜 PBC 参考集通过；近似代码退出正式名称。 |
| 6. 高复杂分析 | Atomic/elastic strain、Wigner-Seitz、DXA、Surface/isosurface、Bader、相关函数/结构因子、局部熵/层错 | 位错网络和 Burgers 矢量、体素积分守恒、网格拓扑等专门验收通过。 |
| 7. 可视化与导出 | 全部可见对象、标签/轨迹线/多面体、格式化数据和动画导出；M09 保持清晰的暂缓提示 | 本轮范围 66/66 + 模板入口逐项全链路通过，导出再导入无数据丢失。 |
| 8. 性能/跨 GPU/发布 | 有界全量计算、GPU 分块/LOD、NVIDIA/Intel 测试、可复现实测报告 | 1M 目标 60 FPS、10M 可操作预览、100M+ 分块/LOD；按注明 GPU、分辨率、数据和采样策略报告，不把预览帧率当全量精度。 |

各阶段可并行研究/实现不同算法，但必须按依赖合并：例如 DXA 在真实结构识别与线/网格数据对象可用前不能验收；Bader 在体素输入和积分校验前不能验收。每个交付包是一个小而可回退的提交，编译与相关测试必须通过；不要一次性把 67 个占位按钮改为可点。

## 测试矩阵与发布门槛

- **固定样本**：单原子、空数据、随机气体、FCC/HCP/BCC/金刚石、含空位/间隙/位错晶体、水模型、有键分子、三斜 PBC、跨边界轨迹、体素电荷密度；所有样本和参考结果记录生成方式。Pro 参考仅使用文档与合法可运行示例，不假称本机 Basic 已验证 Pro 面板。
- **数值验证**：每项独立写明容差与拓扑判据。示例：理想晶体结构分类计数、周期键/邻居对、RDF 壳峰、Voronoi 体积和总晶胞体积、DXA Burgers 矢量/线类型与总长度、Bader 总电子数守恒。每项涵盖正常、边界、错误输入、禁用和采样数据；性能优化不能改变科学输出而不记录误差。
- **端到端验证**：从导入 → 添加 → 编辑参数 → 播放/切帧 → 禁用/重排 → 检查结果 → 导出 → 撤销/重做 → 保存/重开。UI 图像比较结合控件几何、文案与状态断言；不以单张漂亮截图替代功能验证。
- **性能验证**：指定 GPU/驱动、显示分辨率、粒子数、属性数、PBC、帧数、显存/内存预算，记录 p50/p95 帧时间、首次加载/重算时间、峰值内存、GPU 占用、采样或 LOD 比例；NVIDIA 和 Intel 独显分别运行。100M+ 只承诺可验证的流式/LOD 交互，不宣称所有粒子完整同时驻留显存。
- **发布判定**：台账中本轮 66 项及模板入口满足完成定义、所有必需格式/结果对象可往返、全部回归通过、剩余限制公开，才能宣称“本轮功能范围完成”；由于 M09 暂缓，不得宣称“OVITO 全量功能对齐”。任何未通过项进入明确的未完成列表，而不是以菜单覆盖率充当完成率。

## 已确定的脚本范围

用户选择“暂不兼容脚本，先完成其余功能”。M09 保持暂缓并给出明确提示；安全表达式解析器仅服务 Expression selection/Compute property，不宣称兼容 OVITO Python 语法。未来如需运行既有 `.py` 脚本，应另立计划处理运行时、安全、打包、对象绑定和兼容测试；本轮 66 项与模板入口不依赖它。

## 执行记录

- **2026-09-23：Color coding 属性着色第一步。** D3D11 为每块粒子上传所选标量缓冲；颜色映射读取该缓冲，支持动态选择当前数据集中的数值属性、自动范围、对称范围、离散、反向及仅给选中粒子着色。`build.ps1` 增加 GPU 回归：固定粒子位置、交换属性值后图像哈希必须改变；仅选中模式与全体模式输出也必须不同。MSVC 构建、core/POSCAR/IO 测试与 NVIDIA GeForce RTX 5090 D v2 GPU 渲染测试全部通过。此条仍为**部分实现**，未完成每节点独立参数、全部渐变准确度、图例、全帧范围、Keep selection 与 Intel GPU 像素验证。
- **2026-09-23：Pipeline 和菜单第一步。** 主程序的修改器存储改为真实 `ModifierNode` 集合；新增/删除/重排/启用和导出评估都通过 `PipelineGraph` 节点状态执行，并开始在节点上记录错误、脏状态和输出对象类型。四列目录已加入截图中的 Bader integration、Add text labels、Modifier templates 入口；未实现项仍灰显，`Python script` 标为暂缓，近似分类明确说明不是 CNA。节点级缓存、专用参数/结果编辑区、模板管理器和真实算法仍未完成。`--catalog --smoke` 截图已人工检查列布局，构建和 GPU/核心/导入测试通过。
- **2026-09-23：邻域内核与 Selection 第一批。** 将 cutoff linked-cell neighbor traversal 收敛为一个共享实现，Cluster/RDF/协调数、键对和选择不再各自维护邻域循环。`Expand selection` 可由已有选区向外扩展 1–64 个邻居层；`Find overlapping particles` 可选择所有落入至少一个 cutoff 粒子对的粒子。两项菜单已启用，面板可编辑 cutoff 并显示结果计数；对 sampled preview 明确拒绝，保留正交周期最小镜像与比较预算限制。新增链状邻居、扩展多层、重叠对端点、采样拒绝回归。完整构建、核心/格式测试和 NVIDIA D3D11 形状及属性着色测试通过。Selection 仍缺安全表达式与视口 picking；overlap 目前按统一 cutoff 判断，不考虑每种粒子的渲染半径。
- **2026-09-23：安全表达式选择。** 新增独立原生表达式解析器，支持 `x/y/z` 与 `Position.X/Y/Z`、粒子类型、已有标量属性、算术、比较、逻辑运算及 `abs/sqrt/isfinite`；未知属性/函数、非法数值和除零报带列号错误，不提供任意脚本或文件/进程 API。Expression selection 已接入真实 Pipeline 节点和参数编辑面板，回归覆盖混合逻辑、属性读取、定义域错误和不允许的函数。完整构建及核心/导入/GPU 回归通过。下一步可将该解析器提取为 Compute property 的表达式基础，但当前只用于布尔选择。
- **2026-09-23：Compute property 第一版。** 重用相同的限制型原生表达式解析器，为每个粒子计算数值并发布命名标量属性；输出名与表达式可在节点面板编辑，后续节点可立即读取。测试验证计算值以及下游 Expression selection 消费刚生成属性；完整构建和核心/格式/GPU 回归通过。仍缺向量/分量表达式、单位与属性元数据、缓存和进度显示。
- **2026-09-23：用户验收反馈中的体验问题。** 单帧时间轴总数改为 `0 / 1`（帧刻度仍从 0 开始）；适配器列表按 DXGI LUID 隐藏重复别名；默认粒子半径由 0.23 调到 0.32；正交相机 Fit 依据当前投影方向上的晶胞包围范围计算视野，避免使用最长世界坐标轴导致视口留白过大。全量构建、core/格式/GPU 测试通过；Intel 设备未在本机运行验证。
- **2026-09-23：全帧 Color coding 范围与图例。** Range worker 对 XYZ/LAMMPS dump 每帧完整读取（每帧上限 200 万粒子），先运行目标颜色节点的上游管线，再扫描有限属性值；支持进度、取消、同轨迹切帧保留范围及上游参数更改失效。`colorRangeAcrossFrames` 有跨帧/上游变换/进度/取消数值回归，真实两帧 Extended XYZ 测试覆盖索引、完整读入与属性汇总。视口 ImGui 图例和活动视口 PNG 图例显示属性、渐变及端值；D3D11 渲染测试导出 PNG 并验证反向/离散后输出变化。`build.ps1` 五组测试和 UI screenshot smoke 全部通过。剩余：每帧超过 200 万的流式属性扫描、OVITO 标准渐变颜色数值核对、Intel GPU 图例/像素检查。
- **2026-09-23：固定 cutoff CNA 第一版。** 用共享空间邻域引擎构造公共近邻图并计算 Honeycutt–Andersen 局部键拓扑签名，管线发布 `CNA Structure` 分类属性；按周期 FCC 与 BCC 晶体 fixtures 验证所有原子正确分类。菜单由近似配位分类改为真实 Common neighbor analysis，并显示分类统计。局部公共图最长链搜索设预算，超限时保守归入 Other；当前只对 FCC/BCC 完成样本验证，HCP/ICO、自适应 cutoff、非正交 PBC 与生产级优化仍未完成。最新全量构建、格式及 GPU 回归通过。
- **2026-09-25：P1 波次（Slice/Replicate/EditCell）对齐 OVITO 实测行为。** 建立干净会话下的 OVITO 真值流程（发现强杀后流水线恢复会污染截图；Add modification 菜单自动聚焦搜索框，不得点击菜单内部）。Slice 重写为任意法向平面：严格半空间（d=5.4 法向 x → 64 remaining，与 OVITO 实测一致）、居中闭区间 slab（d=5.4 w=1.8 → 18 remaining，一致）、Reverse orientation、Create selection (do not delete)、Apply to selection only、Operate-on 元素行、input/deleted/remaining 计数与视口半透明切平面（D3D11 alpha blend，深度测试开/写关）；默认法向 (1,0,0)、距离取晶胞中点、Visualize plane 默认关。Replicate 升级为 Na/Nb/Nc 三方向复制 + Adjust box size + 跨轴周期键重映射（修复 bond.b 未写回缺陷）。Edit simulation cell 面板顺序按实测改为原点→向量→PBC。新增 29 个测试：Slice 语义/错误/选区路径、Replicate 三轴与键重映射，以及用户强调的修饰器栈组合回归（Replicate→Slice ≠ Slice→Replicate、上游改参下游重算、双 Slice 节点参数互不串扰、插入仅下游置脏、禁用上游等价于移除）。构建与 5 组测试全绿，无头 smoke/catalog 截图通过，GUI 双端比对（面板布局与计数）通过。遗留：Align view/Pick three points 按钮未实现、Miller indices 占位、多 Slice 节点时结果行仅反映最后执行节点、Replicate 面板文案与 OVITO 尚有差异。
- **2026-09-25：N1 波次（Centrosymmetry parameter）。** 由灰显占位转为真实修饰器：Conventional（贪心最小对权）与 Minimum-weight matching（位掩码 DP 精确最小权匹配）双模式、邻居数默认 12（偶数校验 2–64）、仅选中粒子、节点面板实时橙色 CSP 直方图、发布 Centrosymmetry 粒子属性与统计全局属性并可被下游 Color coding 选择。GUI 验收发现热扰动样本 CSP 离群（134 vs 真值 1.91），根因为 XYZ 缺省 pbc 被默认非周期，修复为默认周期（对齐 OVITO，惠及全部邻域分析）；引擎补自适应截断增长重试与欠配位回退。数值与独立暴力参考逐原子一致（1e-6），构建/五组测试/无头截图全绿。
- **后续工作：** 完成节点级缓存与共享数据策略、完整鼠标交互回归、显示范围/选择适配、剩余基础分析与修改器；补充 OVITO 官方渐变色参考值和 Intel GPU 对照。DXA/PTM/Voronoi 等高复杂模块仍属于单独的后续里程碑。
