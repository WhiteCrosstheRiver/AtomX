# AtomX 功能对齐清单

这份清单根据用户提供的五张 OVITO 截图整理。截图中的菜单文字是功能参考，不是额外的操作指令。vendor/zed 仅参考 UI 配色、密度与面板结构，没有移植它的业务功能。版本：0.1 开发版。

状态定义：**已实现**表示程序内存在可执行实现；**部分**表示存在明确限制；**待实现**不代表可用，界面不提供伪实现。OVITO 功能定义参考 [官方修改器手册](https://www.ovito.org/manual/reference/pipelines/modifiers/index.html)。

## 工作区、输入和渲染

| 截图功能 | 状态 | 当前行为 / 下一步 |
|---|---|---|
| Top / Bottom / Front / Back / Left / Right | 已实现 | 六个固定相机方向 |
| Ortho / Perspective | 已实现 | 正交与透视，左键旋转、右键平移、滚轮缩放 |
| 多视口与 Window layout | 部分 | 单视口 / 四视口，活动视口切换；没有任意拆分 |
| Preview mode / Adjust view | 部分 | Fit all、Fit selected、PNG 尺寸设置；没有输出安全框 |
| Constrain rotation | 待实现 | 当前俯仰角有限制，没有独立约束开关 |
| Create camera | 待实现 | 尚无可保存的场景相机对象 |
| Pipeline visibility | 部分 | 单数据源，粒子 / 晶胞可见性开关 |
| Configure graphics | 部分 | 启动参数选择显卡、显示预算、背景、球体半径 |
| 修改器堆栈 | 已实现 | 添加、开关、删除、前移、128 步撤销/重做 |
| 多个数据管线 | 待实现 | 当前同时加载一个数据源 |
| XYZ / Extended XYZ | 部分 | 多帧、Lattice、pbc、Properties 中的 species/type 和 pos；额外粒子属性尚不保留 |
| POSCAR / CONTCAR / CIF / LAMMPS data | 部分 | 单结构读取与导出，晶胞、元素和笛卡尔/分数坐标；复杂键拓扑、电荷和约束尚不保留 |
| 文件序列 / 搜索模式 | 待实现 | 支持单文件多帧；不支持目录通配符序列 |
| 多时间步 / 轨迹播放 | 已实现 | 64 位偏移索引、后台载入、播放、暂停、首尾帧、帧滑块；用户显示从第 1 帧开始，内部索引从 0 开始 |
| Detect reduced coordinates | 待实现 | 当前位置按文件中的笛卡尔坐标读取 |
| Generate bounding box | 部分 | 没有 Lattice 时显示粒子包围盒，不生成周期晶胞 |
| Sort particles by ID | 待实现 | 当前保留加载顺序，未解析 ID 属性 |
| Particles / Simulation cell / Global attributes | 部分 | 虚拟化坐标表、3x3 晶胞和 PBC、原始注释 |
| 单帧 GPU 图片导出 | 已实现 | 活动相机 PNG，64–8192 像素，粒子与背景；晶胞 UI 叠层不进入 PNG |
| 完整动画 / 范围 / Every Nth frame | 待实现 | 播放可用，动画文件导出不可用 |
| 背景颜色 | 已实现 | RGB |
| 透明背景 / 半透明粒子 / 抗锯齿等级 | 待实现 | 当前不透明单采样渲染 |
| Standard / Wireframe / Flat / Cinematic preview | 部分 | 已提供四种 Direct3D 11 实时预览模式；Wireframe 为独立光栅路径，尚未接入 OptiX/OSPRay/Tachyon |
| OpenGL / Tachyon / OSPRay / VisRTX | 未接入 | OVITO 的这些后端需要独立渲染库；当前没有用空壳选项冒充已安装后端 |
| Intel / NVIDIA / AMD | 部分 | 通用 D3D11 feature level 11.0 路径，Intel 与 NVIDIA 在本机实测；AMD 未实测 |
| 几亿原子 | 未达到完整目标 | 流式扫描 + 有界采样；没有几亿原子全量显存驻留 / 全精度交互的验证 |

## Pipeline / analysis status, 2026-09-23

- Color coding is stored per pipeline node and writes the selected Position/scalar values into the evaluated dataset. Automatic/symmetric/manual range, exact cancellable range scanning across XYZ/LAMMPS dump frames (up to 2 million atoms per frame), discrete mapping, inversion, selected-only coloring, and keep-selection are implemented. The viewport and active-view PNG show the selected property and numeric range. The D3D11 viewport, legend and PNG now sample one shared 256-step table; Magma, Viridis and Plasma use Matplotlib's published 256-color tables (8-bit RGB), while other gradients retain AtomX's existing formulas sampled into the common table.
- Fixed-cutoff common-neighbor analysis publishes `Structure Type`, global structure counts, and a result table; periodic FCC, BCC, HCP (triclinic), and an isolated icosahedral-center fixture are covered. Adaptive CNA and production-scale acceleration remain incomplete. The coordination-based DXA helper remains an explicitly approximate prepass.
- Create bonds preserves and de-duplicates existing topology by default, tracks periodic image shifts, and renders GPU lines in viewports and image exports. It supports one global cutoff or a symmetric per-type-pair cutoff matrix (up to 32 types); visibility, color, and pixel width are configurable. Cylinder rendering remains future work.
- Coordination, cluster, RDF, histogram, and reduce-property entries execute as pipeline modifiers and publish particle properties, global values, or data tables. Analysis tables are virtualized and export to CSV. Particle-table manual selection is one persistent pipeline node: click replaces the set, Ctrl-click toggles one particle, and invalid indices are reported rather than silently applied.
- While a source frame or modifier stack is being re-evaluated, the previous result remains visible with a `STALE RESULT` footer marker. Failed evaluations retain that marker until a successful publish.
- Pipeline evaluation now takes ownership of its working `Dataset` snapshot, removing the extra full-dataset copy between the asynchronous worker input and its result. The application still makes one working copy from the retained source; node-level caching and shared immutable storage remain incomplete.
- During asynchronous evaluation the status bar reports the active pipeline stage (`i / node count`); this is stage-level progress only, not an estimate of work remaining inside a long-running modifier.

## Analysis

| 修改器 | 状态 | 实现范围 / 所需后续工作 |
|---|---|---|
| Atomic strain | 待实现 | 参考构型、邻居映射、局部变形梯度与应变 |
| Bond analysis | 待实现 | 显式键拓扑与键角/长度分布 |
| Cluster analysis | 部分 | 可组合管线节点：周期最小镜像 cutoff 连通分量、Cluster 粒子属性、簇尺寸表；不超过 200 万原子 |
| Coordination analysis | 部分 | 可组合管线节点：Coordination 粒子属性和全局均值；正交与三斜周期最小镜像；不超过 200 万原子 |
| Difference between frames | 待实现 | 持久 ID 匹配与属性差值 |
| Dislocation analysis (DXA) | 待实现 | 晶格识别、Burgers 回路和位错网络 |
| Displacement vectors | 待实现 | 参考帧匹配、周期展开与矢量显示 |
| Elastic strain calculation | 待实现 | 晶格局部拟合与弹性变形 |
| Find rings | 待实现 | 键图最短环分析 |
| Grain segmentation | 待实现 | 局部晶体取向及晶粒聚类 |
| Histogram | 部分 | 可选择位置分量或现有标量属性，配置 1–4096 bins，发布 Data Table |
| Reduce property | 部分 | 位置分量或现有标量属性的 min/max/mean/sum，发布全局属性 |
| Scatter plot | 待实现 | 属性选择、二维图与导出 |
| Spatial binning | 待实现 | 空间网格统计与场数据 |
| Spatial correlation function | 待实现 | 相关函数、周期性和误差控制 |
| Structure factor | 待实现 | 倒空间采样和傅里叶计算 |
| Time averaging | 待实现 | 流式跨帧聚合 |
| Time series | 待实现 | 帧属性采样及曲线 |
| Voronoi analysis | 待实现 | 周期 / 非正交晶胞下的多面体构造 |
| Wigner-Seitz defect analysis | 待实现 | 参考晶格位点占据、空位与间隙原子 |

邻域分析与选择共享 linked-cell 邻居搜索内核；在采样数据上明确拒绝运行。截断邻居会损坏配位数与聚类结果，不能以可视化采样代替全数据科学分析。正交和三斜周期晶胞均走周期最小镜像；非正交最小镜像使用有界精确搜索，部分周期 slab/wire 只要求周期向量独立，缺失的非周期向量由搜索内部补基，不改变真实笛卡尔距离。200 万原子与候选比较上限用于防止超大任务无界运行。

## Coloring / Modification / Python

| 修改器 | 状态 | 范围 |
|---|---|---|
| Ambient occlusion | 待实现 | 当前仅球体解析法线、漫反射与高光 |
| Assign color | 部分 | 管线节点对选中粒子赋色；无选区时作用于全部粒子；粒子色可检查且进入 GPU 渲染，并随筛选/复制映射；颜色不写入当前结构文件格式 |
| Color by type | 已实现 | 默认 8 色循环，选中粒子高亮 |
| 粒子形状 | 部分 | 全局选择 Sphere、Circle、Cube、Cylinder、Spherocylinder；按类型的独立半径/颜色/形状编辑待做 |
| Color coding | 部分 | GPU 按节点配置的 Position 或数值粒子属性着色，支持当前帧自动/对称/手动范围和跨 XYZ/LAMMPS dump 帧的精确异步范围、取消、离散、反转、仅选中和 Keep selection；全帧计算限每帧 200 万原子；视口、图例和 PNG 共用 256 项渐变表；Magma/Viridis/Plasma 使用官方 8-bit 色表，其余现有渐变公式仍保留 |
| Affine transformation | 部分 | 按轴平移、统一比例缩放、旋转坐标与晶胞；不是完整 3x4 仿射矩阵 |
| Combine datasets | 待实现 | 属性对齐、类型合并、晶胞处理 |
| Compute property | 部分 | 安全原生数值表达式逐粒子计算并发布标量属性，可供下游节点读取；不支持向量表达式、单位系统、任意脚本及优化缓存 |
| Delete selected | 已实现 | 非破坏性管线过滤 |
| Freeze property | 待实现 | 按稳定 ID 保存参考属性 |
| Load trajectory | 部分 | 单个 XYZ 多帧文件；未支持拓扑和轨迹文件合并 |
| Python script | 待实现 | 尚无嵌入式 Python 或插件 API |
| Replicate | 部分 | 按晶胞向量复制、选区同步、2000 万原子预算；尚无持久 ID |
| Slice | 部分 | 轴向半空间切片，保留坐标小于阈值的原子；任意平面与厚度待做 |
| Smooth trajectory | 待实现 | 时间窗口及周期展开 |
| Unwrap trajectories | 待实现 | 稳定 ID、跨帧周期跳跃处理 |
| Wrap at periodic boundaries | 部分 | 正交晶胞、按文件 PBC 标记操作，原点固定为零 |
| Assign shared visual element | 待实现 | 多管线共享外观 |
| Calculate local entropy | 待实现 | 局部 RDF、积分与参数控制 |
| Identify FCC planar faults | 待实现 | 局部结构、层错分类 |
| Render LAMMPS regions | 待实现 | region 解析与几何可视化 |
| Shrink-wrap simulation box | 待实现 | 更新晶胞并处理周期语义 |
| Get more modifiers | 待实现 | 插件包发现与版本机制 |

## Selection / Structure identification / Visualization

| 修改器 | 状态 | 范围 |
|---|---|---|
| Clear selection | 已实现 | 清空选择掩码 |
| Expand selection | 部分 | 通过 cutoff 邻接扩展当前选区，支持 1–64 层及正交周期最小镜像；非正交 PBC 和大规模邻域任务待做 |
| Expression selection | 部分 | 安全原生表达式支持坐标/类型/标量属性、算术、比较、逻辑和 abs/sqrt/isfinite；不执行脚本，无向量分量语法、单位和帧变量 |
| Invert selection | 已实现 | 当前管线中的粒子选择取反 |
| Manual selection | 部分 | 粒子表选择存入一个持久 Manual selection 管线节点；单击替换选区、Ctrl 单击切换单个粒子；视口 picking、框选与套索待实现 |
| Select type | 已实现 | species/type 映射后的类型索引 |
| Find overlapping particles | 部分 | cutoff 邻居对中的所有端点均被选中；粒子半径感知的 overlap、非正交 PBC 待做 |
| Ackland-Jones analysis | 待实现 | 邻居键角结构分类 |
| Centrosymmetry parameter | 待实现 | 最近邻最优配对 |
| Chill+ | 待实现 | 冰相局域键序参数 |
| Common neighbor analysis | 部分 | 固定 cutoff 公共近邻签名；周期 FCC 与 BCC fixtures 验证通过。自适应 cutoff、HCP/ICO 专门 fixtures、非正交 PBC 和性能加速仍待完成 |
| Identify diamond structure | 待实现 | 多壳层邻域识别 |
| Polyhedral template matching | 待实现 | 模板匹配、取向和 RMSD |
| VoroTop analysis | 待实现 | Voronoi 拓扑签名及分类器 |
| Construct surface mesh | 待实现 | 表面重建、周期网格、法向 |
| Coordination polyhedra | 待实现 | 邻域凸包 |
| Create bonds | 部分 | 固定 cutoff 或最多 32 种类型的对称类型对 cutoff；周期镜像位移、默认拓扑保留与去重；D3D11 可见线、颜色/宽度/可见性；无键圆柱 |
| Create isosurface | 待实现 | 体数据、等值面提取 |
| Generate trajectory lines | 待实现 | 帧间匹配、周期分段和曲线绘制 |

## 达到产品目标所需的后续里程碑

1. 精确大数据引擎：磁盘缓存、空间层次块、按视锥 / 像素误差的 LOD、异步 GPU 驻留与淘汰、块级选择、量化坐标的精度界限。当前 stride 预览不是这套系统的替代品。
2. 用 1 亿、3 亿、5 亿真实数据建立冷读时间、交互帧时间 P50/P95/P99、CPU 峰值内存、显存预算、IO 带宽的基准，并分别覆盖 Intel 核显、Intel Arc、AMD 和 NVIDIA。
3. 完整属性 / ID / 键 / 体素类型系统，多格式读取和项目保存，构建经过物理参考数据验证的分析模块。
4. 优先补齐 CNA / PTM / DXA / Voronoi / 应变等用户关心的分析，与 OVITO 对照结果和容差。
5. 输出管线、抗锯齿 / AO / 透明度、电影导出、离线高质量渲染和插件系统。



## 2026-09-21 界面与基础修改器更新

- 原始用户 Logo 嵌入可执行文件、标题栏、任务栏和托盘，无外部图片路径依赖。
- 四列分类、可搜索、非模态 Add modification 下拉；点击外部或 Esc 可收起。无 Pro 门槛；尚未实现的算法灰显并标注不执行。
- 无系统标题栏；保留原生拖动、双击最大化、边缘缩放和任务栏最小化。X / Alt+F4 收到托盘，电源键彻底退出。
- Slate dark / Classic light / Midnight、Segoe UI / Arial / Consolas、14–20 px 字体设置立即应用并持久化。
- Rotate、Replicate、Coordinate range selection、Edit particle types 已实现，并支持现有撤销/重做及启停。
- 邻域距离直方图与全周期正交/三斜晶胞 RDF 已实现为可排序管线节点；Coordination、Cluster、Histogram、Reduce property 也提供节点参数和可检查属性/数据表，数据表可导出 CSV。各节点仍有数据规模和物理模型限制，详见上表。
- 高级晶格识别、DXA、Voronoi、显式键拓扑、Python 等仍待实现，不能将这次改动视为 OVITO 全功能完成。
