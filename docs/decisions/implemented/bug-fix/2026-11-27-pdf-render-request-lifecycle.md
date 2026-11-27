# Agent Note: PDF 首步优化的请求去重与失效范围

Status: implemented

## Problem

用户已经对 `68ac33a` 启动 GitHub Actions，并准备自行采集 baseline，要求现在开始第一步性能优化。这个版本包含 `2f65852` 的渲染行为及测量工具。当前没有 baseline 数字，不能判断收益比例；本次处理可以从控制流直接确认的重复工作。

整页预加载只检查已完成缓存，重复调度及多个笔记页指向同一 PDF 页时会重复提交。已在后台渲染的页面若在 settled paint 中缺失，主线程还会重新执行一次。整页与区域共享 generation，使普通缩放下 `prunePdfTiles()` 清理 tile 缓存时连有效整页任务也失效。完成回调在判重之前执行深色转换与 QPixmap 转换，并对离屏结果请求全窗口重绘。

## Decision

整页请求按与现有缓存一致的 sourceId、原始 PDF 页号、DPI 判重，使用主线程管理的在途列表。每个条目由创建它的 watcher 清理；worker 完成但 GUI 回调尚未处理时仍视为在途。预加载批次内再次检查身份，覆盖重复笔记页。同步 cache miss 发现同一在途任务时返回空图，等待该任务完成后重绘，既不重复光栅化，也不在 paint 中等待 worker。

整页和区域分别拥有共享 atomic generation。全局失效仍覆盖文档、源、缩放、主题变化，并同时使两条路径过期；tile-only 失效不会影响整页结果。单页失效仍按所属路径的整个 generation 处理，暂不增加按页 epoch。整页 worker 在打开 provider 前及光栅化前检查有效性，区域 worker 增加打开 provider 后的检查。

失效时保留在途身份，防止旧任务与同键新任务并行、或旧回调移除新任务的标记。旧 owner 返回后，在现有 preload timer 未启动时安排一次当前需求检查，不重启已有 timer。只有过期任务完成触发这次检查，不添加周期轮询。`cancelAndWaitForBackgroundThreads()` 在所有 worker 结束且 watcher 删除后清理两类在途身份，支持视口复用。

完成回调先检查有效性、空结果及已有缓存，再执行深色处理和 QPixmap 转换。重绘区域只包含当前可见的同源同页位置及返回 tile 的范围；重复笔记页的所有可见位置都会更新。离屏结果和无法展示新结果的手势截图不触发无关重绘。Qt 合并已有 update 请求，不增加新的 repaint timer。

区域请求键保留 17 位浮点有效数字，避免默认字符串精度把不同 DPI/区域合成同一个 key。覆盖复用仍沿用现有规则，本次没有引入固定网格或新的图片缓存。

## Scope

这是[性能优先提案](../../proposed/architecture/2026-11-27-pdf-performance-first.md)中减少确定性浪费的第一个可比较改动，不能标记整个第一阶段完成：

- 普通滚动的 120 ms settle 和 150 ms preload 等待仍在；第一次未命中且没有在途任务时仍同步渲染。
- 深色转换尚未迁入 worker，尚未实现全应用在途像素预算及队列限制。
- 手势仍移动原截图，首次访问内容的空白尚未解决；本次仅停止为无法更新的截图重绘。
- worker 私有 provider 仍按路径复用，本次没有修改文件变更识别及资源保留策略。
- 没有扩大线程池、预加载页数或缓存容量。也没有修改 Benchmark 原生探针、轨迹、统计脚本与质量标准，因此可以与保存的 `68ac33a` 测量包按原协议比较。

## Alternatives considered

| 方案 | 最强理由 | 本次选择 |
| --- | --- | --- |
| 只修 prune 的共享 generation | 修改范围小，能停止误丢整页结果 | 仍有重复提交和主线程重复渲染；一起修正请求所有权，形成可独立验证的行为 |
| paint 中等待已经提交的 future | 立即取得结果，无需临时空白 | 主线程可能等待排队或渲染，影响输入响应；完成后重绘，失败时允许原同步路径恢复 |
| 失效时立即清空在途 key | 新需求可立刻提交 | 旧计算未必停止，会重复渲染且旧回调可能清理新任务身份；保留到 owner 结束后再重查需求 |
| 同时引入有界调度器、预览及新缓存 | 一次处理更多空白场景 | 混合变化难以归因且增加维护成本；这一步仅修改去重、失效与完成处理，下一步单独验证调度 |

## Testing

新增 `PdfRenderLifecycleTests`，使用 QPdfWriter 创建临时 PDF、真实 MuPDF 和真实视口。通过阻塞单个全局 worker，让请求稳定停留在队列中，然后检查重复提交、同步路径、跨 generation、旧结果回调及恢复行为。测试不用固定 sleep 推断任务已完成；事件等待有 10 秒上限，CI 进程有 60 秒上限。

检查包括：同批重复页与多次预加载只提交一次；worker 完成但回调未执行时仍不重复；普通 prune 保留整页结果；失效期间同键请求等待，旧结果不入缓存，随后自动恢复；主题变化生成新主题结果；文档销毁后的旧任务不能污染新文档；joined cancellation 释放身份；多处同源页、区域更新与截图抑制的重绘范围。

Debug 或 Benchmark 桌面构建提供 `--test-pdf-render-lifecycle REPORT.json`。Windows Benchmark workflow 在构建后用 offscreen 平台执行并检查 JSON 和退出码；这是功能验证，不产生性能数字。普通发布构建不包含该测试入口。

本地缺少 Qt/MuPDF，新增原生代码和这些测试**尚未编译运行**。已核对 Qt 官方 QFutureWatcher 的异步取消、完成通知行为：不能把 cancel 当成 worker 已停止。脚本与文档静态验证另行记录于交付结果；必须在后续 Actions 中执行原生功能测试，并以用户采集的 baseline 验证 CPU、页调用数、paint 长尾和空白面积时间。

## Consequences

这一步减少可以明确识别的重复请求、无效丢弃及不相关重绘，并保留正常构建的现有缓存与线程配置。在途列表按当前请求数线性查询，不新增全局锁；仍需要后续有界调度限制快速跨页积累的不同请求。

等待已有后台任务可能比主线程重新渲染更晚显示页面，尤其在共享线程池拥塞时；是否整体改善阅读与输入响应必须实测。由于没有去掉原有等待和截图路径，不能承诺消除滚动空白。

重绘范围依赖当前布局和 source/page 映射。未来手势改为增量合成时，需要重新审视截图期间抑制 update 的条件。源内容身份、DPI、主题失效及 old-owner 清理顺序是后续调度器必须保留的约束。

## Related records

检索活跃 PDF/generation/在途记录后的分类：

- [性能优先提案](../../proposed/architecture/2026-11-27-pdf-performance-first.md)：部分重叠；本篇实现部分请求生命周期规则，整体优化计划仍为 proposed。
- [Benchmark 决策](../testing/2026-11-27-pdf-benchmark.md)：部分重叠；复用原测量协议，baseline 为 `68ac33a`，不重写已有测量定义。
- [初版审查](../../rejected/architecture/2026-11-27-pdf-scroll-rendering.md)：部分重叠；共享 generation 和重复预加载的问题在本篇处理，原审查作为基线事实保留。
