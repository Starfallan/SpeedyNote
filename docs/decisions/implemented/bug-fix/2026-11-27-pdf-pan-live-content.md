# Agent Note: 按住平移时持续加载并更新画面

Status: implemented

## Problem

Step3 实机中手指持续按住时，新区域始终空白。Pan 只移动开始时截图，真实视口位置直到松手才更新；整页和区域调度均拒绝活动手势，完成回调也跳过截图重绘。`vector-hand` 的空白积分几乎没有改善，普通 `real-pan` 用 setPanOffset，不能代替触摸路径的测量。

## Decision

分页 Pan 的需求位置随手指更新，沿用一视口一个在途任务、共享最多两个任务和原缓存预算。按住期间只加载可见需求，不运行停止后的邻页预取和笔迹缓存维护。

保留 opaque 截图及即时平移。单次 33 ms timer 合并新区域和 PDF 完成通知，不因持续输入延期、不在静止时轮询。更新发生在 paint 内，计入现有 paint 耗时。按物理整数像素原地移动截图，仅重绘露出的区域；新 PDF 结果合并触发当前截图重绘。共用正常画布绘制函数，PDF、对象、笔迹和覆盖层维持原顺序。每次更新后调整截图原点，保留不足一个物理像素的位移，避免往返累计取整误差。33 ms 是限制合成频率的初始值，需实测决定是否调整。

Benchmark 跟踪实际截图更新及其覆盖范围，使用物理像素保存截图覆盖以支持分数 DPR；最终输出仍为逻辑像素覆盖。新探针需对旧实现重新测量才能正式比较。生命周期检查仅保留用户下载后的手动入口，不加入 Actions。

普通窗口绘制依赖不可被对象替换的系统脏区裁剪；离屏截图只有 painter 裁剪。文本框现用 IntersectClip，并调用 QTextDocument 的 documentLayout()->draw，保留外层脏区。Qt 的 drawContents 会内部 ReplaceClip，单改外层 setClipRect 不足以保护透明文字免于重复绘制。Direct 笔迹按当前 painter 裁剪的包围矩形筛选，不为窄条重绘所有可见笔迹。

笔落下前提交当前平移并停止触摸惯性，在确定第一点页坐标前完成；指针按下期间不允许触摸重新启动平移。隐藏、换文档、外部改变位置或缩放会取消旧截图更新。

Actions 提供可选 pdf_pan_reference 参数：候选编译后保存 exe，恢复 3705003 的 viewport、生命周期测试和 TextBoxObject 源码，使用同一新探针和 build 目录增量编译 speedynote-step3.exe，最后恢复候选 exe 和源文件字节。脚本验证两版应用源文件差异只在声明范围内，超出即拒绝。无 PDF 测试步骤；可选对照失败会记录错误但继续上传候选。两版使用相同 DLL、编译参数和探针，旧 step3 报告仍不能直接用于正式比较。

pan-suite.mjs 从已有 suite 派生快速高倍、正常高倍阅读及普通跨页三种手势场景，不改写 PDF 或原 suite。原 hand 轨迹保留；新增阅读轨迹六秒移动两个视口高度再返回，以区分实际阅读和高速压力测试。

快速滑动的低清预览作为后续独立机制评估，本次修复持续拖动被禁止加载的行为。允许临时模糊是合理的体验取舍，但额外生成预览的成本与最终清晰质量须分别测量。

## Alternatives considered

- 每次触摸都全场景重绘：简单且可立即显示结果，但复杂笔迹和对象的成本随输入频率增加，放弃。
- 只将 PDF 画到截图上：合成成本低，却会覆盖笔迹和对象，放弃。
- 周期重新 grab 整屏：易实现，但反复分配全屏位图、重绘未改变的场景，选择复用截图及新露出的区域。
- 保持松手才加载：节省工作，却无法满足正常触摸阅读，放弃。

## Testing

手动原生检查覆盖持续按住时两条 PDF 路径完成并显示、无额外移动时结果合成、笔迹/图片层次、DPR 1/1.25/1.5/2 的往返移动和透明文本、主题失效、隐藏/换文档取消 timer。代码审查检查输入合并、在途上限和 Qt API。Node 检查覆盖派生 suite 的路径、哈希、原样本保持和拒绝覆盖；PowerShell 故障注入检查对照构建成功及失败均恢复候选与源码。没有本地 Qt/MuPDF 开发环境，原生功能及 Surface 性能待用户手动验证；两架构编译结果在交付时单独报告。

下载 `822ff29` 的 Actions 便携包后，本机执行原生检查：363 项中 6 项失败，按住加载、整页/区域交付和笔迹/图片层次通过。Step3 同探针对照的 333 项中两项同样失败。主题测试此前未固定默认深色状态，使“切换深色”实际没有变化；共享调度断言仅接受整页缓存，遗漏了重新关联文档时自动适配后的区域模式。测试现在显式以浅色开始，并接受实际使用的任一缓存。

DPR 1.25/1.5 的截图严格像素比较失败；增量裁剪现在按物理像素设置，并在恢复抗锯齿绘制前不透明清空暴露区域。逻辑 bounds 只用于场景筛选。失败的像素检查保存实际/期望 PNG 和差异统计以便区分真实残影和抗锯齿差异。原生复验结果单独报告，不能用编译通过替代。普通非区域模式的 prune 不再标记整个手势截图脏，否则每次需求维护都会抵消增量绘制。

`8b62de1` 下载复验为 360/363，通过了主题检查，两个分数 DPR 的文字分别仅有 48/21 个边缘像素不同、最大通道差 16，网格和页面一致。检查改为两组独立的严格相等断言：网格长距离往返验证移动与新区域，固定文字变换下重新绘制不相连窄条验证透明文本的裁剪和层次，不为字体的移动栅格化引入像素容差。原差异 PNG 保留作证据。共享调度的记录显示第三个隐藏视口被自动缩放后没有收到 paint，非 worker 失败或积压；该检查改为直接撤回/重新注册等待需求，文档切换仍由专用检查覆盖。

`bf44efb` 的 x86_64 便携包在本机执行最终手动入口，371/371 通过、diagnostics 为空，报告为 `out/pdf-benchmark/step4-final-package-check/pdf-render-lifecycle.json`（不提交机器产物）。同包包含 3705003 + 新探针的 Step3 对照。此前失败报告和 PNG 分别保留在 step4-package-check 与 step4-fixed-package-check；没有移除像素相等断言或设置像素容差。Surface 性能与 ARM64 运行期检查仍未测量，不能从功能通过推断资源或笔延迟没有退化。

## Consequences

新露出的复杂场景仍需合成，CPU 可能比只移动旧截图高；冷 PDF 的执行时间仍可导致暂时空白。不会宣称本次实现无限快翻页或已证明笔输入没有退化。

## Related records

检索现有 PDF 记录：性能优先提案、可见需求调度、Benchmark 与本篇部分重叠，互链并保留各自决定；区域复用、生命周期身份和手动验证约束继续成立；被拒初版无需再修改，没有完全取代或归档的记录。

- [性能优先](../../proposed/architecture/2026-11-27-pdf-performance-first.md)
- [可见需求调度](../../implemented/architecture/2026-11-27-pdf-visible-demand-scheduling.md)
- [区域复用](../../implemented/architecture/2026-11-27-pdf-region-work-reuse.md)
- [Benchmark](../../implemented/testing/2026-11-27-pdf-benchmark.md)
- [手动验证](../../implemented/process/2026-11-27-pdf-benchmark-manual-validation.md)

Qt API 依据：[QPixmap 实现与 scroll 契约](https://github.com/qt/qtbase/blob/dev/src/gui/image/qpixmap.cpp)。移动使用物理像素，QPainter 裁剪使用逻辑坐标；scroll 时不得持有活动 painter。

文本裁剪依据：[QTextDocument::drawContents](https://github.com/qt/qtbase/blob/dev/src/gui/text/qtextdocument.cpp) 与 [QAbstractTextDocumentLayout::PaintContext](https://github.com/qt/qtbase/blob/dev/src/gui/text/qabstracttextdocumentlayout.h)。
