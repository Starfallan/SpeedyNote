# PDF Benchmark

用于比较 `2f65852` 的 PDF 渲染行为与后续优化，覆盖打开文档、普通连续滚动、高倍页内移动、手形拖动、反向回看、跳页及深色模式。使用真实 `DocumentViewport`、MuPDF、缓存、worker 和 Qt 桌面事件循环；没有替换渲染器或修改调度策略。

**当前状态：工具测试通过；用户已运行 Actions 构建的 `68ac33a` 基线程序，但首次 smoke 为 `initial_timeout`，尚无有效 baseline。** Windows runner 的隐藏窗口参数已修正，需用原基线程序重新测量。第一步优化经 `c260c0d` 修正测试编译错误后，x86_64 / ARM64 原生编译已通过；功能测试因原脚本要求未部署的 offscreen 插件而超时，改用包内 Windows 插件后待本机验证。新增开关默认关闭，普通构建不包含测量代码。

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

基线程序为 **`68ac33a`（`2f65852` 的 PDF 渲染行为 + 测量代码）**。`a797b8a` 已包含第一步优化，其构建应作为 candidate，不能替换 baseline。保留基线 exe、依赖和构建记录；基线与候选均使用同一份最新 runner 脚本，不能用没有测量入口的旧 exe 与新入口直接比较。

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

### Windows 首屏超时排查

`initial_timeout` 是失败结果，不能作为性能 baseline。若原始 JSON 中 `frames` 为空、整页/区域 render calls 都为 0、`motion_start_ms` 为 null，说明测量期间没有记录绘制，滚动尚未开始；15 秒表示首屏等待超时，不能解释为 PDF 光栅渲染花了 15 秒。

早期 runner 使用 `windowsHide: true`，Node/libuv 在 Windows 上会为 GUI 窗口设置 `SW_HIDE`，可能导致 Qt 窗口没有 paint。这一启动参数现为 `false`。已有 `68ac33a` 便携包只需用仓库最新 `benchmarks/pdf/bench.mjs` 替换包内同名文件，保留 exe 和 DLL，无需重新编译。然后从便携包根目录使用新的输出目录重试：

```powershell
node benchmarks/pdf/bench.mjs run --exe ./speedynote.exe --suite out/pdf-benchmark/corpus-real/suite.json --out out/pdf-benchmark/smoke-visible --case text-scroll --repeat 1
```

应看到 PDF 窗口自动向下滚动并返回，结束显示 `0 unsuccessful`，结果 `status` 为 `ok` 且有帧与运动时间。若仍超时，保留新结果并检查窗口是否实际显示及有无内容。`text-scroll` 使用生成的文本样本；`corpus-real` 中的真实 PDF 对应 `real-scroll` / `real-pan`。修正 runner 不改变原生探针；后续 baseline/candidate 必须用同一修正版脚本重新采集，以满足 runner 哈希一致的比较条件。

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

第四步处理持续按住手指或手形拖动：加载位置随手势移动，合并更新截图的新露出区域和新完成内容，仍保留即时截图平移。没有提高 PDF 并发、缓存预算或降低最终 DPI。复杂冷页仍可能暂时空白；本步没有新增低清预览。普通拖动和快速翻动应分别衡量。

手势截图覆盖探针已更新，**不能直接用旧 step3 报告做正式比较**。Actions 的 `pdf_benchmark=true` 加 `pdf_pan_reference=true` 会在同一次构建中提供 `speedynote.exe` 候选和 `speedynote-step3.exe` 对照，使用相同新版探针及 DLL。`step3-reference-build.json` 记录对照来源；对照为 `3705003` 应用源码，只有测量代码采用新版。对照编译失败时保留候选包并生成 `step3-reference-error.txt`，不执行 PDF 功能测试。该构建选项仅适用于本步；脚本发现后续应用变化超出恢复范围会拒绝生成错误的对照。

从便携包根目录执行，沿用原 suite 即可对原有十个场景交替 A/B：

```powershell
./benchmarks/pdf/check-render-lifecycle.ps1 -BuildDirectory .
node benchmarks/pdf/bench.mjs run --exe ./speedynote-step3.exe --against ./speedynote.exe --suite out/pdf-benchmark/corpus-real/suite.json --out out/pdf-benchmark/step4-paired --repeat 5
node benchmarks/pdf/bench.mjs compare --baseline out/pdf-benchmark/step4-paired --candidate out/pdf-benchmark/step4-paired --out out/pdf-benchmark/step4-comparison.json
```

针对真实触摸路径，无需重新生成 PDF；从原 suite 派生手势场景：

```powershell
node benchmarks/pdf/pan-suite.mjs out/pdf-benchmark/corpus-real/suite.json out/pdf-benchmark/pan-suite.json
node benchmarks/pdf/bench.mjs run --exe ./speedynote-step3.exe --against ./speedynote.exe --suite out/pdf-benchmark/pan-suite.json --out out/pdf-benchmark/step4-hand-read --case real-hand-read --repeat 5
```

`real-hand-read` 为 400% 缩放下用六秒移动两个视口高度并返回；`real-hand-scroll` 使用普通缩放的相同阅读速度，覆盖跨页；`real-hand-fast` 沿用原 `hand` 的三秒移动本页可移动高度 85% 并返回。三者全程使用 begin/update/endPanGesture。输出目录必须不同；供电、窗口、DPR 和后台负载尽量固定。首次可用 `--repeat 1` 检查，正式比较再交替重复。手势场景以外的十个场景也需要复测，避免只观察空白改善而遗漏 CPU、内存或事件循环长尾变化。

第四步手动功能检查覆盖：按住期间整页和区域完成后进入截图、无新增输入时显示、笔迹与图片层次、主题/隐藏/文档切换、DPR 1/1.25/1.5/2 的往返偏移及透明文本重复绘制。合成发生在 paint 计时内，不能用低 paint 数字隐藏 timer 中的绘制成本；它仍不是物理笔到屏幕延迟测试。

`bf44efb` x86_64 包已在开发机手动执行 371 项功能检查并全部通过；ARM64 运行期、Surface 的等待/CPU/内存/笔输入仍需实测。早期 `822ff29` / `8b62de1` 的手动报告有失败，使用 `bf44efb` 或更新版本执行本节验证。

第二步候选提供滚动中的可见需求调度和任务上限，普通整页 miss 不再同步渲染。其测试标签可用 `--label step2`。原生探针和 runner 未修改，可与已有 baseline / step1 对比；先使用 `--case text-scroll --repeat 1` 检查滚动过程中是否出现内容，再测 `real-scroll` / `real-pan`。单视口的 `sampled_max_pending` 应不超过 1，但这是采样值，完整并发约束由手动功能测试检查。手形截图路径未在第二步修改，不能期待 `vector-hand` 的新区域空白因此消失。

第三步候选复用最多两个 worker provider；每个 provider 仅保留一个热点页 display list，构建期间 MuPDF context 分配限额为 32 MiB。深色图片区域提取和像素转换在 worker 完成，离屏但仍有效的已完成图像可以进入现有缓存供回看使用。并发、光栅和图片缓存预算不增加；32 MiB 不代表整个 provider 或进程内存上限。首次构建有成本，复杂页超额会退回直接渲染，需同时比较首屏、空白、CPU 和峰值内存。手形截图的新区域仍不在此步范围内。

复用此前的真实 PDF suite，不重新生成样本。下载第三步包后，在解压根目录运行：

```powershell
./benchmarks/pdf/check-render-lifecycle.ps1 -BuildDirectory .
node benchmarks/pdf/bench.mjs run --exe ./speedynote.exe --suite out/pdf-benchmark/corpus-real/suite.json --out out/pdf-benchmark/step3 --repeat 5 --label step3
```

`suite.json` 可换为原样本的绝对路径。原生探针和 runner 未改变，优先与 step2 比较 `real-pan`、`vector-pan`、`vector-dark` 的同页移动与回看，再检查 `real-scroll`、`scan-scroll` 和首屏有无退化。单视口 `sampled_max_pending` 仍应不超过 1。下方交替测量命令中的 baseline/candidate 可以分别指向 step2/step3，交替测量比不同时段两次独立测量更能控制后台负载波动。

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

后续候选构建另外包含 PDF 请求生命周期功能测试，覆盖去重、失效、文档切换和局部重绘。该测试由用户下载构建包后按需运行，Actions 不执行它，也不以它的结果限制构建包上传。从便携包根目录调用：

```powershell
./benchmarks/pdf/check-render-lifecycle.ps1 -BuildDirectory .
```

该测试使用便携包已有的 Windows 平台插件，不显示测试窗口、不生成性能数据。脚本启动前检查 `platforms/qwindows.dll`，无需额外部署 offscreen 插件。`68ac33a` 对照包没有此测试入口，不需对旧包运行。候选的 `pdf-render-lifecycle.json` 记录每项检查结果；脚本用非零退出码报告失败。它拒绝覆盖已有报告，重试前应自行保留并移走旧报告。修正后的手动测试尚未在实际设备运行。

第二步增加首屏 miss 的异步检查、连续移动后的最新需求、滚动未停止时完成加载，以及多个视口共享提交额度和释放预留的检查。另覆盖重排页面的缓存保留和区域缓存容量耗尽后不重复渲染。它继续使用原有手动入口。

第三步增加缓存与直接区域渲染的像素比较（72/144/384 DPI、分数坐标、旋转页、CropBox 偏移、照片和透明图形）、指令缓存预算拒绝后的直接渲染、有效完成结果离屏再返回，以及 worker 深色处理的图片保留开关和主题失效。仍只使用手动入口；编译成功不能替代这些功能检查或实际设备性能测量。

内存计数器另有不依赖 Qt/MuPDF 的本地 C++17 检查，不加入 Actions。已有 MinGW 工具链时可在仓库根目录执行（输出目录需已存在）：

```powershell
g++ -std=c++17 -O2 -Wall -Wextra -Werror -pthread source/pdf/PdfCacheAllocatorTests.cpp -o out/pdf-benchmark/pdf-cache-allocator-tests.exe
if ($LASTEXITCODE -ne 0) { throw 'Allocator test build failed' }
./out/pdf-benchmark/pdf-cache-allocator-tests.exe
```

```powershell
node --check benchmarks/pdf/bench.mjs
node --test benchmarks/pdf/bench.test.mjs
```

测试涵盖 PDF xref/压缩流结构与生成确定性、长空白时间积分、回看分段、median/MAD、比较条件、输出不覆盖及失败进程记录。这些测试不能证明 MuPDF 成功读取 PDF，也不能替代原生编译、实际画面检查和 baseline 采集。

相关决策：[性能优先提案](../../docs/decisions/proposed/architecture/2026-11-27-pdf-performance-first.md)、[Benchmark 决策](../../docs/decisions/implemented/testing/2026-11-27-pdf-benchmark.md)、[第三步区域执行复用](../../docs/decisions/implemented/architecture/2026-11-27-pdf-region-work-reuse.md)。
