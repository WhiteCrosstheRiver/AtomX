# 架构与性能边界

## 当前实现

`src/core.hpp` 是不依赖图形后端的数据核心：64 位轨迹帧偏移、XYZ schema 解析、16 字节 Atom 记录、非破坏性顺序修改器、统计与 XYZ 写出。

`src/analysis.hpp` 使用空间哈希分箱、正交周期最小镜像和 union-find，计算 cutoff 范围的配位数与连通分量。预期复杂度随局部密度变化，极端大 cutoff 会接近二次复杂度，因此有候选比较预算。它不是 GPU 分析实现，也没有冒充 DXA / CNA / PTM。

`src/renderer.hpp` 封装 DXGI 显卡选择、D3D11 设备、结构化原子缓冲、球体 impostor shader、深度缓冲、每视口离屏纹理与 WIC PNG 导出。原子缓冲对视口共享，顶点着色器每原子构造六顶点 billboard，像素着色器解析球面法线和表面深度。没有每个原子的独立 draw call，也没有 CPU 生成的大型球面三角网格。

`src/main.cpp` 使用 Dear ImGui / Win32 / D3D11 构建原生 UI，后台 future 执行轨迹读入和邻域分析。几何修改仍同步执行，在预算很大时可能造成短暂停顿。统计结果仅在数据变化时重新计算，粒子表使用 clipper，不逐帧遍历全表。

## 内存

每个显示原子的 GPU 原始记录为 16 字节。200 万个原子的记录是 32,000,000 字节，约 30.5 MiB；不包括驱动分配粒度、视口纹理、深度、UI 和其他资源。

CPU 同时保留源预览和管线输出；修改 / 载入期间可能短暂保留新旧数组，不能把“16 字节/原子”当作进程总内存。选择掩码和分析数组也有额外开销。导出 PNG 包含 staging、CPU RGBA 数组和编码器内存。

## 大文件语义

1. 第一遍扫描为每帧建立 64 位文件偏移和原子计数。
2. 第二遍顺序扫描所选帧；当源数量大于预算，按 `ceil(count/budget)` 的间隔保留记录。
3. UI 显示源原子数、实际预览数和 stride。导出与修改操作针对当前预览，而不是隐藏的全部源数据。
4. 缺失邻居会改变科学结论，因此配位 / 聚类分析拒绝采样输入。

这是**有界预览**，不是全量 out-of-core 科学可视化。均匀按行采样可能和数据排序产生混叠，不能保证小缺陷、表面或罕见物种的保留。采样模式只解析被保留记录的坐标；其他记录完成行数扫描，但并未逐条校验其属性内容。

## 后续大规模渲染设计

全量几亿原子的产品需要在导入阶段构建空间分块、块包围盒、量化误差元数据、不同空间尺度的代表集合和磁盘缓存。运行时先做块级视锥 / 投影大小筛选，再按显存预算调度驻留，配合 GPU 可见性筛选和间接绘制。近景需要从磁盘换入精确数据，而非永久删去 stride 间隔内的原子。交互降级、静止高质量、全精度分块导出应是不同可控模式。

300,000,000 × 16 字节只是 4.8 GB 的原始位置/类型，不含其他科学属性、双缓冲、索引或可见性数据；Intel 共享内存与独显必须独立设预算。几亿个球体每帧的像素重叠也会成为瓶颈，单纯能够分配记录不等于交互流畅。

## 官方参考

- [Microsoft：Direct3D feature levels](https://learn.microsoft.com/en-us/windows/win32/direct3d11/overviews-direct3d-11-devices-downlevel-intro)
- [OVITO：Modifier reference](https://www.ovito.org/manual/reference/pipelines/modifiers/index.html)
- [OVITO：Pipeline concept](https://ovito.org/manual/usage/pipeline.html)
