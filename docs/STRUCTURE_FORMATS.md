# 结构导入与导出

保存窗口自动填写目录和文件名。目录优先使用上次导出位置，其次输入文件目录、项目目录，最后使用 Windows 文档目录。上次导出位置随应用设置保存；已删除的目录不会沿用。普通格式采用体系名称与对应扩展名，同目录同格式导出会加 `-export`，避免默认覆盖输入。

**POSCAR 默认文件名就是 `POSCAR`，没有扩展名。** 用户仍可主动指定 `CONTCAR`、`.vasp`、`.poscar` 或 `.contcar`。默认多帧导出分别写入 `frame_000000/POSCAR`、`frame_000001/POSCAR` 等目录；主动选择 `.vasp` 等扩展名时沿用 `name_000000.vasp` 命名。

| 格式 | 导入 | 导出 | 内容与限制 |
| --- | --- | --- | --- |
| XYZ / Extended XYZ | 多帧 | 多帧 | 元素、坐标；扩展 XYZ 可保留晶胞、原点、PBC 和所选科学属性 |
| POSCAR / CONTCAR | 单体系 | 单体系 / 分文件序列 | 晶胞、分组坐标、可选选择性动力学 |
| CIF | 单体系 | 单体系 / 分文件序列 | P1 晶胞与分数坐标；当前不展开其他空间群 |
| LAMMPS data | 单体系 | 单体系 / 分文件序列 | atomic 风格，受限三斜晶胞；不存 PBC 标志和键 |
| LAMMPS dump | 多帧 | 多帧 | 文本原子轨迹、受限三斜晶胞、PBC |
| PDB / ENT | 单模型 | 单模型 / 分文件序列 | 元素、坐标、CONECT 单键、形式电荷；不导入残基元数据。导出最多 99,999 原子，坐标 3 位小数。多重键/芳香键无法无损表达时拒绝导出，改用 MOL/SDF；晶胞采用常规取向，PBC 轴标志不保留 |
| GRO | 单体系 | 单体系 / 分文件序列 | 坐标与晶胞，nm 单位；最多 99,999 原子；不保存分子拓扑 |
| **MOL** | 单分子 | 单分子 / 分文件序列 | V2000，坐标 4 位小数、单/双/三/芳香键、形式电荷；每条记录最多 999 原子与 999 键 |
| **SDF / SD** | 多记录 | 多记录 | V2000，同 MOL；每条分子记录作为独立帧。SD 文本字段不映射为逐原子属性 |
| **XSF** | 单体系 | 单体系 / 分文件序列 | ATOMS 或 PRIMVEC/PRIMCOORD、Å 坐标、完整晶胞向量、X/XY/XYZ 周期维度；导入可读取 Hartree/Å 力列；导出不保存力。暂不支持网格、AXSF 动画、任意组合 PBC 轴 |
| AtomX `.atomx` | 单文档 | 单文档 | 完整体系、键级、周期镜像、科学属性、约束、标签、显示和相机 |

MOL/SDF 不保存晶胞、周期键、任意科学属性、同位素、自由基、立体化学、查询原子和 Sgroup。无法表达的键、非法电荷、超出固定列宽/数量限制会报错；格式失败时不会覆盖原有目标文件。采样导入只保留两个端点都存在的显式键，重新编号。

## 格式依据

- [BIOVIA CTfile Formats 2020](https://discover.3ds.com/sites/default/files/2020-08/biovia_ctfileformats_2020.pdf)：V2000、M CHG、SDF 分隔符。
- [XCrySDen XSF](http://www.xcrysden.org/doc/XSF.html)：晶胞向量、坐标块、周期维度。
- [wwPDB 3.3](https://www.wwpdb.org/documentation/file-format-content/format33/v3.3.html)：固定列坐标、电荷、CRYST1 与 CONECT。

## 验证

`structure_io_tests` 覆盖默认文件名/目录、MOL/SDF 键级和形式电荷往返、SDF 多帧定位、PDB 固定列与双向连接去重、XSF 三斜晶胞与二维周期、采样键重编号、容量/截断/格式不支持时目标文件保护。

`export_workflow` 覆盖真实应用异步导出、默认 POSCAR 分帧目录、显式 `.vasp` 序列、输入覆盖保护、取消/失败时临时文件清理。
