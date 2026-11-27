# Agent Note: PDF 桌面基准与可复现语料

Status: implemented

## Problem

`2f65852` 修复高倍清晰度后，用户报告普通翻页与高倍移动均有空白等待。现有 F10 `ViewportPerfMonitor` 是短时滚动统计，不记录内容覆盖，且 FPS 计算排除超过 250 ms 的间隔；现有 PDF tile 测试主要验证几何。它们不能说明用户等待改善多少，也不能排除以更高 CPU、内存和输入停顿换取画面的情况。

用户要求按 README 的 Performance First 建立 baseline，并选择固定生成样本加真实 PDF。当前机器没有 Qt/MuPDF 开发环境，用户沿用 GitHub Actions 构建，已在实际 Windows 设备运行 `68ac33a`。首次 smoke 导入 24 页文本 PDF 约 24 ms，但 15 秒内 frames 为空、整页和区域渲染调用均为 0，返回 `initial_timeout`；**尚无有效 baseline**。

## Decision

使用默认关闭的 `SPEEDYNOTE_ENABLE_PDF_BENCHMARK`，在 Release 可执行程序内提供独立入口。实际调用 Document、DocumentViewport、MuPDF、缓存及异步任务，使用真实桌面事件循环；入口早于会话恢复和单实例 IPC，采用临时 QSettings。普通构建不包含探针。测量模式禁止新文档的延迟适合页宽操作，以保留指定缩放；其余 PDF 调度及缓存策略沿用基线。

Node 工具只依赖内置模块，生成文本、合成扫描、复杂矢量、超大页面四类 PDF，记录内容哈希，并可只读登记用户 PDF。测量固定逻辑视口、缩放、运动轨迹；每次启动新进程，记录失败和超时。runner 在 campaign 前校验语料，不清 OS 缓存，因此明确标记为 fresh process、OS cache uncontrolled，不能称为冷盘读取。

内容指标来自真正提交给 QPainter 的 PDF 图像及实际 DPI，手形路径根据捕获画面的覆盖和位移计算。统计分别保留首内容、95% 内容、99% 目标清晰度、空白/不清晰面积时间积分、最终等待。主线程 timer 延迟保留长停顿，另记 paint、CPU、进程内存峰值、光栅次数与像素。原始 trace 有上限，超过上限使样本无效，避免静默截断。

比较按每次运行的指标报告 median、MAD、min/max 与变化幅度，不合并所有帧后重新计算总体 P99，不以五次样本宣称显著性。语料、探针、runner、显示、机器、编译选项或部署 DLL 不一致时拒绝比较。失败结果保持可见；交替 A/B、B/A 执行用于减少顺序偏差。

现有 Windows workflow 的 `pdf_benchmark` 输入生成 x64/ARM64 便携 artifact，保存源版本、CMake 配置和 MSYS2 包版本。测量模式不生成安装器、不上传 Release。GitHub 托管 runner 用于编译及工具验证；桌面体验的 baseline 在固定实际设备采集。

使用方法及指标边界见 [Benchmark README](../../../../benchmarks/pdf/README.md)。

### Windows 启动窗口

runner 显式使用 `windowsHide: false`。Node 的 `spawnSync` 将 `windowsHide: true` 映射为 `UV_PROCESS_WINDOWS_HIDE`；libuv 设置 `STARTF_USESHOWWINDOW` 和 `SW_HIDE`，同时影响 GUI。证据为 [Node spawn_sync.cc](https://github.com/nodejs/node/blob/v24.x/src/spawn_sync.cc) 和 [内置 libuv process.c](https://github.com/nodejs/node/blob/v24.x/deps/uv/src/win/process.c)。[Qt Windows 窗口实现](https://github.com/qt/qtbase/blob/6.12/src/plugins/platforms/windows/qwindowswindow.cpp) 通过 `ShowWindow` 显示窗口，普通激活流程不保证再次显示被隐藏的窗口。

这解释了首次 smoke 没有 paint 的可能机制，但尚未经同一设备重试确认。修正只修改 Node 启动参数，不改变原生探针、超时或质量标准；用户保留原 baseline exe 和 DLL，仅更新脚本后重试。新 baseline 和 candidate 必须统一使用修正版 runner。原生入口的 `isVisible()`/`isMinimized()` 检查不能证明操作系统窗口已 exposed，因此 `initial_timeout` 必须结合帧数、渲染计数和实际窗口观察解读。

## Alternatives considered

| 选项 | 最强理由 | 取舍 |
| --- | --- | --- |
| 只扩展 F10 HUD 或记录平均 FPS | 修改少，可直接观察真实应用 | 短窗口无法保留完整等待，流畅移动截图仍可能大面积空白；保留 HUD，另建有明确协议的基准 |
| 只做 MuPDF 单函数测速 | 噪声较小，易比较光栅成本 | 漏掉 timer、主线程后处理、缓存失效与截图路径；以实际视口为主要基准，provider 计数辅助解释 |
| 在 GitHub hosted runner 生成 UI baseline | CI 自动化方便，无需本机环境 | 显示环境与共享资源不稳定，不能代表低端设备；CI 负责构建，再下载测量 |
| 只测真实 PDF | 更接近用户问题 | 文档可能私有、变化或无法共享；固定语料用于重复对比，真实 PDF 用于代表性验证 |
| 用截图白色比例识别未加载 | 不需渲染探针 | PDF 本身白纸和背景不能区分，截图还有复制成本；使用实际绘制覆盖，并明确其不是物理屏幕呈现或像素正确性验证 |
| 延长首屏超时 | 对真正缓慢的文档可获得完整结果 | 首次失败没有任何 paint 或光栅调用；延时不能纠正启动时隐藏 GUI，保留原超时并修正启动参数 |

## Testing

- `node --check benchmarks/pdf/bench.mjs` 通过。
- `node --test benchmarks/pdf/bench.test.mjs`：8 项通过。验证 PDF xref/stream 结构、固定生成、积分的长间隔与边界、回看分段、统计、不可比条件、文件不覆盖和失败进程记录。
- `package.ps1` 和 Windows workflow 中的 8 个 PowerShell 脚本块经 parser 静态语法检查通过。工作流 YAML 未经 actionlint 验证，需要真正执行后才能证明构建及部署有效。
- 用户的 Actions 基线程序已运行并报告 MuPDF 导入 24 页生成文本样本；首次 smoke 失败，无绘制和滚动数据。修正 runner 后的实际窗口、轨迹与覆盖率仍待同设备重试，完整语料和第一步优化的原生功能测试尚未验证。本地 Node 测试不能替代这些验证。

## Consequences

可以在修改渲染策略之前保存可复现基线，并同时观察等待与资源代价。普通发布构建无新增时钟、计数器或几何统计成本；benchmark 构建自身有成本，提供关闭覆盖统计的校准模式，不能把它与主测量模式混合比较。

覆盖率为逻辑几何近似，取整存在边缘误差，DPI 也不能证明图像内容像素正确。事件循环延迟不是实际笔到屏幕延迟；CPU 不是能耗。Windows private bytes 为采样峰值，进程 peak RSS 由 OS 统计，两者不能互换。实时动作驱动会跳过卡顿期间错过的输入，必须同时报告 missed ticks 与长尾延迟，不能只看空白积分。

工具尚不覆盖书写、缩略图/搜索、多视口并行、移动端及持续发热；这些是优化进入默认发布前的后续实机验证。生成 scan 使用 Flate，不能代表 JPEG2000 解码。新增预览、合成缓存或绘制方式时需要维护 draw hook，否则内容指标会失真；探针改变后需重新测量旧实现。

首轮取得 baseline 后，按[性能优先提案](../../proposed/architecture/2026-11-27-pdf-performance-first.md)逐项减少浪费，再用相同条件验证。不在没有数字时预定收益百分比、预加载页数或新增线程数。

## Related records

- [按住平移实时内容](../bug-fix/2026-11-27-pdf-pan-live-content.md)：新探针跟踪截图增量覆盖，新增正常阅读手势；提供同探针 Step3 对照，禁止与旧探针报告直接正式比较。

本次检索 proposed、implemented、rejected 中的 PDF/benchmark 记录：

- [性能优先提案](../../proposed/architecture/2026-11-27-pdf-performance-first.md)：部分重叠；测量基础设施由本篇承接，优化仍为 proposed。
- [初版审查](../../rejected/architecture/2026-11-27-pdf-scroll-rendering.md)：部分重叠；保留控制流证据及被否的默认预览优先策略，无需迁移状态。
- 未发现其他 benchmark 决策需要替代或归档。
