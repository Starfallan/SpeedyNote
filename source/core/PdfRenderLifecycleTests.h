#pragma once

// Integration checks using the real viewport, QtConcurrent and a generated PDF.
// The worker pool gate makes duplicate/invalidation races deterministic. No
// QtTest or visible window is required; this is NOT a performance test.
#include "DocumentViewport.h"
#include "DarkModeUtils.h"
#include "../pdf/MuPdfProvider.h"
#include "../objects/ImageObject.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QPageSize>
#include <QPdfWriter>
#include <QSaveFile>
#include <QSemaphore>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <QtConcurrent>

class PdfRenderLifecycleTests {
    struct Checks {
        QJsonArray results;
        bool passed = true;
        void check(bool ok, const char* description) {
            passed &= ok;
            results.append(QJsonObject{{"check", description}, {"passed", ok}});
        }
    };

    class WorkerGate {
        QThreadPool* pool = QThreadPool::globalInstance();
        int previousCount = pool->maxThreadCount();
        QSemaphore entered, resume;
        QFuture<void> future;
        bool released = false;
    public:
        WorkerGate() {
            pool->waitForDone();
            pool->setMaxThreadCount(1);
            future = QtConcurrent::run([this] { entered.release(); resume.acquire(); });
        }
        bool ready() { return entered.tryAcquire(1, 5000); }
        void release() { if (!released) { released = true; resume.release(); } }
        ~WorkerGate() {
            release();
            future.waitForFinished();
            pool->waitForDone();
            pool->setMaxThreadCount(previousCount);
        }
    };

    template<class Predicate>
    static bool waitUntil(Predicate done) {
        QElapsedTimer timer;
        timer.start();
        while (!done() && timer.elapsed() < 10000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            QThread::msleep(1);
        }
        return done();
    }

    static void prepare(DocumentViewport& view, Document* document) {
        view.resize(640, 360);
        view.setAutoLayoutEnabled(false);
        view.setDocument(document);
        // Deliver the existing deferred auto-fit before fixing the test pose.
        QCoreApplication::processEvents();
        view.cancelAndWaitForBackgroundThreads();
        view.invalidatePdfCache();
        view.setZoomLevel(1.0);
        view.setPanOffset(QPointF(0, 0));
        view.m_pdfPreloadTimer->stop();
        view.m_scrollSettleTimer->stop();
        view.m_focusRebuildTimer->stop();
        view.m_scrollActive = false;
    }

    static std::unique_ptr<Document> open(const QString& path, Checks& c) {
        auto document = Document::createForPdf("PDF request tests", path);
        const bool valid = document && document->providerForSource(QString())
            && document->pageCount() == 3;
        c.check(valid, "MuPDF opens the three-page test PDF");
        if (!valid) return nullptr;
        // Two notebook pages reference the same original page. They are both
        // selected by the initial preload batch and must share a single job.
        document->page(1)->pdfPageNumber = document->page(0)->pdfPageNumber;
        document->page(1)->pdfSourceId = document->page(0)->pdfSourceId;
        return document;
    }

    static void wholePages(const QString& path, Checks& c) {
        auto document = open(path, c);
        if (!document) return;
        DocumentViewport view;
        prepare(view, document.get());
        const QString source = document->page(0)->pdfSourceId;
        const qreal dpi = view.effectivePdfDpi();
        const quint64 pageEpoch = view.m_pdfPageGeneration->load();
        WorkerGate gate; // Released before view/document destruction on failure.
        if (!gate.ready()) { c.check(false, "worker gate starts"); return; }
        view.doAsyncPdfPreload();
        view.doAsyncPdfPreload();
        c.check(view.m_activePdfWatchers.size() == 1 && view.m_pdfPagesInFlight.size() == 1,
                "duplicate notebook pages and repeated preloads submit one page job");
        c.check(view.getCachedPdfPage(source, 0, dpi).isNull(),
                "settled paint defers to the pending job instead of synchronously rasterising");
        c.check(view.isPdfPageInFlight(source, 0, dpi)
                && !view.isPdfPageInFlight(source, 1, dpi)
                && !view.isPdfPageInFlight("another-source", 0, dpi)
                && !view.isPdfPageInFlight(source, 0, dpi * 2),
                "pending identity distinguishes source, original page and DPI");
        view.updatePdfCacheCapacity();
        view.prunePdfTiles();
        c.check(view.m_pdfPageGeneration->load() == pageEpoch,
                "ordinary capacity maintenance and tile pruning preserve page validity");
        gate.release();
        QThreadPool::globalInstance()->waitForDone();
        c.check(view.getCachedPdfPage(source, 0, dpi).isNull(),
                "completed worker awaiting its GUI callback still owns the page");
        c.check(waitUntil([&] { return view.m_activePdfWatchers.isEmpty(); }), "page completion is delivered");
        c.check(!view.lookupCachedPdfPage(source, 0, dpi).isNull(), "valid page survives non-tiled pruning");
        c.check(view.m_pdfPagesInFlight.isEmpty(), "page completion retires its pending identity");
        view.doAsyncPdfPreload();
        c.check(view.m_activePdfWatchers.isEmpty(), "completed cache prevents resubmission");
    }

    static void invalidation(const QString& path, Checks& c, bool tiled) {
        auto document = open(path, c);
        if (!document) return;
        DocumentViewport view;
        prepare(view, document.get());
        if (tiled) view.setZoomLevel(4.0);
        view.m_pdfPreloadTimer->stop();
        const QString source = document->page(0)->pdfSourceId;
        WorkerGate gate;
        if (!gate.ready()) { c.check(false, "worker gate starts"); return; }
        view.doAsyncPdfPreload();
        if (view.m_activePdfWatchers.size() != 1) {
            c.check(false, "one initial render job exists"); return;
        }
        auto* oldWatcher = view.m_activePdfWatchers.first();
        if (tiled) view.invalidatePdfTileCache();
        else {
            view.setPdfDarkModeEnabled(true);
            view.setDarkMode(true);
        }
        view.doAsyncPdfPreload();
        c.check(view.m_activePdfWatchers.size() == 1,
                "invalidation keeps the old job's identity until its completion");
        gate.release();
        oldWatcher->waitForFinished();
        // Deliver just this watcher's completion, not the retry timer. This
        // exposes stale-result rejection independently from the replacement.
        QCoreApplication::sendPostedEvents(oldWatcher);
        c.check(view.m_activePdfWatchers.isEmpty(), "obsolete owner is retired");
        c.check(view.m_pdfCache.isEmpty() && view.m_pdfTileCache.isEmpty(),
                "obsolete image is rejected before cache insertion");
        c.check(view.m_pdfPreloadTimer->isActive(), "discard schedules latest demand once");
        c.check(waitUntil([&] {
            return view.m_activePdfWatchers.isEmpty() && !view.m_pdfPreloadTimer->isActive()
                && (tiled ? !view.m_pdfTileCache.isEmpty() : !view.m_pdfCache.isEmpty());
        }), "deferred replacement completes without additional user input");
        c.check(view.m_pdfPagesInFlight.isEmpty() && view.m_pdfTilesInFlight.isEmpty(),
                "late completion cannot remove or strand a replacement identity");
        if (!tiled) {
            const QImage image = view.lookupCachedPdfPage(source, 0, view.effectivePdfDpi()).toImage();
            c.check(!image.isNull() && image.pixelColor(5, 5).lightness() < 128,
                    "replacement uses the new dark theme");
        }
    }

    static void documentSwitchAndShutdown(const QString& path, Checks& c) {
        auto oldDocument = open(path, c);
        auto newDocument = Document::createNew("replacement without PDF");
        if (!oldDocument) return;
        DocumentViewport view;
        prepare(view, oldDocument.get());
        WorkerGate gate;
        if (!gate.ready()) { c.check(false, "worker gate starts"); return; }
        view.doAsyncPdfPreload();
        view.setDocument(newDocument.get());
        oldDocument.reset(); // Workers must use snapshots, never the Document.
        gate.release();
        c.check(waitUntil([&] { return view.m_activePdfWatchers.isEmpty() && !view.m_pdfPreloadTimer->isActive(); }),
                "pending old-document jobs finish after the document is destroyed");
        c.check(view.m_pdfCache.isEmpty() && view.m_pdfTileCache.isEmpty(),
                "old document results never enter the new document cache");

        auto pdfDocument = open(path, c);
        if (!pdfDocument) return;
        prepare(view, pdfDocument.get());
        view.setZoomLevel(4.0);
        view.doAsyncPdfPreload();
        view.cancelAndWaitForBackgroundThreads();
        c.check(view.m_activePdfWatchers.isEmpty() && view.m_pdfPagesInFlight.isEmpty()
                && view.m_pdfTilesInFlight.isEmpty(), "joined shutdown clears all pending identities");
        view.doAsyncPdfPreload();
        c.check(waitUntil([&] { return view.m_activePdfWatchers.isEmpty(); }) && !view.m_pdfTileCache.isEmpty(),
                "a viewport can submit the same tile after joined cancellation");
        view.setDocument(nullptr); // pdfDocument is declared after the viewport.
    }

    static void repaint(const QString& path, Checks& c) {
        auto document = open(path, c);
        if (!document) return;
        DocumentViewport view;
        prepare(view, document.get());
        view.resize(640, 720);
        view.setZoomLevel(.25);
        view.setPanOffset(QPointF(0, 0));
        const QString source = document->page(0)->pdfSourceId;
        QRegion expected;
        for (int i = 0; i < 2; ++i) {
            expected += QRectF((view.pagePosition(i) - view.panOffset()) * view.zoomLevel(),
                               document->page(i)->size * view.zoomLevel()).toAlignedRect().intersected(view.rect());
        }
        const QRegion actual = view.pdfRepaintRegion(source, 0);
        c.check(!actual.isEmpty() && actual == expected,
                "repaint includes both visible occurrences and excludes unrelated pages and margins");
        c.check(view.pdfRepaintRegion(source, 99).isEmpty()
                && view.pdfRepaintRegion("another-source", 0).isEmpty(), "unrelated results cause no repaint");
        const QRegion tileDirty = view.pdfRepaintRegion(source, 0, QRectF(0, 0, 40, 40));
        c.check(!tileDirty.isEmpty() && tileDirty != actual && (tileDirty - actual).isEmpty(),
                "tile completion restricts repaint to its visible region");
        view.m_gesture.activeType = DocumentViewport::ViewportGestureState::Pan;
        view.m_gesture.cachedFrame = QPixmap(8, 8);
        c.check(view.pdfRepaintRegion(source, 0) == expected,
                "pan completion identifies current visible content for snapshot refresh");
        view.m_gesture.activeType = DocumentViewport::ViewportGestureState::Zoom;
        c.check(view.pdfRepaintRegion(source, 0).isEmpty(), "zoom still defers snapshot content changes");
        view.m_gesture.reset();
    }

    static QImage canvasImage(DocumentViewport& view, qreal dpr) {
        QImage image(QSize(qRound(view.width() * dpr), qRound(view.height() * dpr)), QImage::Format_RGB32);
        image.setDevicePixelRatio(dpr);
        QPainter painter(&image);
        view.renderCanvas(painter, view.rect());
        return image;
    }

    static void livePan(const QString& path, Checks& c, bool tiled) {
        auto document = open(path, c);
        if (!document) return;
        DocumentViewport view;
        view.setAttribute(Qt::WA_DontShowOnScreen);
        view.show(); // Exercise the real visible/enabled entry without showing a window.
        prepare(view, document.get());
        if (tiled) view.setZoomLevel(4.0);
        view.m_pdfPreloadTimer->stop();
        const QString source = document->page(2)->pdfSourceId;
        // Ink and an inserted object must survive PDF result composition.
        VectorStroke stroke;
        stroke.color = Qt::magenta;
        stroke.baseThickness = 8;
        stroke.points = {{QPointF(20, 60), 1.0, 0}, {QPointF(120, 60), 1.0, 0}};
        stroke.updateBoundingBox();
        document->page(2)->layer(0)->addStroke(stroke);
        auto object = std::make_unique<ImageObject>();
        QPixmap swatch(30, 30);
        swatch.fill(Qt::green);
        object->setPixmap(swatch);
        object->position = QPointF(80, 20);
        document->page(2)->addObject(std::move(object));

        WorkerGate gate;
        if (!gate.ready()) { c.check(false, "pan worker gate starts"); return; }
        view.beginPanGesture();
        c.check(view.m_gesture.activeType == DocumentViewport::ViewportGestureState::Pan,
                "live pan starts through the real gesture entry");
        const QPointF target = view.pagePosition(2);
        for (int i = 0; i < 40; ++i) {
            view.updatePanGesture(target + QPointF(0, i / view.zoomLevel()) - view.m_gesture.targetPan);
            view.doAsyncPdfPreload();
        }
        view.updatePanGesture(target - view.m_gesture.targetPan);
        view.m_gestureTimeoutTimer->stop(); // A safety timeout must not masquerade as live progress.
        c.check(view.m_panOffset == target && view.m_scrollActive
                && !view.m_scrollSettleTimer->isActive() && !view.m_focusRebuildTimer->isActive()
                && view.m_activePdfWatchers.size() == 1,
                "held pan follows target demand with one job and no settle housekeeping");
        gate.release();
        c.check(waitUntil([&] {
            // QWidget::render supplies paints even without a native visible window.
            if (view.m_gesture.panFrameRefreshDue) view.grabOpaqueViewport();
            const bool loaded = tiled
                ? !view.lookupCachedPdfTiles(source, 2, view.pdfTileDpi()).isEmpty()
                : !view.lookupCachedPdfPage(source, 2, view.effectivePdfDpi()).isNull();
            return loaded && view.m_activePdfWatchers.isEmpty()
                && !view.m_pdfPreloadTimer->isActive() && !view.m_panFrameTimer->isActive()
                && !view.m_gesture.panFrameRefreshDue && !view.m_gesture.panFrameDirty;
        }), "PDF loads and enters the held snapshot without release or further movement");
        c.check(view.m_gesture.activeType == DocumentViewport::ViewportGestureState::Pan,
                "progress did not end the gesture");
        const QPointF actualPan = view.m_panOffset;
        view.m_panOffset = view.m_gesture.startPan;
        const QImage expected = canvasImage(view, view.m_gesture.frameDevicePixelRatio);
        view.m_panOffset = actualPan;
        c.check(view.m_gesture.cachedFrame.toImage().convertToFormat(QImage::Format_RGB32) == expected,
                "held snapshot matches normal PDF, ink and object composition");

        // Refresh-only invalidation works while the finger remains stationary.
        view.setDarkMode(true);
        c.check(view.m_panFrameTimer->isActive() && view.m_gesture.panFrameDirty,
                "stationary theme change schedules a snapshot refresh");
        view.endPanGesture();
        c.check(!view.m_panFrameTimer->isActive() && !view.m_gesture.panFrameRefreshDue,
                "release retires pending snapshot refresh");
        view.beginPanGesture();
        view.updatePanGesture(QPointF(0, 1));
        view.hide();
        c.check(!view.m_panFrameTimer->isActive() && !view.m_gesture.isActive(),
                "hiding cancels a queued pan snapshot update");
        view.show();
        view.beginPanGesture();
        view.updatePanGesture(QPointF(0, 1));
        view.setDocument(nullptr);
        c.check(!view.m_panFrameTimer->isActive() && !view.m_gesture.isActive(),
                "document switch cancels a queued pan snapshot update");
    }

    static void panPixelAlignment(const QString& path, Checks& c) {
        auto document = open(path, c);
        if (!document) return;
        DocumentViewport view;
        prepare(view, document.get());
        // Cache-independent, high-contrast content detects stale exposed pixels.
        for (int i = 0; i < document->pageCount(); ++i) {
            document->page(i)->backgroundType = Page::BackgroundType::Grid;
            document->page(i)->gridSpacing = 19;
            for (int version : {0, 1}) {
                auto text = std::make_unique<TextBoxObject>();
                text->text = QStringLiteral("**Pan clip:** overlapping translucent text");
                text->textLayoutVersion = version;
                text->fontSize = 16;
                text->position = QPointF(35, 45 + version * 90);
                text->size = QSizeF(250, 80);
                text->fontColor = QColor(30, 50, 150, 120);
                text->backgroundColor = QColor(180, 210, 240, 100);
                document->page(i)->addObject(std::move(text));
            }
        }
        for (qreal dpr : {1.0, 1.25, 1.5, 2.0}) {
            const QPointF origin(10, 10);
            view.m_panOffset = origin;
            view.m_gesture.activeType = DocumentViewport::ViewportGestureState::Pan;
            view.m_gesture.startZoom = view.zoomLevel();
            view.m_gesture.startPan = view.m_gesture.targetPan = origin;
            view.m_gesture.panFrameOrigin = origin;
            view.m_gesture.panFramePixelOffset = QPoint();
            view.m_gesture.frameDevicePixelRatio = dpr;
            view.m_gesture.cachedFrame = QPixmap::fromImage(canvasImage(view, dpr));
            bool aligned = true;
            // Multiple viewport lengths, both directions, and subpixel steps.
            for (int step = 0; step < 80; ++step) {
                const qreal travel = (step < 40 ? step : 79 - step) * 13.37;
                const QPointF target = origin + QPointF(travel * .41, travel);
                view.updatePanGesture(target - view.m_gesture.targetPan);
                view.m_panFrameTimer->stop();
                view.refreshPanFrame();
                const QPointF residual = (target - view.m_gesture.startPan) * (view.zoomLevel() * dpr);
                aligned &= qAbs(residual.x()) <= .500001 && qAbs(residual.y()) <= .500001;
            }
            c.check(aligned && QLineF(view.m_gesture.startPan, origin).length() < .000001,
                    "fractional-DPR round trip preserves origin and bounds subpixel residual");
            c.check(view.m_gesture.cachedFrame.toImage().convertToFormat(QImage::Format_RGB32)
                    == canvasImage(view, dpr), "scrolled strips match a fresh canvas after a round trip");
            view.resetViewportGesture();
        }
        view.setDocument(nullptr);
    }

    static void latestDemand(const QString& path, Checks& c, bool tiled) {
        auto document = open(path, c);
        if (!document) return;
        DocumentViewport view;
        prepare(view, document.get());
        if (tiled) view.setZoomLevel(4.0);
        view.m_pdfPreloadTimer->stop();
        WorkerGate gate;
        if (!gate.ready()) { c.check(false, "worker gate starts"); return; }
        const QString source = document->page(0)->pdfSourceId;
        if (!tiled) {
            c.check(view.getCachedPdfPage(source, 0, view.effectivePdfDpi()).isNull()
                    && view.m_pdfPreloadTimer->isActive() && view.m_activePdfWatchers.isEmpty(),
                    "first settled cache miss schedules work without synchronously rendering");
        }
        view.doAsyncPdfPreload();
        c.check(view.m_activePdfWatchers.size() == 1, "one render is admitted per viewport");
        const auto oldDemand = view.m_pdfRenderObsolete;
        const QPointF target = view.pagePosition(2);
        for (int i = 0; i < 40; ++i) {
            view.setPanOffset(target + QPointF(0, i));
            view.doAsyncPdfPreload();
        }
        view.setPanOffset(target);
        view.m_scrollSettleTimer->stop(); // Keep scrolling active throughout completion.
        c.check(view.m_scrollActive && view.m_activePdfWatchers.size() == 1
                && oldDemand && oldDemand->load(),
                "rapid scroll keeps one job and marks the off-screen queued request obsolete");
        gate.release();
        c.check(waitUntil([&] {
            return view.m_activePdfWatchers.isEmpty() && !view.m_pdfPreloadTimer->isActive()
                && (tiled ? !view.lookupCachedPdfTiles(source, 2, view.pdfTileDpi()).isEmpty()
                          : !view.lookupCachedPdfPage(source, 2, view.effectivePdfDpi()).isNull());
        }), "latest visible content loads while scrolling is still active");
        c.check(view.m_scrollActive && view.m_pdfReservedPixels == 0,
                "progress does not depend on scroll settle and releases pixel reservations");
        c.check(tiled ? view.lookupCachedPdfTiles(source, 0, view.pdfTileDpi()).isEmpty()
                      : view.lookupCachedPdfPage(source, 0, view.effectivePdfDpi()).isNull(),
                "obsolete queued content is not inserted into cache");
    }

    static void cacheRetention(const QString& path, Checks& c, bool tiled) {
        auto document = open(path, c);
        if (!document) return;
        // Source order 2, 0, 1 differs from notebook order. With stale page-1
        // entries, distance-from-new-result eviction alternates 2 and 0 forever.
        document->page(0)->pdfPageNumber = 2;
        document->page(1)->pdfPageNumber = 0;
        document->page(2)->pdfPageNumber = 1;
        DocumentViewport view;
        prepare(view, document.get());
        if (tiled) {
            view.setZoomLevel(4.0);
            view.setPanOffset(view.pagePosition(1) - QPointF(0, 45));
            view.m_scrollSettleTimer->stop();
            c.check(view.visiblePages().size() == 2, "tile retention test spans two reordered pages");
        }
        view.updatePdfCacheCapacity();
        view.m_pdfPreloadTimer->stop();
        const QString source = document->page(0)->pdfSourceId;
        QPixmap marker(1, 1);
        marker.fill(Qt::white);
        const int capacity = tiled ? view.m_pdfTileCacheCapacity : view.m_pdfCacheCapacity;
        for (int i = 0; i < capacity; ++i) {
            if (tiled) {
                PdfTileEntry entry;
                entry.sourceId = QStringLiteral("previous-source-%1").arg(i);
                entry.pageIndex = 1;
                entry.dpi = view.pdfTileDpi();
                entry.regionUnits = QRectF(QPointF(0, 0), document->page(0)->size);
                entry.pixmap = marker;
                view.m_pdfTileCache.append(entry);
            } else {
                PdfCacheEntry entry;
                entry.sourceId = QStringLiteral("previous-source-%1").arg(i);
                entry.pageIndex = 1;
                entry.dpi = view.effectivePdfDpi();
                entry.pixmap = marker;
                view.m_pdfCache.append(entry);
            }
        }
        view.doAsyncPdfPreload();
        c.check(waitUntil([&] {
            const bool bothCached = tiled
                ? !view.lookupCachedPdfTiles(source, 2, view.pdfTileDpi()).isEmpty()
                    && !view.lookupCachedPdfTiles(source, 0, view.pdfTileDpi()).isEmpty()
                : !view.lookupCachedPdfPage(source, 2, view.effectivePdfDpi()).isNull()
                    && !view.lookupCachedPdfPage(source, 0, view.effectivePdfDpi()).isNull();
            return bothCached && view.m_activePdfWatchers.isEmpty() && !view.m_pdfPreloadTimer->isActive();
        }), "reordered pages retain current demand and finish without repeated eviction");
        if (tiled) {
            view.cancelAndWaitForBackgroundThreads();
            view.m_pdfTileCache.clear();
            view.m_pdfTileCacheCapacity = 1;
            PdfTileEntry entry;
            entry.sourceId = source;
            entry.pageIndex = 2;
            entry.dpi = view.pdfTileDpi();
            entry.regionUnits = QRectF(QPointF(0, 0), document->page(0)->size);
            entry.pixmap = marker;
            view.insertPdfTile(entry);
            view.doAsyncPdfTilePreload();
            c.check(view.m_activePdfWatchers.isEmpty() && !view.m_pdfPreloadTimer->isActive()
                    && view.m_pdfTileCache.size() == 1 && view.m_pdfTileCache.first().pageIndex == 2,
                    "exhausted tile cache does not replace visible coverage in a render loop");
        }
    }

    static void regionReuse(const QString& path, Checks& c) {
        MuPdfProvider direct(path), cached(path, true);
        for (int page : {0, 0, 1, 2, 0}) {
            for (qreal dpi : {72.0, 144.0, 384.0}) {
                for (const QRectF& region : {QRectF(0, 0, 120, 100), QRectF(20.25, 30.5, 200, 150)}) {
                    const QImage expected = direct.renderPageRegionToImage(page, dpi, region);
                    const QImage actual = cached.renderPageRegionToImage(page, dpi, region);
                    c.check(!actual.isNull() && actual == expected, "cached region pixels match direct rendering");
                    c.check(cached.cachedRegionPage() == page, "one display list tracks the requested source page");
                    c.check(cached.imageRegions(page, dpi) == direct.imageRegions(page, dpi),
                            "display-list image masks match direct page masks");
                    cached.trimStore();
                    c.check(cached.cachedRegionPage() == page, "small display list survives store trimming");
                }
            }
        }
        cached.clearRegionCache();
        cached.m_regionCacheLimit = 1; // Existing context already exceeds this.
        const QRectF region(0, 0, 120, 100);
        const QImage expected = direct.renderPageRegionToImage(0, 144, region);
        for (int n = 0; n < 2; ++n) {
            const QImage actual = cached.renderPageRegionToImage(0, 144, region);
            c.check(!actual.isNull() && actual == expected && cached.cachedRegionPage() < 0,
                    "cache budget rejection preserves the direct render path");
        }
        cached.m_regionCacheLimit = 32u * 1024u * 1024u;
        cached.renderPageRegionToImage(1, 72, region);
        c.check(cached.cachedRegionPage() == 1, "another page can cache after rejection");
        cached.renderPageToImage(0, 72);
        c.check(cached.cachedRegionPage() < 0, "whole-page mode releases the region list");
    }

    static bool writeGeometryPdf(const QString& path) {
        // QPdfWriter normalizes page geometry. Explicit PDF boxes/rotation
        // exercise MuPDF's transformed page space and fractional clip edges.
        const QByteArray content = "0.2 0.6 0.8 rg -50 -30 400 530 re f\n"
            "1 0 0 RG 3.5 w -40 -20 m 330 470 l S\n"
            "0 1 0 RG 0.4 w 20 400 m 100 80 250 350 300 20 c S\n";
        QVector<QByteArray> objects{
            "<< /Type /Catalog /Pages 2 0 R >>",
            "<< /Type /Pages /Count 3 /Kids [4 0 R 5 0 R 6 0 R] >>",
            QByteArray("<< /Length ") + QByteArray::number(qint64(content.size())) + ">>\nstream\n"
                + content + "endstream"};
        for (int rotation : {0, 90, 270}) {
            objects.append(QByteArray("<< /Type /Page /Parent 2 0 R /MediaBox [-50 -30 350 500] "
                "/CropBox [-25 -15 310 450] /Rotate ") + QByteArray::number(rotation)
                + " /Resources << >> /Contents 3 0 R >>");
        }
        QByteArray bytes("%PDF-1.4\n");
        QByteArray table("0000000000 65535 f \n");
        for (int i = 0; i < objects.size(); ++i) {
            table += QByteArray::number(qint64(bytes.size())).rightJustified(10, '0') + " 00000 n \n";
            bytes += QByteArray::number(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
        }
        const auto xref = bytes.size();
        bytes += "xref\n0 7\n" + table + "trailer\n<< /Size 7 /Root 1 0 R >>\nstartxref\n"
            + QByteArray::number(qint64(xref)) + "\n%%EOF\n";
        QSaveFile file(path);
        return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
    }

    static void completedResultReuse(const QString& path, Checks& c, bool tiled) {
        auto document = open(path, c);
        if (!document) return;
        DocumentViewport view;
        prepare(view, document.get());
        if (tiled) view.setZoomLevel(4.0);
        const QPointF origin = view.panOffset();
        view.doAsyncPdfPreload();
        if (view.m_activePdfWatchers.size() != 1) { c.check(false, "completed reuse job admitted"); return; }
        auto* watcher = view.m_activePdfWatchers.first();
        watcher->waitForFinished(); // Leave the GUI callback undelivered.
        view.setPanOffset(view.pagePosition(2));
        view.m_scrollSettleTimer->stop();
        QCoreApplication::sendPostedEvents(watcher);
        const QString source = document->page(0)->pdfSourceId;
        c.check(tiled ? !view.lookupCachedPdfTiles(source, 0, view.pdfTileDpi()).isEmpty()
                      : !view.lookupCachedPdfPage(source, 0, view.effectivePdfDpi()).isNull(),
                "completed valid content survives moving off-screen before its callback");
        view.setPanOffset(origin);
        view.m_scrollSettleTimer->stop();
        view.doAsyncPdfPreload();
        c.check(view.m_activePdfWatchers.isEmpty(), "return to completed content needs no second render");
    }

    static void workerDarkMode(const QString& path, Checks& c, bool tiled, bool skipImages) {
        auto document = open(path, c);
        if (!document) return;
        DocumentViewport view;
        prepare(view, document.get());
        view.setPdfDarkModeEnabled(true);
        view.setDarkMode(true);
        view.setSkipImageMasking(skipImages);
        if (tiled) view.setZoomLevel(4.0);
        view.doAsyncPdfPreload();
        if (view.m_activePdfWatchers.size() != 1) { c.check(false, "dark worker job admitted"); return; }
        auto* watcher = view.m_activePdfWatchers.first();
        watcher->waitForFinished();
        const QImage actual = watcher->result();
        MuPdfProvider reference(path);
        const qreal dpi = tiled ? view.pdfTileDpi() : view.effectivePdfDpi();
        QRectF region;
        if (tiled) {
            const auto tile = view.pdfTileRegionForPage(document->page(0), 0);
            region = QRectF(tile.xPt, tile.yPt, tile.wPt, tile.hPt);
        }
        QImage expected = tiled ? reference.renderPageRegionToImage(0, dpi, region)
                                : reference.renderPageToImage(0, dpi);
        auto masks = reference.imageRegions(0, dpi);
        if (tiled) {
            QVector<QRect> clipped;
            for (const QRect& mask : masks) {
                const QRect part = mask.translated(-qRound(region.x() * dpi / 72.0),
                    -qRound(region.y() * dpi / 72.0)).intersected(expected.rect());
                if (!part.isEmpty()) clipped.append(part);
            }
            masks = clipped;
        }
        c.check(!masks.isEmpty(), "dark worker sample contains a raster image");
        if (skipImages) masks.clear();
        DarkModeUtils::invertImageLightness(expected, masks);
        c.check(!actual.isNull() && actual == expected,
                "worker returns final dark pixels respecting the image-mask setting before GUI callback");
        // A completed image waiting for delivery is still stale after a theme
        // change, even though moving off-screen alone now allows its reuse.
        view.setDarkMode(false);
        QCoreApplication::sendPostedEvents(watcher);
        c.check(view.m_pdfCache.isEmpty() && view.m_pdfTileCache.isEmpty(),
                "theme change rejects a completed dark image before cache insertion");
        view.cancelAndWaitForBackgroundThreads();
    }

    static void sharedAdmission(const QString& path, Checks& c) {
        auto document = open(path, c);
        if (!document) return;
        DocumentViewport a, b, d;
        for (auto* view : {&a, &b, &d}) prepare(*view, document.get());
        WorkerGate gate;
        if (!gate.ready()) { c.check(false, "worker gate starts"); return; }
        for (auto* view : {&a, &b, &d}) view->doAsyncPdfPreload();
        const int admitted = a.m_activePdfWatchers.size() + b.m_activePdfWatchers.size()
            + d.m_activePdfWatchers.size();
        c.check(admitted > 0 && admitted <= 2 && d.m_activePdfWatchers.isEmpty(),
                "multiple viewports share the process-wide admission bound");
        // Remove a waiter, then re-register it; a dead queue head must not
        // strand later viewports, and repeated demand must not duplicate it.
        d.setDocument(nullptr);
        d.setDocument(document.get());
        d.doAsyncPdfPreload();
        gate.release();
        c.check(waitUntil([&] {
            for (auto* view : {&a, &b, &d}) {
                if (view->m_pdfCache.isEmpty() || !view->m_activePdfWatchers.isEmpty()
                    || view->m_pdfPreloadTimer->isActive()) return false;
            }
            return true;
        }), "all waiting viewports progress without polling or more user input");
        c.check(a.m_pdfReservedPixels == 0 && b.m_pdfReservedPixels == 0 && d.m_pdfReservedPixels == 0,
                "shared admission releases all reservations");
    }

public:
    static int run(const QString& reportPath) {
        if (reportPath.isEmpty() || QFileInfo::exists(reportPath)) return 2;
        QTemporaryDir temporary;
        if (!temporary.isValid()) return 2;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
        QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, temporary.path());
        Checks c;
        const QString pdfPath = temporary.filePath("pages.pdf");
        {
            QPdfWriter writer(pdfPath);
            writer.setResolution(96);
            writer.setPageSize(QPageSize(QPageSize::A4));
            QPainter painter(&writer);
            for (int page = 0; page < 3; ++page) {
                if (page) c.check(writer.newPage(), "PDF writer creates another page");
                painter.fillRect(0, 0, writer.width(), writer.height(), Qt::white);
                painter.drawText(QPoint(30, 50), QStringLiteral("PDF lifecycle page %1").arg(page));
                QImage photo(32, 32, QImage::Format_RGB32);
                photo.fill(QColor(90, 120, 150));
                painter.drawImage(QRect(10, 10, 64, 48), photo);
                painter.save();
                painter.setOpacity(0.4);
                painter.fillRect(QRect(95, 75, 65, 40), Qt::red);
                painter.restore();
            }
            c.check(painter.end(), "PDF painter finishes");
        }
        wholePages(pdfPath, c);
        invalidation(pdfPath, c, false);
        invalidation(pdfPath, c, true);
        documentSwitchAndShutdown(pdfPath, c);
        repaint(pdfPath, c);
        livePan(pdfPath, c, false);
        livePan(pdfPath, c, true);
        panPixelAlignment(pdfPath, c);
        latestDemand(pdfPath, c, false);
        latestDemand(pdfPath, c, true);
        cacheRetention(pdfPath, c, false);
        cacheRetention(pdfPath, c, true);
        regionReuse(pdfPath, c);
        const QString geometryPath = temporary.filePath("geometry.pdf");
        const bool geometryWritten = writeGeometryPdf(geometryPath);
        c.check(geometryWritten, "writes rotated/cropped PDF for region comparisons");
        if (geometryWritten) regionReuse(geometryPath, c);
        completedResultReuse(pdfPath, c, false);
        completedResultReuse(pdfPath, c, true);
        workerDarkMode(pdfPath, c, false, false);
        workerDarkMode(pdfPath, c, true, false);
        workerDarkMode(pdfPath, c, false, true);
        workerDarkMode(pdfPath, c, true, true);
        sharedAdmission(pdfPath, c);
        QSaveFile report(reportPath);
        if (!report.open(QIODevice::WriteOnly)) return 2;
        const QByteArray bytes = QJsonDocument(QJsonObject{{"passed", c.passed}, {"checks", c.results}}).toJson();
        if (report.write(bytes) != bytes.size() || !report.commit()) return 2;
        return c.passed ? 0 : 1;
    }
};
