#pragma once

// Integration checks using the real viewport, QtConcurrent and a generated PDF.
// The worker pool gate makes duplicate/invalidation races deterministic. No
// QtTest or visible window is required; this is NOT a performance test.
#include "DocumentViewport.h"
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
        c.check(view.pdfRepaintRegion(source, 0).isEmpty(), "results do not redraw an unchanged gesture snapshot");
        view.m_gesture.reset();
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
            }
            c.check(painter.end(), "PDF painter finishes");
        }
        wholePages(pdfPath, c);
        invalidation(pdfPath, c, false);
        invalidation(pdfPath, c, true);
        documentSwitchAndShutdown(pdfPath, c);
        repaint(pdfPath, c);
        latestDemand(pdfPath, c, false);
        latestDemand(pdfPath, c, true);
        sharedAdmission(pdfPath, c);
        QSaveFile report(reportPath);
        if (!report.open(QIODevice::WriteOnly)) return 2;
        const QByteArray bytes = QJsonDocument(QJsonObject{{"passed", c.passed}, {"checks", c.results}}).toJson();
        if (report.write(bytes) != bytes.size() || !report.commit()) return 2;
        return c.passed ? 0 : 1;
    }
};
