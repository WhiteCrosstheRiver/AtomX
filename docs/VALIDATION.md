# 本机验证记录

日期：2026-09-21。Windows x64 / MSVC 19.44，Release `/O2`。这是一次开发机运行记录，不是标准化跨设备性能保证。

## 构建与核心测试

`build.ps1` 编译原生 GUI 和 `core_tests.exe`，测试覆盖：多帧偏移索引、跳帧读取、Extended XYZ 属性顺序、晶胞与 PBC 元数据、有界采样、类型选择与删除、非破坏性切片、周期 Wrap、缩放、直方图数量守恒、XYZ 导出回读、截断文件拒绝、周期 FCC 配位数、周期聚类、重复周期分箱去重、采样数据分析拒绝。

科学检查：4×4×4 FCC 晶胞启用三轴 PBC、cutoff=0.8，所有原子的最近邻配位数为 12，连通分量为 1。另以跨周期边界的二原子对和孤立第三原子检查最小镜像与聚类数量。这些检查不等同于对 OVITO 所有科学模块的验证。

## 图形检查

渲染模式现在提供 Standard GPU、Wireframe GPU、Flat particle preview 和 Cinematic GPU preview。它们共享 Direct3D 11 设备；Wireframe 使用独立光栅化状态，其余模式是不同的实时预览质量路径，不冒充未安装的 OptiX/OSPRay/Tachyon 后端。

| 显卡 | 数据 | 布局 | 观测 UI 帧率 |
|---|---|---|---|
| NVIDIA GeForce RTX 5090 D v2 | 55,296 原子，全量 | 四视口 | 约 60 FPS |
| Intel(R) Graphics | 55,296 原子，全量 | 四视口 | 约 60 FPS |
| NVIDIA GeForce RTX 5090 D v2 | 400 万源原子，200 万实际绘制，stride=2 | 四视口 | 约 60 FPS |
| Intel(R) Graphics | 400 万源原子，200 万实际绘制，stride=2 | 四视口 | 约 27 FPS |

垂直同步开启，上限约 60 FPS。数字来自 ImGui 的滑动窗口帧率，不是 GPU timestamp query，不包括冷启动导入时间，也不代表长时间帧时间分布。绘制球体半径为 0.23，默认窗口 1560×1000（客户区略小）。视口绘制每帧执行，未启用静态帧复用。

原始证据：`build/smoke-nvidia.txt`、`build/smoke-intel.txt`、`build/benchmark-4m-nvidia.txt`、`build/benchmark-4m-intel.txt` 及对应 PNG。截图已检查面板布局、实际原子显示和源 / 预览计数；检查中修正了 WIC PNG 通道顺序错误。

## 文件读取基准

通过 C++ 生成 100×100×100 FCC，4,000,000 个原子，文件大小 62,800,087 字节。

| 阶段 | 一次观测 |
|---|---|
| 生成与写入 | 3848.08 ms |
| 全文件帧索引扫描 | 347.411 ms |
| 再扫描并解析 200 万预览原子 | 1661.73 ms |
| 实际保留的原子缓冲 | 32,000,000 字节 |

文件刚写入，可能命中操作系统缓存，**不是冷盘读取基准**。生成器为 `tests/benchmark.cpp`，报告为 `build/benchmark-4m.txt`。

## 尚未验证 / 尚未实现

- 几亿原子的全量文件载入、全量显示、交互帧时间和输出正确性。
- Intel Arc 独显、AMD、多种驱动版本、远程桌面 GPU 切换和设备丢失恢复。
- 极端坐标尺度、非正交周期邻域、稳定 ID 跨帧映射、通用 Extended XYZ 属性保留。
- 所有参考截图中的高级分析、轨迹电影、透明渲染、AO / 光追和完整项目持久化。
- 安装包、签名、跨机器依赖部署与长期压力测试。

本机约 20.8 GB 物理内存。当前架构在 CPU 同时保留源预览与修改结果，不能把大显存硬件作为绕过系统内存预算的理由。完整几亿原子支持需要进一步改成真正的分块驻留与空间层次细节系统。

## 2026-09-23 管线分析与键渲染更新

- 先保存开发基线 `bdbd4a4`；后续实现以小提交交付，忽略的构建产物不纳入版本控制。
- `build.ps1` 当前统一执行核心算法、POSCAR、结构格式、D3D11 形状/着色/键外观、数据导出工作流五组测试，全部通过。实测渲染适配器为 NVIDIA GeForce RTX 5090 D v2；Intel Arc 未验证。
- 新增管线节点的数值回归覆盖 Coordination 属性与均值、Cluster 标签与簇大小表、FCC RDF 表、可配置直方图、Reduce mean、选区/全体赋色与复制/删除颜色映射、CSV 转义，以及采样数据拒绝和节点错误归属。
- 全帧颜色范围测试覆盖两个帧的 XYZ 属性聚合，以及 GRO、CIF、LAMMPS data、POSCAR、PDB 静态文件的完整单帧范围；界面计算在后台运行、支持取消，并且每帧超过 200 万原子时明确拒绝，不使用预览采样冒充完整范围。
- 全帧范围扫描所有已支持输入格式：XYZ / LAMMPS dump 遍历轨迹帧，静态结构格式作为单帧处理；每帧严格限制 200 万原子，未知粒子数的静态输入若触发采样或超限会报错，不以预览数据计算。手动改颜色属性或上游修改器后会将下游全帧范围标为需重算；更改轨迹帧时保留已算出的跨帧范围。
- D3D11 渲染测试导出并检查 `build/shape-validation/color-legend.png`；Windows workspace smoke 测试截图 `build/ui-color-legend.png` 目视验证四视口图例与属性范围。活动视口 PNG 使用同一颜色渐变及反向/离散设置绘制图例。
- D3D11 渲染回归使用一个远端离群原子对比全体取景与选区取景，验证 Fit selected 不会被全数据包围盒拉远。工作区 smoke 截图 `build/fit-selected-workspace.png` 检查四视口控件和禁用状态；没有执行鼠标框选粒子的完整端到端 GUI 测试。
- 手动选区节点测试覆盖多粒子集合、与 Delete selected 的管线顺序组合及越界索引错误；`build/manual-selection-workspace.png` 检查工作区布局。普通鼠标单击 / Ctrl 单击的桌面自动交互尚未验证。
- 管线 / 轨迹载入期间保留旧画面并在状态栏显示 `STALE RESULT`，成功发布后清除；加载失败恢复进入加载前的标记状态。MSVC 完整测试与普通工作区 smoke 通过；本轮没有用自动 GUI 操作强制制造失败状态，因此 stale 提示未做截图视觉验收。
- CNA 输出统一为 `Structure Type`，提供全局结构计数和结果表；本地固定样本包含 FCC、BCC、三斜周期 HCP 与孤立 ICO 中心。
- GPU 渲染回归检查键线像素、隐藏行为、颜色及线宽变化、逐粒子赋色和七种粒子形状。键按 image shift 跨周期展开。
- Create bonds 数值测试覆盖类型对阈值矩阵、对称表校验、零阈值禁用和不同元素的候选距离过滤。
- `--smoke-bond-pairs --smoke 5 --screenshot build/bond-type-cutoffs.png` 实际渲染了 Cu/Ni 类型对编辑面板；检查了下三角的对称提示和默认阈值。没有自动鼠标点击矩阵单元格验证键盘输入/拖动行为。
- `tests/export_workflow.cpp` 验证帧范围、步长、序列导出、应用管线、拒绝覆盖输入和失败清理；Data Tables 增加 CSV 导出。
- `--smoke 3 --catalog --screenshot build/phase-catalog.png` 与普通工作区截图运行成功，目视检查四视口和菜单。时间轴标签与单帧计数已改为 1-based 用户显示；未执行自动鼠标逐项点击回归。
- CMakeLists 已注册与 PowerShell 入口相同的五组测试，但当前环境找不到 `cmake` 命令，因此本次只实跑 `build.ps1`，没有声称 CTest 通过。
- 仍未完成：缓存复用/节点级按需拷贝、稳定大数据异步索引和可复现交互测试；键圆柱；渐变色与 OVITO 标准色表的数值级比对；高级结构算法、DXA、Voronoi、完整透明/AO及 Intel Arc 实机验证。

## 2026-09-24 Pipeline 工作集复制

- `evaluate` 接收工作 Dataset 按值所有权；异步 worker 将自己的工作副本 move 到 PipelineResult，避免之前 worker 输入与评估结果之间的第二份完整粒子/属性复制。应用仍保留原始 source 并在开始计算时复制一份工作集；节点缓存和共享不可变存储尚未实现。
- 两节点数值回归验证执行顺序和 active-node 进度值；桌面 footer 仅展示当前第几个 Pipeline 节点，不显示伪造的节点内百分比。
- `build.ps1` 五组核心、格式、渲染与导出测试全部通过；此处记录的是经代码路径确认的复制次数变化，不是性能 benchmark，不据此声称固定帧率或内存百分比收益。

## 2026-09-24 部分周期三斜邻居搜索

- 加入倾斜二维周期 slab 测试：c 向量为零且 z 非周期，a/b 方向跨边界成键正确返回 image shift `{1,0,0}`，z 向分离的原子不会误连。
- `build.ps1` 五组核心、格式、渲染与导出测试通过；原有三维三斜 HCP/CNA 与键 image shift 回归仍通过。尚无高倾斜大规模 slab 性能基准。


## 2026-09-21 界面改版验证

- `build.ps1`：MSVC Release 编译成功，核心测试通过。新增断言覆盖区间选择、选中类型修改、复制后的坐标/晶胞/选区、旋转后的坐标/晶胞、非法缩放拒绝，以及 FCC RDF 积分恢复配位数 12。
- `--smoke 15 --desktop-test`：任务栏最小化、最大化、WM_CLOSE 隐藏到托盘、恢复最大化状态、标题栏拖动命中测试通过。
- 三列下拉、默认工作区和设置弹窗均实际渲染并检查截图。额外读取 Classic light / Arial / 20 px 和 Midnight / Consolas / 14 px 配置完成启动截图检查，测试后恢复原配置。
- XYZ 三帧示例加载并截图成功；适配器索引 1 在本次枚举中为 NVIDIA RTX 3050，因此不把此次运行误记为 Intel 验证。
- 测试截图位于忽略提交的 `build/ui-*.png`，窗口测试报告位于 `build/desktop-test.txt`。
- 尚未完成手动鼠标逐项回归、系统重启恢复、不同 DPI / 多显示器拖动回归。RDF 测试是解析 FCC 基准，未声称与 OVITO 全算法对齐。


### UI 阅读性与轨迹尺改版

- MSVC 构建、核心回归通过。175% Windows DPI 下按 2730×1750 窗口实际截图检查，默认浅色菜单三列内容完整显示。
- 合成的 37 帧 / 每帧 4000 粒子 XYZ 验证轨迹加载。扩展的 `--desktop-test` 验证后台读帧时后一个定位请求生效，以及首尾越界请求限制；测试结束位于第 36 帧。
- 检查 `build/ui-v2.png`、`build/ui-v2-catalog.png`、`build/ui-v2-settings-large.png` 和 `build/ui-v2-dark.png`，覆盖轨迹尺、菜单卡片、20 px 设置和深色菜单。
- 保留已存外观配置；验证过程中临时配置已恢复。尚未执行跨显示器 DPI 切换回归，当前缩放取自启动显示器。

### 共享渐变表验证

- D3D11 粒子、视口图例和 PNG 导出共用相同的 256 项渐变表。Magma、Viridis、Plasma 色值取 Matplotlib 公布的 256-entry CC0 表并量化为 RGB8；其余渐变沿用 AtomX 配方后采样进同一张表。
- `render_shapes` 检查已知端点、所有渐变 257 个输入位置的有限/归一化范围，并对十种渐变实际渲染图像做区分测试。
- 此项验证的是 AtomX 内部的 GPU/图例一致性与色表取值，不代表所有渐变与 OVITO 私有/专有实现数值一致。

### Simulation Cell 管线编辑验证

- `core_tests` 覆盖修改晶胞向量、原点和 PBC 时默认不移动粒子；显式开启时通过旧/新晶胞分数坐标映射粒子位置；退化晶胞在对应管线节点报错。
- 参数面板提供三条向量、原点、X/Y/Z 周期边界和可选坐标映射开关。晶胞尺寸快捷输入和实际 GUI 鼠标回归尚未覆盖。

### 三斜晶胞 Wrap 验证

- `core_tests` 增加倾斜 a/b 向量、X/Y 周期而 Z 非周期、非零原点样本，检查被包装坐标落回周期范围且非周期方向保持不变。
- Wrap 对任意独立周期向量求周期子空间分数坐标；重复/退化周期向量会在对应管线节点报错，不会静默跳过。

### 3x4 Affine transformation 验证

- `core_tests` 用非均匀缩放、剪切和位移矩阵验证粒子坐标、晶胞向量和原点的同步变换；奇异线性部分在管线节点报告错误。
- 桌面参数面板显示可编辑 3x4 矩阵；粒子向量属性没有自动变换，避免在不知道数据物理语义时错误改变速度、位移或法向量。
