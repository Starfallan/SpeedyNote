# Agent Note: PDF 候选包交付与手动功能验证

Status: implemented

## Problem

用户要求通过 Actions 构建候选包，下载后自行运行测试。新增的 PDF 生命周期测试被设置为 artifact 上传的前置步骤，超时导致已成功编译的 x86_64 / ARM64 包无法下载。这是未经用户要求增加的交付限制。

首次超时还暴露了测试部署遗漏：脚本指定 offscreen，但 `windeployqt6` 的日志只部署 `qwindows.dll`。Qt 的 Release GUI 在缺少平台插件时可能显示阻塞错误对话框；本次运行没有生成功能测试报告，不能当作功能测试通过或性能退化证据。

## Decision

Windows workflow 删除两个架构的 `Verify PDF render lifecycle` 步骤。构建和部署成功后按既有流程上传候选包；PDF 生命周期测试的运行结果不参与上传条件。包中保留手动脚本和原生测试入口，用户下载后按需运行。

手动脚本使用包内已有的 Windows 平台插件，启动前检查 `platforms/qwindows.dll`。测试不显示视口，保留 60 秒进程超时和非零失败退出码；不增加 offscreen DLL，避免为了功能测试改变 baseline/candidate 的部署库指纹。

已有 Node 工具测试保持独立 job。本文不改变真实设备上的性能测量及比较协议；构建成功不代表功能检查或 benchmark 已通过。

## Alternatives considered

| 方案 | 优点 | 选择理由 |
| --- | --- | --- |
| 修正环境后继续让生命周期测试阻止上传 | 自动拦截并发行为错误 | 不符合用户下载后自行验证的交付要求，取消此安排 |
| 在 CI 非阻断地运行生命周期测试 | 同时保留自动反馈和构建包 | 用户要求自行运行，当前不增加这一步；保留包内入口即可 |
| 为手动测试额外部署 offscreen 插件 | 可沿用原启动参数 | 会改变部署 DLL 指纹且增加测试依赖；现有隐藏视口测试可使用已部署的 Windows 插件 |

## Consequences

用户能够取得编译成功的候选包，自行决定何时进行功能检查与性能测量。Actions 的绿色状态只证明它实际执行的检查；生命周期错误可能在下载后才发现，不能将上传成功解释为功能测试通过。

`c260c0d` 的两种架构已实际编译通过，旧测试脚本均因 60 秒超时失败；修正后的手动测试尚未在设备运行。workflow 删除步骤、PowerShell 语法和文档链接通过静态检查后，仍需新的 Actions 运行验证 artifact 上传。

## Related records

检索活跃 PDF/benchmark 记录后的分类：

- [请求生命周期实现](../bug-fix/2026-11-27-pdf-render-request-lifecycle.md)：部分重叠，本文取代其 CI 自动执行和上传前置要求，渲染实现及功能检查内容继续保留。
- [Benchmark 决策](../testing/2026-11-27-pdf-benchmark.md)：部分重叠，保持 Actions 构建、实际设备测量的分工。
- [性能优先提案](../../proposed/architecture/2026-11-27-pdf-performance-first.md)：部分重叠，功能与性能验证仍需完成，交付包不等于验证通过。
- [初版审查](../../rejected/architecture/2026-11-27-pdf-scroll-rendering.md)：无关，本文不修改渲染方案。
