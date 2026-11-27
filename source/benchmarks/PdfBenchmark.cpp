#include "PdfBenchmark.h"
#include "PdfBenchmarkSupport.h"
#include "../core/DocumentViewport.h"
#include "../core/Document.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPaintEvent>
#include <QRegion>
#include <QSaveFile>
#include <QScreen>
#include <QSettings>
#include <QSysInfo>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QTransform>
#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif
#ifdef SPEEDYNOTE_MUPDF_EXPORT
#include <mupdf/fitz/version.h>
#endif

namespace {
double area(const QRegion& region) {
    double total = 0;
    for (const QRect& r : region) total += double(r.width()) * r.height();
    return total;
}
double percentile(std::vector<double> values, double p) {
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end());
    return values[std::max<std::size_t>(1, std::size_t(std::ceil(p * values.size()))) - 1];
}
double cpuMs() {
#ifdef Q_OS_WIN
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return -1;
    const auto ticks = [](FILETIME t) { return (quint64(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
    return double(ticks(kernel) + ticks(user)) / 10000.0;
#else
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) return -1;
    return (usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) * 1000.0
        + (usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1000.0;
#endif
}
struct Memory { double peakRss = -1, privateBytes = -1; };
Memory memory() {
#ifdef Q_OS_WIN
    using Query = BOOL (WINAPI*)(HANDLE, PPROCESS_MEMORY_COUNTERS, DWORD);
    static const auto query = reinterpret_cast<Query>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "K32GetProcessMemoryInfo"));
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (query && query(GetCurrentProcess(), reinterpret_cast<PPROCESS_MEMORY_COUNTERS>(&counters), sizeof(counters)))
        return {double(counters.PeakWorkingSetSize), double(counters.PrivateUsage)};
#else
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
#ifdef Q_OS_MACOS
        return {double(usage.ru_maxrss), -1};
#else
        return {double(usage.ru_maxrss) * 1024.0, -1};
#endif
    }
#endif
    return {};
}
QJsonValue optional(double number) {
    return number < 0 ? QJsonValue(QJsonValue::Null) : QJsonValue(number);
}
}

// Reads existing view geometry but observes PDF draws, not cache availability.
// No Qt::Test, synthetic input device or forced repaint is involved.
class PdfBenchmarkViewport final : public DocumentViewport, public PdfBenchmark::PaintSink {
public:
    struct Frame {
        double at, paint, probe, missing, unsharp, pdfArea;
        int phase;
    };
    QElapsedTimer clock;
    std::vector<Frame> frames;
    int phase = 0;
    bool recording = false, capturing = false, sawHidden = false, measureCoverage = true;
    bool traceOverflow = false;
    QRegion content, sharp, snapshotContent, snapshotSharp, drawn, drawnSharp;
    QRegion frameDirty, frameDrawn, frameSharp;
    bool updatingFrame = false;
    qreal frameDpr = 1;
    QPointF lastPan;
    qreal lastZoom = 0;
    double lastFrameAt = -1, firstContentAt = -1, firstReadableAt = -1, firstSharpAt = -1;
    double missing = 1, unsharp = 1;

    PdfBenchmarkViewport() {
        frames.reserve(8192);
        m_pdfBenchmarkSink = this;
    }
    ~PdfBenchmarkViewport() override { m_pdfBenchmarkSink = nullptr; }
    double now() const { return clock.nsecsElapsed() / 1e6; }
    QPointF position() const { return m_panOffset; }
    bool idle() const {
        return !isMeasuredPanActive() && m_activePdfWatchers.isEmpty()
            && !m_pdfPreloadTimer->isActive() && !m_scrollSettleTimer->isActive()
            && !m_focusRebuildTimer->isActive();
    }
    int pending() const { return m_activePdfWatchers.size(); }
    void beginMeasuredPan() {
        capturing = true;
        beginPanGesture();
        capturing = false;
        frameDpr = m_gesture.frameDevicePixelRatio;
        const QTransform physical = QTransform::fromScale(frameDpr, frameDpr);
        snapshotContent = physical.map(content);
        snapshotSharp = physical.map(sharp);
    }
    void panTo(const QPointF& pos) { updatePanGesture(pos - m_gesture.targetPan); }
    void endMeasuredPan() { endPanGesture(); }
    bool isMeasuredPanActive() const { return m_gesture.activeType == ViewportGestureState::Pan; }
    qreal zoom() const { return m_zoomLevel; }
    void setInitialView(qreal zoom) {
        setZoomLevel(zoom);
        // Mixed-size PDFs center narrow pages within the widest page. Align
        // with that common center so the trace never follows an empty margin.
        m_panOffset = pagePosition(0) + QPointF(
            (m_document->pageSizeAt(0).width() - width() / zoom) / 2.0, 0);
        clampPanOffset();
        refreshPdfTiledMode();
        update();
    }
    void pdfDraw(const QRectF& rect, qreal dpi) override {
        if (!recording || !measureCoverage) return;
        if (updatingFrame) {
            const QTransform physical = QTransform::fromScale(frameDpr, frameDpr);
            const QRegion r(physical.mapRect(rect).toAlignedRect());
            frameDrawn += r;
            if (dpi >= requiredPdfDpi() * 0.99) frameSharp += r;
            return;
        }
        const QRegion r(rect.toAlignedRect().intersected(this->rect()));
        drawn += r;
        // Target is screen demand, never silently reduced to the cache's DPI.
        if (dpi >= requiredPdfDpi() * 0.99) drawnSharp += r;
    }
    void panFrameBegin(const QPoint& shift, const QRegion& dirty, qreal dpr) override {
        if (!recording || !measureCoverage) return;
        updatingFrame = true;
        frameDpr = dpr;
        const QRegion bounds(m_gesture.cachedFrame.rect());
        snapshotContent = snapshotContent.translated(shift) & bounds;
        snapshotSharp = snapshotSharp.translated(shift) & bounds;
        frameDirty = dirty & bounds;
        frameDrawn = frameSharp = QRegion();
    }
    void panFrameEnd() override {
        if (!updatingFrame) return;
        snapshotContent = (snapshotContent - frameDirty) + (frameDrawn & frameDirty);
        snapshotSharp = (snapshotSharp - frameDirty) + (frameSharp & frameDirty);
        updatingFrame = false;
    }
    QRegion visiblePdfArea(const QPointF& pan, qreal zoom) {
        QRegion result;
        if (!m_document || !(zoom > 0)) return result;
        ensurePageLayoutCache();
        const QRectF view(pan, QSizeF(width() / zoom, height() / zoom));
        // Imported PDF only: every notebook page is PDF-backed. Query metadata
        // and the existing layout index; never load off-screen page content.
        const int columns = m_layoutMode == LayoutMode::TwoColumn ? 2 : 1;
        auto it = std::upper_bound(m_pageYCache.cbegin(), m_pageYCache.cend(), view.top());
        int first = std::max(0, int(it - m_pageYCache.cbegin()) - 1);
        first = (first / columns) * columns;
        for (int i = first; i < m_document->pageCount(); ++i) {
            if (m_pageYCache[i] > view.bottom()) break;
            const QRectF page(pagePosition(i), m_document->pageSizeAt(i));
            const QRectF visible = page.intersected(view);
            if (!visible.isEmpty()) {
                const QRectF screen((visible.topLeft() - pan) * zoom, visible.size() * zoom);
                result += screen.toAlignedRect().intersected(rect());
            }
        }
        return result;
    }
protected:
    void paintEvent(QPaintEvent* event) override {
        drawn = QRegion();
        drawnSharp = QRegion();
        const double before = recording ? now() : 0;
        DocumentViewport::paintEvent(event);
        if (!recording) return;
        const double after = now();
        const bool gesture = m_gesture.activeType == ViewportGestureState::Pan
            && !m_gesture.cachedFrame.isNull() && !capturing;
        const QPointF pan = gesture ? m_gesture.targetPan : m_panOffset;
        if (measureCoverage) {
            if (gesture) {
                const QPointF delta = (m_gesture.targetPan - m_gesture.startPan) * -m_gesture.startZoom;
                const QTransform logical = QTransform::fromScale(1 / frameDpr, 1 / frameDpr);
                const QPoint shift(qRound(delta.x() * frameDpr), qRound(delta.y() * frameDpr));
                content = logical.map(snapshotContent.translated(shift));
                sharp = logical.map(snapshotSharp.translated(shift));
            } else {
                if (pan != lastPan || !qFuzzyCompare(lastZoom, m_zoomLevel)) {
                    content = QRegion();
                    sharp = QRegion();
                }
                // Preserve previously painted coverage outside a partial update.
                content = (content - event->region()) + (drawn & event->region());
                sharp = (sharp - event->region()) + (drawnSharp & event->region());
            }
        }
        lastPan = pan;
        lastZoom = m_zoomLevel;
        if (capturing) return; // Snapshot capture is work, but not a presented frame.
        const QRegion expected = measureCoverage ? visiblePdfArea(pan, m_zoomLevel) : QRegion();
        const double visible = area(expected);
        missing = visible > 0 ? std::clamp(1.0 - area(content & expected) / visible, 0.0, 1.0) : 1;
        unsharp = visible > 0 ? std::clamp(1.0 - area(sharp & expected) / visible, 0.0, 1.0) : 1;
        lastFrameAt = after;
        if (visible > 0 && missing < 0.99 && firstContentAt < 0) firstContentAt = after;
        if (visible > 0 && missing <= 0.05 && firstReadableAt < 0) firstReadableAt = after;
        if (visible > 0 && unsharp <= 0.01 && firstSharpAt < 0) firstSharpAt = after;
        if (frames.size() < 100000)
            frames.push_back({after, after - before, now() - after, missing, unsharp, visible, phase});
        else traceOverflow = true;
    }
};

namespace PdfBenchmark {
int run(QApplication& app) {
    const auto args = app.arguments();
    auto value = [&args](const QString& key, const QString& fallback = QString()) {
        const int i = args.indexOf(key);
        return i >= 0 && i + 1 < args.size() ? args[i + 1] : fallback;
    };
    const QString input = QFileInfo(value("--benchmark-pdf")).absoluteFilePath();
    const QString output = value("--benchmark-output");
    const QString scenario = value("--benchmark-scenario", "scroll");
    const bool dark = args.contains("--benchmark-dark");
    const bool coverage = !args.contains("--benchmark-no-coverage");
    const bool hand = scenario == "hand" || scenario == "hand-read" || scenario == "hand-scroll";
    const bool reading = scenario == "hand-read" || scenario == "hand-scroll";
    const double travelMs = reading ? 6000 : 3000;
    const bool high = scenario == "pan" || scenario == "hand" || scenario == "hand-read";
    if (output.isEmpty() || QFileInfo::exists(output) || !QFileInfo(input).isFile()
        || !QStringList{"scroll", "pan", "hand", "jump", "hand-read", "hand-scroll"}.contains(scenario)) {
        qWarning("PDF benchmark: invalid input/scenario or output already exists");
        return 2;
    }
    if (QGuiApplication::platformName() == "offscreen" || QGuiApplication::platformName() == "minimal") {
        qWarning("PDF benchmark requires a visible desktop window");
        return 2;
    }
#ifndef NDEBUG
    qWarning("PDF benchmark requires an optimized Release build");
    return 2;
#endif
    if (QStringLiteral(SPEEDYNOTE_PDF_BENCHMARK_CONFIG) != QStringLiteral("Release")) {
        qWarning("PDF benchmark requires the Release configuration");
        return 2;
    }
    // Do not hash/read the entire PDF before measuring import: that would warm
    // the OS cache. Corpus hashing happens in the separate preparation tool.
    QTemporaryDir settings;
    if (!settings.isValid()) return 2;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settings.path());

    std::unique_ptr<Document> document;
    PdfBenchmarkViewport viewport; // Destroyed before its Document.
    viewport.measureCoverage = coverage;
    viewport.resize(1024, 640);
    viewport.setWindowTitle("SpeedyNote PDF benchmark — keep this window visible");
    viewport.setAutoLayoutEnabled(false);
    viewport.setLayoutMode(LayoutMode::SingleColumn);
    viewport.setDarkMode(dark);
    viewport.setPdfDarkModeEnabled(dark);
    viewport.show();
    viewport.raise();
    viewport.activateWindow();

    std::vector<double> late;
    late.reserve(8192);
    double startCpu = -1, importMs = -1, motionStart = -1, motionEnd = -1;
    double initialReady = -1, finalReady = -1, nextTick = 16, lastMemoryAt = -1000;
    double peakRss = -1, privatePeak = -1;
    int maxPending = 0, missedTicks = 0;
    bool finished = false, reversed = false, paused = false, handEnded = false;
    const qreal initialDpr = viewport.devicePixelRatioF();
    QPointF origin, destination;
    QJsonArray phases;
    QTimer timer;
    timer.setTimerType(Qt::PreciseTimer);
    timer.setInterval(16);

    auto finish = [&](const QString& status) {
        if (finished) return;
        finished = true;
        timer.stop();
        const double end = viewport.now();
        const double endCpu = cpuMs();
        measuring.store(false);
        viewport.recording = false;
        const Memory mem = memory();
        peakRss = std::max(peakRss, mem.peakRss);
        privatePeak = std::max(privatePeak, mem.privateBytes);
        std::vector<double> paints, probes;
        QJsonArray frames;
        for (const auto& f : viewport.frames) {
            paints.push_back(f.paint);
            probes.push_back(f.probe);
            frames.append(QJsonArray{f.at, f.paint, f.missing, f.unsharp, f.pdfArea, f.phase, f.probe});
        }
        const QString resultStatus = viewport.traceOverflow ? "trace_overflow"
            : viewport.sawHidden ? "invalid_window_hidden" : status;
        QJsonObject environment{
            {"os", QSysInfo::prettyProductName()}, {"kernel", QSysInfo::kernelVersion()},
            {"architecture", QSysInfo::currentCpuArchitecture()}, {"qt", qVersion()},
            {"build_options_sha256", SPEEDYNOTE_PDF_BENCHMARK_BUILD_HASH},
            {"configuration", SPEEDYNOTE_PDF_BENCHMARK_CONFIG},
            {"platform", QGuiApplication::platformName()}, {"logical_width", viewport.width()},
            {"logical_height", viewport.height()}, {"dpr", viewport.devicePixelRatioF()},
            {"ideal_threads", QThread::idealThreadCount()},
            {"refresh_hz", viewport.screen() ? viewport.screen()->refreshRate() : 0.0}
        };
#ifdef FZ_VERSION
        environment.insert("mupdf", FZ_VERSION);
#endif
#ifdef __VERSION__
        environment.insert("compiler", __VERSION__);
#endif
        QJsonObject metrics{
            {"import_ms", optional(importMs)},
            {"first_content_ms", optional(viewport.firstContentAt)},
            {"first_readable_ms", optional(viewport.firstReadableAt)},
            {"first_sharp_ms", optional(viewport.firstSharpAt)},
            {"final_sharp_ms", optional(finalReady >= 0 ? finalReady - motionEnd : -1)},
            {"final_settle_ms", optional(status == "ok" ? end - motionEnd : -1)},
            {"elapsed_ms", end}, {"cpu_ms", optional(startCpu >= 0 && endCpu >= 0 ? endCpu - startCpu : -1)},
            {"paint_p95_ms", percentile(paints, .95)}, {"paint_p99_ms", percentile(paints, .99)},
            {"event_loop_p95_ms", percentile(late, .95)}, {"event_loop_p99_ms", percentile(late, .99)},
            {"event_loop_max_ms", percentile(late, 1.0)},
            {"probe_p95_ms", percentile(probes, .95)},
            {"process_peak_rss_bytes", optional(peakRss)},
            {"sampled_private_peak_bytes", optional(privatePeak)},
            {"whole_render_calls", double(wholeCalls.load())}, {"region_render_calls", double(regionCalls.load())},
            {"render_failures", double(failures.load())}, {"raster_pixels", double(rasterPixels.load())},
            {"raster_call_ms", rasterNanoseconds.load() / 1e6},
            {"sampled_max_pending", maxPending}, {"missed_driver_ticks", missedTicks}
        };
        QJsonArray lateness;
        for (double delay : late) lateness.append(delay);
        QJsonObject result{
            {"schema", 1}, {"protocol", "speedynote-pdf-v1"}, {"status", resultStatus},
            {"instrumentation", SPEEDYNOTE_PDF_BENCHMARK_HASH},
            {"scenario", scenario}, {"dark", dark}, {"zoom", high ? 4.0 : 1.0},
            {"coverage_enabled", coverage}, {"input", input}, {"cache_state", "fresh_process_os_uncontrolled"},
            {"page_count", document ? document->pageCount() : 0}, {"environment", environment},
            {"metrics", metrics}, {"motion_start_ms", optional(motionStart)},
            {"motion_end_ms", optional(motionEnd)}, {"measurement_end_ms", end},
            {"frame_columns", QJsonArray{"at_ms", "paint_ms", "missing_fraction", "unsharp_fraction", "pdf_area_logical_px", "phase", "probe_ms"}},
            {"frames", frames}, {"phases", phases}, {"event_loop_lateness_ms", lateness}
        };
        QSaveFile file(output);
        bool saved = file.open(QIODevice::WriteOnly);
        if (saved) {
            const QByteArray bytes = QJsonDocument(result).toJson(QJsonDocument::Indented);
            saved = file.write(bytes) == bytes.size() && file.commit();
        }
        app.exit(saved && resultStatus == "ok" ? 0 : 2);
    };
    QObject::connect(&timer, &QTimer::timeout, &viewport, [&] {
        if (finished) return;
        const double now = viewport.now();
        const double delay = std::max(0.0, now - nextTick);
        late.push_back(delay);
        const int missed = int(delay / 16.0);
        missedTicks += missed;
        nextTick = now + 16.0;
        viewport.sawHidden |= !viewport.isVisible() || viewport.isMinimized();
        if (viewport.width() != 1024 || viewport.height() != 640) {
            finish("invalid_viewport_size"); return;
        }
        if (!qFuzzyCompare(viewport.devicePixelRatioF(), initialDpr)
            || !qFuzzyCompare(viewport.zoom(), high ? 4.0 : 1.0)) {
            finish("invalid_view_transform"); return;
        }
        maxPending = std::max(maxPending, viewport.pending());
        if (now - lastMemoryAt >= 250) {
            const Memory mem = memory();
            peakRss = std::max(peakRss, mem.peakRss);
            privatePeak = std::max(privatePeak, mem.privateBytes);
            lastMemoryAt = now;
        }
        if (motionStart < 0) {
            const bool ready = coverage ? viewport.firstSharpAt >= 0 : viewport.lastFrameAt >= 0 && viewport.idle();
            if (!ready) { if (now > 15000) finish("initial_timeout"); return; }
            if (initialReady < 0) initialReady = now;
            if (now - initialReady < 500 || !viewport.idle()) {
                if (now > 20000) finish("initial_idle_timeout");
                return;
            }
            origin = viewport.position();
            if (reading) {
                // Two viewport heights in six seconds models continuous reading,
                // distinct from the existing fast movement pressure scenario.
                destination = origin + QPointF(0, 2.0 * viewport.height() / viewport.zoom());
            } else if (high) {
                const double travel = std::max(0.0, document->pageSizeAt(0).height() - viewport.height() / 4.0);
                destination = origin + QPointF(0, travel * .85);
                if (travel <= 0) { finish("invalid_pan_extent"); return; }
            } else {
                if (document->pageCount() < 2) { finish("invalid_page_count"); return; }
                destination = viewport.pagePosition(std::min(10, document->pageCount() - 1));
                destination.setX(origin.x());
            }
            motionStart = now;
            viewport.phase = 1;
            phases.append(QJsonObject{{"name", "forward"}, {"at_ms", now}});
            if (hand) viewport.beginMeasuredPan();
            if (scenario == "jump") {
                viewport.setPanOffset(destination);
                motionEnd = now;
                viewport.phase = 4;
                phases.append(QJsonObject{{"name", "settle"}, {"at_ms", now}});
            }
            return;
        }
        const double elapsed = now - motionStart;
        if (hand && !handEnded && !viewport.isMeasuredPanActive()) {
            finish("gesture_interrupted"); return;
        }
        if (scenario != "jump" && elapsed < travelMs) {
            const QPointF position = origin + (destination - origin) * (elapsed / travelMs);
            if (hand) viewport.panTo(position); else viewport.setPanOffset(position);
        } else if (scenario != "jump" && !hand && elapsed < travelMs + 500) {
            if (!paused) {
                paused = true;
                viewport.setPanOffset(destination);
                viewport.phase = 3;
                phases.append(QJsonObject{{"name", "pause"}, {"at_ms", now}});
            }
        } else if (scenario != "jump" && elapsed < travelMs * 2 + (hand ? 0 : 500)) {
            if (!reversed) {
                if (hand) viewport.panTo(destination); else viewport.setPanOffset(destination);
                reversed = true;
                viewport.phase = 2;
                phases.append(QJsonObject{{"name", "return"}, {"at_ms", now}});
            }
            const double reverseStart = travelMs + (hand ? 0 : 500);
            if (elapsed >= reverseStart) {
                const QPointF position = destination + (origin - destination) * ((elapsed - reverseStart) / travelMs);
                if (hand) viewport.panTo(position); else viewport.setPanOffset(position);
            }
        } else if (motionEnd < 0) {
            if (hand) {
                viewport.panTo(origin);
                viewport.endMeasuredPan();
                handEnded = true;
            } else viewport.setPanOffset(origin);
            motionEnd = now;
            viewport.phase = 4;
            phases.append(QJsonObject{{"name", "settle"}, {"at_ms", now}});
        }
        if (motionEnd >= 0) {
            const bool ready = viewport.lastFrameAt >= motionEnd && (!coverage || viewport.unsharp <= .01);
            if (ready && finalReady < 0) finalReady = viewport.lastFrameAt;
            if (ready && viewport.idle()) finish("ok");
            else if (now - motionEnd > 10000) finish("final_timeout");
        }
    });
    QTimer::singleShot(0, &viewport, [&] {
        viewport.clock.start();
        viewport.recording = true;
        measuring.store(true);
        startCpu = cpuMs();
        timer.start();
        document = Document::createForPdf("PDF benchmark", input);
        importMs = viewport.now();
        if (!document || !document->providerForSource(QString())) { finish("import_failed"); return; }
        viewport.setDocument(document.get());
        viewport.setInitialView(high ? 4.0 : 1.0);
    });
    const int result = app.exec();
    if (!finished) return 2; // A manually closed window is not a successful run.
    if (hand && !handEnded) viewport.endMeasuredPan();
    viewport.cancelAndWaitForBackgroundThreads();
    return result;
}
}
