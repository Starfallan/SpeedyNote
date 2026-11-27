#pragma once

// Only opt-in benchmark builds contain clocks/counters on rendering paths.
#ifdef SPEEDYNOTE_PDF_BENCHMARK
#include <QImage>
#include <QRectF>
#include <QRegion>
#include <atomic>
#include <chrono>
#include <cstdint>

namespace PdfBenchmark {
inline std::atomic<bool> measuring{false};
inline std::atomic<std::uint64_t> wholeCalls{0}, regionCalls{0}, failures{0};
inline std::atomic<std::uint64_t> rasterPixels{0}, rasterNanoseconds{0};

class RasterSample {
public:
    explicit RasterSample(bool region) : active(measuring.load(std::memory_order_relaxed)) {
        if (active) {
            start = Clock::now();
            (region ? regionCalls : wholeCalls).fetch_add(1, std::memory_order_relaxed);
        }
    }
    void result(const QImage& image) {
        if (active && !image.isNull()) {
            succeeded = true;
            rasterPixels.fetch_add(std::uint64_t(image.width()) * image.height(),
                                   std::memory_order_relaxed);
        }
    }
    ~RasterSample() {
        if (!active) return;
        rasterNanoseconds.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now() - start).count(), std::memory_order_relaxed);
        if (!succeeded) failures.fetch_add(1, std::memory_order_relaxed);
    }
private:
    using Clock = std::chrono::steady_clock;
    bool active = false, succeeded = false;
    Clock::time_point start;
};

class PaintSink {
public:
    virtual ~PaintSink() = default;
    // Logical viewport coordinates of a PDF raster actually sent to QPainter.
    // Future preview paths must report their real DPI through this same hook.
    virtual void pdfDraw(const QRectF& viewportRect, qreal dpi) = 0;
    // The existing snapshot is shifted in physical pixels, then dirty physical
    // regions are repainted with the normal layer stack. Hooks bracket draws.
    virtual void panFrameBegin(const QPoint& shift, const QRegion& dirty, qreal dpr) = 0;
    virtual void panFrameEnd() = 0;
};
}
#endif
