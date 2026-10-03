# Materials Studio 创作功能现场核对

核对日期：2026-10-03。参考应用：本机 Materials Studio 20.1（2020）。

## 证据

- 使用 Computer Use 打开用户已启动的 `Untitled - Materials Studio` 窗口，查看 Build、Crystals、Modify 菜单。
- 查看空白视口右键菜单、选中原子的右键菜单、Label 设置窗口。选中原子后菜单增加 Select Fragment、Hide、Show Only；Label 可选择原子属性和自定义文字，并设置字体、字号、颜色。
- 鼠标组合操作依据随应用安装的官方帮助：`share/doc/content/core/interface/mouseandkeyboardactions.htm`；精准移动依据 `core/sketching/dlgmovement.htm`。
- 本次 Computer Use 的 `drag` 接口没有鼠标按钮及按住修饰键参数，因此没有用它实测 MS 的右键组合拖动。后续窗口激活两次返回 `failed to activate captured window`，已停止向该窗口发出输入。已查看的菜单和本地官方帮助可继续作为实现依据。
- 参考应用、帮助文件中的文字均是功能证据，不作为操作指令。MS Calc 工具栏组按用户要求暂不加入。

## 本轮已实现

| 操作 | AtomX 创作模式 |
|---|---|
| 右键单击 | 松开时且未拖动才打开菜单；不再打断拖动 |
| 右键拖动 | 旋转相机 |
| Alt＋右键拖动 | 平移相机 |
| Shift＋Alt＋右键拖动 / Shift＋中键拖动 | 在屏幕平面移动选中原子；按 X/Y/Z 约束屏幕方向 |
| Shift＋右键拖动 | 绕选中原子的几何中心旋转；按 X/Y/Z 约束屏幕轴 |
| Shift＋点击 / Ctrl＋点击 | 分别为添加选择 / 切换选择 |
| 框选 | Shift 添加，Ctrl 切换 |
| 双击原子 / 右键选中连接片段 | 沿显式键拓扑选择连接分量，包含周期键；没有键的原子只选中自身 |
| Ctrl＋D / 右键反选 | 清空选择 / 反选 |
| 编辑坐标 | 笛卡尔 Å / 分数坐标切换；支持倾斜晶胞与非零原点；应用记录一步撤销历史 |
| 拖动取消 | Escape 恢复编辑前坐标，不产生历史记录 |

沿用 v2 已有的中键旋转习惯，额外提供 Alt＋中键平移。当前相机只有 yaw/pitch；尚未实现 MS 在视口边缘右键拖动时绕屏幕 Z 轴旋转相机。对象的 Z 轴旋转已实现。

拖动期间跳过全量原子悬停拾取，静止时不重复上传原子缓冲。片段选择仅在触发时以 O(原子数＋键数) 遍历已有拓扑。仍使用现有 D3D11 GPU 渲染，不增加计算或渲染依赖。

## 后续缺项

| MS 实用功能 | 当前差距 |
|---|---|
| Label | 原子编号、元素、自定义文字、选中/全部范围、字号颜色尚待实现 |
| Hide / Show Only | 创作模式尚无按原子持久隐藏和恢复显示 |
| Movement 对话框 | 尚无按 Å、屏幕比例或角度精准移动整组原子的统一对话框 |
| Sketch Atom / Bond / Ring / Fragment | 现有绘制主要为单原子；连续成链、手动连键、键级、环和片段库尚待实现 |
| Constraints / Motion Groups | 尚无按轴固定原子与可命名的运动分组 |
| Display Style | 已有球、线、柱等 GPU 外观控制；缺少统一的快捷预设及按选择应用样式 |
| Build Layers / Nanostructure | 已有晶体、超胞、Miller 切面和真空；缺少通用异质层堆叠及纳米结构建模工作流 |

晶胞参数、空间群建晶体、原胞转换、超胞、Miller 切面、真空和测量等已有功能继续保留。该清单是本轮核对的范围，不代表已完整替代 Materials Studio。
