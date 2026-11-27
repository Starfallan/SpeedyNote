# PDF Benchmark

用于比较 `2f65852` 的 PDF 渲染行为与后续优化，覆盖打开文档、普通连续滚动、高倍页内移动、手形拖动、反向回看、跳页及深色模式。使用真实 `DocumentViewport`、MuPDF、缓存、worker 和 Qt 桌面事件循环；没有替换渲染器或修改调度策略。

**当前状态：工具测试通过；原生入口尚未在本地编译或运行，尚无实测 baseline。** GitHub Actions 的构建也需要提交这些改动后实际执行。新增开关默认关闭，普通构建不包含测量代码。

## 通过现有 GitHub Actions 构建

1. 将包含本目录及原生入口的改动提交到测试分支并推送。
2. 打开 **Actions → Build SpeedyNote Windows Installer → Run workflow**，选择该分支，勾选 `pdf_benchmark`。
3. 工作流验证 Node 工具，并使用现有 Qt/MuPDF 构建与 DLL 部署流程生成两个便携包：`speedynote-pdf-benchmark-x86_64`、`speedynote-pdf-benchmark-arm64`。此模式不生成安装包，也不上传 Release。
4. 下载与本机架构一致的 artifact，完整解压至独立目录。保留 exe、所有 DLL、插件、`benchmark-build.json`、`CMakeCache.benchmark.txt`、`msys2-packages.txt` 和 `benchmarks/pdf/`。
5. 在自己的桌面设备上执行下方测量命令。GitHub 托管 runner 只承担构建和工具验证，不作为 UI 性能 baseline；它没有保证一致的交互式显示环境。今后可在固定设备、已登录桌面的 self-hosted runner 上自动采集。

如果网页没有显示新增输入，可在已推送的测试分支中使用 CLI。此命令会实际触发构建：

```powershell
$benchmarkBranch = git branch --show-current
gh workflow run build-windows.yml --ref $benchmarkBranch -f pdf_benchmark=true
```

手动触发的工作流发现规则见 [GitHub 文档](https://docs.github.com/en/actions/reference/workflows-and-actions/events-that-trigger-workflows#workflow_dispatch)。

工作流依赖与当前发布构建一致，但 MSYS2 使用滚动更新。基线与候选应尽量在同一依赖环境下构建。比较工具检查 Qt/MuPDF/编译选项和部署 DLL 的指纹；版本或依赖变化时应重新构建两者，不能把依赖升级的影响计入代码优化收益。

## Windows x64 本地编译环境

与仓库的 Windows CI 一致，使用 **MSYS2 CLANG64 + Qt6 + MuPDF**。不需要为了本基准另外安装 Visual Studio 或 Inno Setup。以下以默认安装位置 `C:\msys64` 为例；ARM64 使用现有 Actions 的 CLANGARM64 包更方便。

1. 从 [MSYS2 官网](https://www.msys2.org/)安装 x64 版本，打开 **MSYS2 CLANG64** 终端。
2. 执行完整更新。若提示关闭终端，重新打开 CLANG64 并再次更新，直到没有待更新项。官方仅支持完整系统升级，见[更新说明](https://www.msys2.org/docs/updating/)。

```sh
pacman -Suy
```

3. 在同一个 MSYS2 终端安装项目现有 CI 使用的包：

```sh
pacman -S --needed mingw-w64-clang-x86_64-clang mingw-w64-clang-x86_64-lld mingw-w64-clang-x86_64-make mingw-w64-clang-x86_64-cmake mingw-w64-clang-x86_64-pkgconf mingw-w64-clang-x86_64-cppwinrt mingw-w64-clang-x86_64-qt6-base mingw-w64-clang-x86_64-qt6-tools mingw-w64-clang-x86_64-qt6-svg mingw-w64-clang-x86_64-qt6-translations mingw-w64-clang-x86_64-mupdf mingw-w64-clang-x86_64-ntldd
```

4. 回到 **PowerShell 7**，在仓库根目录执行。所有构建产物写入独立的 `out/pdf-benchmark/build-baseline`；不要混用系统里其他 MinGW/LLVM。`compile.ps1` 默认清理 `build/`，此处直接调用 CMake，以便保存基线。

```powershell
$benchmarkToolchain = 'C:/msys64/clang64'
$env:Path = "$benchmarkToolchain/bin;$env:Path"
$benchmarkTranslations = @(Get-ChildItem resources/translations/app_*.ts | ForEach-Object FullName)
& "$benchmarkToolchain/bin/lrelease-qt6.exe" @benchmarkTranslations
if ($LASTEXITCODE -ne 0) { throw 'Translation build failed' }

cmake -S . -B out/pdf-benchmark/build-baseline -G 'MinGW Makefiles' `
  "-DCMAKE_C_COMPILER=$benchmarkToolchain/bin/clang.exe" `
  "-DCMAKE_CXX_COMPILER=$benchmarkToolchain/bin/clang++.exe" `
  "-DCMAKE_MAKE_PROGRAM=$benchmarkToolchain/bin/mingw32-make.exe" `
  "-DQT_PATH=$benchmarkToolchain" -DCMAKE_BUILD_TYPE=Release `
  -DENABLE_DEBUG_OUTPUT=OFF -DENABLE_SANITIZERS=OFF `
  -DSPEEDYNOTE_ENABLE_PDF_BENCHMARK=ON
if ($LASTEXITCODE -ne 0) { throw 'Configure failed' }
cmake --build out/pdf-benchmark/build-baseline --config Release --parallel 2
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
```

在保留上述 PATH 的终端运行时，程序使用该工具链的 Qt/MuPDF DLL。另开终端后需重新设置 PATH。离开此环境分发时须部署全部依赖，推荐下载 Actions 便携包；只复制 exe 不够。直接使用 PATH 的本地构建不具备便携包的完整 DLL 指纹，比较前必须人工核对外部 DLL 未变化。

## 生成固定样本

工具仅使用 Node 内置模块，无 npm 依赖。使用 Node 22 或 24；本次本地验证为 Node 24。下面命令从仓库根目录或 artifact 解压根目录执行。

```powershell
node benchmarks/pdf/bench.mjs generate --out out/pdf-benchmark/corpus
```

也可以使用新的输出目录，同时登记一份真实 PDF：

```powershell
node benchmarks/pdf/bench.mjs generate --out out/pdf-benchmark/corpus-real --pdf 'D:/Documents/slow.pdf'
```

真实 PDF 仅记录绝对路径与 SHA-256，不复制、不修改、不上传。测量期间不能更改它。移动到另一台设备时需手动调整 suite 中的真实路径，并保留相同文件字节。生成和测量均拒绝覆盖已有输出目录。

| 样本 | 固定内容 | 覆盖范围 |
| --- | --- | --- |
| text | 24 页 Helvetica 文本 | 普通页、跨页、回看 |
| scan | 12 页 1240×1754 RGB 图像，Flate 压缩 | 图像解码、深色扫描、跳页 |
| vector | 8 页，每页 7000 条路径 | 复杂矢量、高倍移动、手形截图、深色处理 |
| giant | 3 页 2384×3370 pt 矢量 | 超大页面的区域渲染 |

生成器固定随机种子，没有日期或系统字体依赖。suite 保存生成器、Node/zlib 版本和每份 PDF 的哈希；跨版本压缩结果可能不同，后续比较应复用**同一份 corpus**。scan 是合成 Flate 图像，不代表 JPEG/JPEG2000；真实扫描件、复杂字体、透明叠加仍需真实 PDF。

## 采集 baseline

基线为 **`2f65852` + 相同测量代码**。本次没有实现 PDF 性能优化。保留构建所对应的源版本及 diff；不能用没有测量入口的旧 exe 与新入口直接比较。

Actions 下载包中的 exe 在根目录。首次只测一个 case，人工确认 PDF 能打开、固定缩放正确、向下及回看轨迹有效，再采集完整 suite：

```powershell
node benchmarks/pdf/bench.mjs run --exe ./speedynote.exe --suite out/pdf-benchmark/corpus/suite.json --out out/pdf-benchmark/smoke --case text-scroll --repeat 1
node benchmarks/pdf/bench.mjs run --exe ./speedynote.exe --suite out/pdf-benchmark/corpus/suite.json --out out/pdf-benchmark/baseline --repeat 5 --label baseline
```

本地 CMake 构建将 `--exe` 换为 `out/pdf-benchmark/build-baseline/speedynote.exe`。通过 `--case vector-pan` 等可以逐个测量。测量期间保持窗口可见、屏幕解锁、不拖动或操作窗口，固定供电、性能模式、显示器、缩放和其他负载。隐藏/最小化、窗口大小变化、DPR/缩放变化会使结果无效；遮挡和其他软件负载不能可靠自动检测，需要人工控制。

每次样本启动新进程，且使用临时 QSettings，不读取用户的应用设置或恢复笔记会话。计时从创建 PDF Document 开始，**不含 exe 启动和 Launcher 初始化**。runner 在一次 campaign 开始时读取文件校验哈希，因此这不是 OS 文件缓存冷启动测量；不会清空系统缓存。回看是重新访问相同区域，不能保证所有缓存都命中。

固定视口为 1024×640 **逻辑像素**。scroll/jump 为 100%，pan/hand 为 400%；DPR 决定实际物理像素。普通路径是 `setPanOffset` 对应的连续滚动，不是硬件 wheel 事件注入；Qt5 离散滚轮路径、触屏输入分发另行验证。

- scroll：第一页到第 11 页（不足则最后一页），3 秒向下、停留 0.5 秒、3 秒返回。
- pan：同页向下移动可移动高度的 85%，时序同上。
- hand：调用真实手形手势方法，3 秒前进、3 秒返回，不在中途停留，以免触发手势超时。
- jump：从首页跳到第 11 页（不足则最后一页），等待目标内容绘制。

首屏达到目标清晰度且 PDF 任务空闲后再等待 0.5 秒，开始轨迹。结束后等待清晰绘制及 PDF 任务、相关 timer 完成。初始等待和最终等待有超时，进程还受 60 秒外部超时限制。驱动使用实际时间推进；停顿后的过期输入直接跳过，避免用补发输入掩盖卡顿。

## 指标如何解释

| 指标 | 含义与边界 |
| --- | --- |
| first_content_ms | 首次绘制超过可见 PDF 面积 1% 的时间；不是完整可读时间 |
| first_readable_ms | 首次至少 95% 可见 PDF 面积有内容；可以是低清图 |
| first_sharp_ms | 首次至少 99% 可见 PDF 面积达到屏幕要求的 DPI；DPI 容差 1% |
| unpainted_area_ms | 从运动开始到任务结束，`未绘制 PDF 面积比例 × 持续毫秒` 的积分；1000 表示等价整屏空白 1 秒 |
| unsharp_area_ms | 未达到目标 DPI 的面积时间积分；包括空白 |
| forward/return_unpainted_area_ms | 分别统计前进与回看，不将中间停留合并到回看 |
| final_sharp_ms / final_settle_ms | 运动结束至清晰画面 / 至清晰且 PDF 任务与相关 timer 空闲 |
| event_loop_p95/p99/max_ms | 16 ms 主线程 timer 的超时延迟，包含导入和长于 250 ms 的阻塞；同时保存 missed_driver_ticks |
| paint_p95/p99_ms | 每次真实 paint 的同步耗时；不包含屏幕扫描输出时间 |
| cpu_ms | 测量全过程的进程 user+kernel CPU 时间，包括多个线程、初始静止与最终等待 |
| process_peak_rss_bytes | OS 提供的进程生存期峰值驻留内存；可能早于开始计时，包含 Qt 等全部成本 |
| sampled_private_peak_bytes | Windows 每 250 ms 采样及结束时的 private bytes 最大值；可能漏过短峰值，其他平台为 null |
| whole/region_render_calls、raster_pixels | MuPDF 整页/区域调用数与成功输出像素，含同步和后台调用 |
| raster_call_ms | provider 调用耗时之和，含等待锁、解释、渲染和图像复制；不是纯 CPU，也不等同于用户等待。方法外 trimStore 不在其中 |
| sampled_max_pending | 定时采样时观察到的 PDF watcher 数，包含等待及执行，可能漏掉瞬时峰值 |
| probe_p95_ms | paint 后几何统计的耗时；draw hook 的开销已在 paint 内，不能据此声称全部探针成本为零 |

覆盖率观察**实际提交给 QPainter 的 PDF 图像矩形**，并将手势截图按真实位移计算；排除页间空隙与桌面背景，避免把原文中的白纸误判为未加载。它是逻辑像素级绘制覆盖代理指标，不是截图 OCR、像素正确性验证或物理屏幕呈现测量。长时间没有新 paint 时，积分保留上一幅已绘制画面：已清晰但冻结的画面由主线程延迟指标反映，不虚报为空白。

原始帧、事件循环延迟、阶段时间、进程退出码和失败记录都保留在 JSON 中。失败样本不会被悄悄排除后计算“成功平均值”。无区域可测时 coverage 时间单独报告。目标设备若因 DPI/像素预算无法达到屏幕要求，可能得到 timeout，应查看原始数据，不能降低质量标准来换取成功结果。

这版尚未覆盖：真实笔到屏幕延迟、注释书写与 PDF 同时加载、缩略图/搜索并行、多视口、移动端、长时间发热/功耗，以及缓存内各类资源字节分解。README 的 360Hz 笔输入目标不能由这个 16 ms 心跳测试证明。

## 比较候选改动

保留 baseline 完整目录；candidate 放在另一个目录，使用相同语料与测量代码。在同一设备、同一时段交替测量 A/B、B/A，减少运行顺序造成的偏差：

```powershell
node benchmarks/pdf/bench.mjs run --exe 'D:/Bench/baseline/speedynote.exe' --against 'D:/Bench/candidate/speedynote.exe' --suite out/pdf-benchmark/corpus/suite.json --out out/pdf-benchmark/paired --repeat 5
node benchmarks/pdf/bench.mjs compare --baseline out/pdf-benchmark/paired --candidate out/pdf-benchmark/paired --out out/pdf-benchmark/comparison.json
```

也支持分别测量：candidate 的 `run` 加 `--label candidate`，然后 compare 指向两个结果目录。每个 case 报告各次运行指标的 median、MAD、min/max 和变化百分比，不合并所有帧后重新算一个总体 P99；基线为零时百分比为 null。

5 次用于探索；有改善迹象后增加到 10 次以上，并跨时段确认。MAD 接近提示只描述观测波动，不是统计显著性检验。不要在没有噪声数据时设定一个通用的“快 5% 即通过”标准。必须同时看内容等待、清晰度、主线程长尾、CPU、峰值内存，以及失败数。

可以给 `run` 加 `--no-coverage` 关闭几何统计，评估测量本身对 CPU/paint 的扰动；此模式缺少内容指标，开始/结束条件也不同，只用于校准。正常比较工具拒绝将其与 coverage 开启的结果混合。

比较工具还要求语料、场景、主题、DPR、视口、硬件、库版本、编译选项、原生探针源码、runner 脚本及已部署 DLL 指纹一致。exe 哈希允许不同，并保存在结果中。未来增加预览或改变手势合成时，必须在真正绘制处报告真实 DPI 和覆盖区域；探针算法发生改变后，需要用同一新探针对基线重新测量。

## 验证工具

```powershell
node --check benchmarks/pdf/bench.mjs
node --test benchmarks/pdf/bench.test.mjs
```

测试涵盖 PDF xref/压缩流结构与生成确定性、长空白时间积分、回看分段、median/MAD、比较条件、输出不覆盖及失败进程记录。这些测试不能证明 MuPDF 成功读取 PDF，也不能替代原生编译、实际画面检查和 baseline 采集。

相关决策：[性能优先提案](../../docs/decisions/proposed/architecture/2026-11-27-pdf-performance-first.md)、[Benchmark 决策](../../docs/decisions/implemented/testing/2026-11-27-pdf-benchmark.md)。
