# AtomX

C++20 原生 Windows 原子可视化工作区，使用 Direct3D 11。界面参考 `vendor/zed` 的 One Dark 色彩、紧凑标签与面板布局。界面采用用户提供的 AtomX Logo、紧凑工具栏、三列分类修改器下拉菜单和纯黑原子视窗。当前版本是可运行的开发版，**尚未全面对齐 OVITO，也未实现几亿原子的全量高效渲染**。

## 启动

双击根目录 `Run AtomX.cmd`，或执行 `build\AtomX.exe`。启动后显示 FCC Cu/Ni 晶体，无需准备输入文件。

```powershell
.\build\AtomX.exe
.\build\AtomX.exe "D:\simulation\trajectory.xyz"
.\build\AtomX.exe --adapter 1
```

显卡索引在 System 面板可见，默认选择专用显存最大的可用硬件适配器。本机索引 0 为 NVIDIA，1 为 Intel；其他电脑的索引可能不同。没有偷偷切换到 CPU 软件渲染。

## 已实现

- 单 / 四视口、六个方向与正交 / 透视相机，粒子与晶胞开关。
- XYZ / Extended XYZ 单文件多帧轨迹、拖入文件、后台索引 / 读取、取消、帧播放。
- ASE 常用结构交换格式：POSCAR/CONTCAR、CIF、LAMMPS data；导出会按扩展名选择对应格式，XYZ 仍支持多帧。
- GPU 实例化球体 impostor，16 字节原子记录、每块最多 1,048,576 原子；共享原子缓冲供所有视口使用。
- 原子预算内全量显示，超出时进行确定性 stride 采样，同时标示源数量、显示数量与 stride。
- 非破坏性修改器：轴向 Slice、类型选择、反选、清空、删除选中、平移、统一缩放、正交周期 Wrap、旋转、晶胞方向复制、坐标区间选择、编辑选中粒子类型；管线开关、前移与撤销/重做。
- 粒子坐标表、晶胞矩阵、原始注释、位置直方图 / min / max / mean。
- 后台空间分箱邻域配位数与距离聚类、邻域距离分布、全周期正交晶胞 RDF、CSV 分析导出；禁止对采样数据给出邻域分析结果。
- 活动相机 PNG 导出、处理后数据 XYZ 导出。
- 粒子外观形状：球体、圆盘、立方体、圆柱、spherocylinder；右侧 Render 面板可切换。

详见 [完整功能对齐表](docs/FEATURE_MATRIX.md) 和 [架构说明](docs/ARCHITECTURE.md)。未实现的算法在下拉菜单中灰显并说明开发状态，不能执行；所有已实现功能均无 Pro 或付费限制。

## 操作

| 操作 | 方法 |
|---|---|
| 导入 | Open trajectory / Ctrl+O / 拖入 XYZ 文件 |
| 旋转 | 视口内左键拖动 |
| 平移 | 视口内右键或中键拖动 |
| 缩放 | 滚轮 |
| 活动视口 | 点击视口 |
| 恢复构图 | Fit |
| 修改器 | Modifiers 或右侧 Add modification，下拉三列分类并可搜索 |
| 最小化 | 标题栏横线，收到任务栏 |
| X / Alt+F4 | 收到系统托盘，保持后台任务 |
| 完全退出 | 最右侧红色电源键，或托盘右键 Exit |
| 恢复窗口 | 双击托盘 Logo |
| 主题与字体 | Settings：三种主题、三种字体、14–20 px，自动保存 |
| 撤销 / 重做 | Ctrl+Z / Ctrl+Y |
| 邻域分析 | Analysis 标签，设置 cutoff 后 Compute neighbors |
| 大文件预览预算 | System → Preview atoms → Reload with budget |
| 图片导出 | Render 标签设置尺寸和背景，然后 Render active viewport |

当前 UI 为英文。XYZ 必须使用笛卡尔位置，额外粒子属性、稳定 ID、LAMMPS dump 等格式尚未支持。PNG 输出不包含晶胞 UI 叠层，轨迹电影导出尚未实现。无项目保存功能。

## 构建与测试

需要 Visual Studio 2022 C++ Build Tools、Windows SDK。Dear ImGui 1.91.9b 已包含在 `third_party/imgui`，无需安装 Qt、Python、CUDA 或 Vulkan SDK。

```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1
.\build\core_tests.exe
.\build\AtomX.exe --smoke 120 --screenshot build\smoke.png
```

也提供 CMake 工程，可用 Visual Studio x64 generator 构建。`build.ps1` 是本机实际验证过的构建入口。发布使用动态 MSVC runtime，其他电脑若缺失需安装相应 VC++ 运行库。

性能记录和测试边界见 [验证报告](docs/VALIDATION.md)。约 60 FPS 的小场景结果不能外推为几亿原子能力。当前没有磁盘空间层次索引、按视锥流式驻留、误差受控 LOD、跨厂商自动化驱动回归或科学分析的全量 OVITO 一致性测试。

## 依赖

Dear ImGui MIT 许可证见 `third_party/imgui/LICENSE.txt`。Zed 源码保留在用户提供的 vendor 中，AtomX 构建不依赖它，也没有复制 Zed 的功能代码。UI 设计参考路径为 `vendor/zed/assets/themes/one/one.json` 与 `vendor/zed/crates/ui/src/styles/spacing.rs`。


外观配置保存于 `%LOCALAPPDATA%/AtomX/settings.ini`。主题不改变原子视窗的黑色背景，Render 页面仍可手动调整视窗背景。窗口支持标题栏拖动、双击最大化和边缘调整大小。关闭至托盘失败时自动最小化，避免窗口丢失。退出时取消并等待后台工作，再释放资源。

新增算法边界：Rotate 同时旋转坐标和晶胞；Replicate 按选定晶胞向量复制 1–32 次，总量限制为 2000 万；区间选择基于当前管线坐标；类型编辑作用于选中粒子。RDF 使用 128 个球壳，按 `2 × pair_count / (N × density × shell_volume)` 归一化，只对全周期正交晶胞输出。邻域距离分布不是基于显式键拓扑的 Bond length distribution。


## 阅读优先的 UI 更新

默认外观改为 Classic light、18 px 逻辑字号，并按启动显示器的 Windows DPI 缩放字体与主要布局。已保存的主题和字体选择保持有效。工作区侧栏通过工具栏 Workspace 展开；粒子表通过 Data inspector 展开，默认把面积留给黑色四视窗。

Trajectory 采用主/次刻度尺，自动选择 1/2/5 系列刻度间隔；所有帧号统一从 0 开始。支持拖动刻度尺、直接输入帧号、首/尾帧、前/后帧与播放暂停。键盘 Tab 可聚焦刻度尺后使用左右箭头和 Home/End。后台读帧期间保留最后一次定位请求，不再为普通逐帧读取弹出阻断操作的加载对话框。

修改器菜单使用三列独立卡片、加粗分类标题、白色内容区和清晰边框。深色主题也提高了正文和次要文字对比度。
