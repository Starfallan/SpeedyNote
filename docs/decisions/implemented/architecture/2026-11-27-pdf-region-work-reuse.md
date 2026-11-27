# Agent Note: PDF 区域执行复用与后台深色处理

Status: implemented

## Problem

step2 的 30 次测试成功，real-pan 在途任务从 23–24 降到 1，但运动中平均未绘制面积仍为 80.6%；矢量高倍空白积分相对 step1 增加 53.1%，深色高倍增加 77.8%。普通真实 PDF 滚动 CPU 增加 121.7%，空白只减少 9.6%。运行条件未完全控制，不能把全部差异归因于代码。

当前每次区域渲染重新执行页面；worker 的 provider 缓存属于线程，同一视口跨线程分配时无法稳定复用。深色图片区域提取和逐像素处理在 GUI 回调中。已完成但离屏的有效图像也会被丢弃，减少回看复用。

## Decision

复用已有两个全局提交位置，各自保留一个 worker provider，使单视口连续请求优先使用相同空闲位置。provider 的打开、解析及渲染由 worker 独占。文件或内容身份变化时在 worker 中释放旧 provider，再创建替代项，避免同时保留两份文档。GUI 的位置所有者与 provider 的实际内容身份分别记录：排队任务被跳过时只能改变前者，不能让下一个任务误用旧 provider。视口退出时等待任务结束并释放所属空闲位置；应用退出时释放其余 provider。

仅视口 worker 启用一个热点页 display list，普通整页释放指令表并直接渲染；高倍区域通过相同页面坐标的 scissor 重放指令，边界向外增加一个光栅像素的余量。新建指令表使用计数 allocator，将该 context 的 MuPDF 实时分配量（包括分配头）限制为 32 MiB；超预算或解释错误时丢弃指令表并恢复直接渲染，同页连续请求不反复尝试失败构建。限额只在记录指令期间启用，原有光栅像素限制保留，trimStore 不取消。清理 store 后 context 仍超限时也释放指令表。所有视口共用最多两个热点 provider，指令表数量不随线程或标签页增加。

深色模式和图片保留开关作为请求快照，使用 worker provider 的实际页映射提取图片区域并转换，GUI 仅接受当前 generation 结果并创建 QPixmap。已离屏且尚未执行的任务仍跳过；已完成且内容身份未失效的结果可进入现有缓存，缓存满时不得挤走当前需求。先检查容量再做主线程转换。

## Alternatives considered

| 方案 | 优点 | 当前选择 |
| --- | --- | --- |
| 扩大并发 | 可直接提高部分矢量场景吞吐 | 会增加 CPU 和内存竞争，先减少重复执行 |
| 每个线程各留多个 display list | 容易接入现有线程缓存 | 资源随线程增长且命中依赖任务分配，复用固定两个位置 |
| 不限制指令表或直接取消 trimStore | 实现简单、可能命中更多资源 | 缺少内存边界，不采用 |
| 所有离屏结果都丢弃 | 限制无关结果转换 | 计算已完成仍可能回看，允许在现有缓存空间中复用 |
| 同时改手形截图与预览 | 可覆盖更多空白场景 | 扩大变量，继续单独处理 |

## Testing

- 保留单视口一个任务、共享最多两个任务、像素预算和全部失效规则。
- 手动功能测试对缓存与直接渲染做完整像素比较，覆盖切页、72/144/384 DPI、分数坐标、照片、透明图形、旋转页和非零 CropBox；另检查限额拒绝后的直接渲染、深色图片保留开关、离屏完成结果复用、主题变更拒绝旧结果，以及原有文档切换/取消/共享额度。
- allocator 的分配失败、realloc、对齐、溢出和归还计数有独立检查。
- benchmark 探针、runner、语料协议不变；在本地审查与可用检查完成后再集中构建。功能测试不加入 Actions 上传条件。

无 Qt/MuPDF 的 allocator 检查使用本地 C++17 编译器编译并执行。原生应用编译由一次集中 Actions 构建验证；像素及生命周期检查由下载后的手动入口验证。没有 step3 性能实测前，不将实现状态解释为性能目标已达成。比较方法见 [Benchmark README](../../../../benchmarks/pdf/README.md)。

## Consequences

同页连续移动可以复用解释结果，深色图片区域提取可重放同页指令表，GUI 完成回调只做身份/容量检查、QPixmap 转换和局部重绘。完成后离屏的有效图片可以供回看使用。普通整页没有增加指令表构建成本。

首次区域请求增加指令记录及重放，必须计入首屏时间；失败后转直接渲染还会增加一次尝试的成本。32 MiB 是初始选择，不声称最优；若真实复杂页反复无法缓存或首屏/CPU 退化，应以 step2/step3 同条件实测重新评估，而非直接增大预算。

该限额不等于进程 RSS 上限，不覆盖 Qt 图片及外部库全部分配；光栅执行阶段也没有被此 allocator 限额约束。已打开文档本身可能超过 32 MiB，因此不能宣称两个 provider 总内存不超过 64 MiB。计数 allocator 的原子操作、额外保留的热点资源和完成后离屏图片转换都有成本，必须一起观察 CPU、峰值内存、首屏、空白积分和主线程长尾。已有任务仍不强制中断，手形截图新暴露区域仍未处理。

## Related records

已检索现有 PDF 决策：本篇与[可见需求调度](2026-11-27-pdf-visible-demand-scheduling.md)、[性能优先提案](../../proposed/architecture/2026-11-27-pdf-performance-first.md)部分重叠，保留其任务与资源约束，调整离屏完成结果策略；与[请求生命周期](../bug-fix/2026-11-27-pdf-render-request-lifecycle.md)部分重叠，保持 generation 所有权。遵循[手动验证](../process/2026-11-27-pdf-benchmark-manual-validation.md)和[benchmark](../testing/2026-11-27-pdf-benchmark.md)协议；[初版预览提案](../../rejected/architecture/2026-11-27-pdf-scroll-rendering.md)仍不采用。

接口依据：MuPDF 官方 [display-list.h](https://github.com/ArtifexSoftware/mupdf/blob/master/include/mupdf/fitz/display-list.h)、[context.h](https://github.com/ArtifexSoftware/mupdf/blob/master/include/mupdf/fitz/context.h)、[list-device.c](https://github.com/ArtifexSoftware/mupdf/blob/master/source/fitz/list-device.c)。scissor 与重放 ctm 使用同一坐标；指令表无公开精确大小接口，因此不依赖私有结构布局估算内存。
