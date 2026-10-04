#include "TrickplayBenchmark.h"

#include "app/GraphicsStartup.h"
#include "app/TrickplayDecoder.h"
#include "platform/NativeAppWindow.h"
#include "platform/PlatformPaths.h"
#include "platform/PlatformStartup.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickGraphicsConfiguration>
#include <QStandardPaths>
#include <QSysInfo>
#include <QThread>
#include <QTimer>
#include <QVariantList>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <clocale>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <jpeglib.h>
#include <limits>
#include <mutex>
#include <random>
#include <setjmp.h>
#include <thread>
#include <vector>
#if defined(Q_OS_UNIX)
#include <sys/resource.h>
#include <time.h>
#endif
#if defined(__GLIBC__)
#include <malloc.h>
#endif

namespace Spool {
namespace {
    using Clock = std::chrono::steady_clock;
    qint64 nowNs()
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
    }
    qint64 threadCpuNs()
    {
#if defined(CLOCK_THREAD_CPUTIME_ID)
        timespec value {};
        if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) == 0)
            return qint64(value.tv_sec) * 1000000000 + value.tv_nsec;
#endif
        return -1;
    }
    QVariant numberOrNull(qint64 value)
    {
        return value < 0 ? QVariant() : QVariant(value);
    }
    struct StageClock {
        qint64 wall = nowNs();
        qint64 cpu = threadCpuNs();
        void finish(QVariantMap& record, const QString& prefix) const
        {
            const qint64 endCpu = threadCpuNs();
            record.insert(prefix + QStringLiteral("_wall_ns"), nowNs() - wall);
            record.insert(
                prefix + QStringLiteral("_thread_cpu_ns"), numberOrNull(cpu < 0 || endCpu < 0 ? -1 : endCpu - cpu));
        }
    };

    struct Memory {
        qint64 rss = -1;
        qint64 anonymous = -1;
        qint64 hwm = -1;
    };
    Memory readMemory()
    {
        Memory result;
#if defined(Q_OS_LINUX)
        if (FILE *file = std::fopen("/proc/self/status", "r")) {
            char line[256];
            while (std::fgets(line, sizeof(line), file)) {
                long long kb;
                if (std::sscanf(line, "VmRSS: %lld kB", &kb) == 1)
                    result.rss = kb * 1024;
                else if (std::sscanf(line, "RssAnon: %lld kB", &kb) == 1)
                    result.anonymous = kb * 1024;
                else if (std::sscanf(line, "VmHWM: %lld kB", &kb) == 1)
                    result.hwm = kb * 1024;
            }
            std::fclose(file);
        }
#elif defined(Q_OS_UNIX)
        rusage usage {};
        if (getrusage(RUSAGE_SELF, &usage) == 0) {
#if defined(Q_OS_MACOS)
            result.hwm = usage.ru_maxrss;
#else
            result.hwm = qint64(usage.ru_maxrss) * 1024;
#endif
        }
#endif
        return result;
    }
    QVariantMap memoryMap(const Memory& value)
    {
        return { { "rss_bytes", numberOrNull(value.rss) }, { "anonymous_bytes", numberOrNull(value.anonymous) },
            { "process_cumulative_high_water_rss_bytes", numberOrNull(value.hwm) } };
    }
    QVariant delta(qint64 a, qint64 b)
    {
        return a < 0 || b < 0 ? QVariant() : QVariant(a - b);
    }
    // An independent sampler sees allocations during decode and on the render thread.
    // Its peaks are sampled lower bounds, not exact allocation/VRAM instrumentation.
    class MemorySampler {
    public:
        MemorySampler()
            : m_thread([this] {
                std::unique_lock<std::mutex> lock(m_mutex);
                while (!m_stop) {
                    const quint64 generation = m_generation;
                    lock.unlock();
                    const Memory value = readMemory();
                    lock.lock();
                    if (generation == m_generation && !m_stop) {
                        m_peak.rss = std::max(m_peak.rss, value.rss);
                        m_peak.anonymous = std::max(m_peak.anonymous, value.anonymous);
                        m_peak.hwm = std::max(m_peak.hwm, value.hwm);
                        ++m_samples;
                    }
                    m_condition.wait_for(lock, std::chrono::milliseconds(1), [this] { return m_stop; });
                }
            })
        {
        }
        ~MemorySampler()
        {
            stop();
        }
        void stop()
        {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_stop = true;
            }
            m_condition.notify_one();
            if (m_thread.joinable())
                m_thread.join();
        }
        void reset(const Memory& baseline)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            ++m_generation;
            m_peak = baseline;
            m_samples = 0;
        }
        std::pair<Memory, quint64> peak()
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return { m_peak, m_samples };
        }

    private:
        std::mutex m_mutex;
        std::condition_variable m_condition;
        bool m_stop = false;
        Memory m_peak;
        quint64 m_samples = 0;
        quint64 m_generation = 0;
        std::thread m_thread;
    };
    void trimAllocator()
    {
#if defined(__GLIBC__)
        malloc_trim(0);
#endif
    }

    struct DecodeResult {
        std::shared_ptr<TrickplayBenchFrame> frame;
        QVariantMap record;
        QString error;
    };
    struct JpegError {
        jpeg_error_mgr base;
        jmp_buf jump;
        char message[JMSG_LENGTH_MAX] {};
    };
    void jpegFailure(j_common_ptr common)
    {
        auto *error = reinterpret_cast<JpegError *>(common->err);
        common->err->format_message(common, error->message);
        longjmp(error->jump, 1);
    }
    // All nontrivial state lives outside the setjmp stack: libjpeg can fail at any
    // scanline, including malformed or truncated input, without skipping destructors.
    struct JpegState {
        jpeg_decompress_struct codec {};
        JpegError error;
        bool created = false;
        std::shared_ptr<TrickplayBenchFrame> frame = std::make_shared<TrickplayBenchFrame>();
        QVariantList factors;
        QVariantList strides;
        QVariantList heights;
        ~JpegState()
        {
            if (created)
                jpeg_destroy_decompress(&codec);
        }
    };
    DecodeResult decodeJpeg(
        const QByteArray& bytes, const QSize& expected, bool planar, const std::atomic<bool>& cancel)
    {
        DecodeResult result;
        const StageClock total;
        auto state = std::make_unique<JpegState>();
        auto& codec = state->codec;
        codec.err = jpeg_std_error(&state->error.base);
        state->error.base.error_exit = jpegFailure;
        if (setjmp(state->error.jump)) {
            result.error = QString::fromLatin1(state->error.message);
            return result;
        }
        const StageClock setup;
        state->created = true;
        jpeg_create_decompress(&codec);
        jpeg_mem_src(&codec, reinterpret_cast<const unsigned char *>(bytes.constData()),
            static_cast<unsigned long>(bytes.size()));
        if (jpeg_read_header(&codec, TRUE) != JPEG_HEADER_OK) {
            result.error = QStringLiteral("JPEG header was not complete");
            return result;
        }
        if (codec.image_width != JDIMENSION(expected.width()) || codec.image_height != JDIMENSION(expected.height())
            || codec.num_components != 3 || codec.jpeg_color_space != JCS_YCbCr) {
            result.error = QStringLiteral("Fixture dimensions or JPEG YCbCr components differ from manifest");
            return result;
        }
        for (int plane = 0; plane < codec.num_components; ++plane)
            state->factors.append(QVariantMap { { "horizontal", codec.comp_info[plane].h_samp_factor },
                { "vertical", codec.comp_info[plane].v_samp_factor } });
        result.record.insert("jpeg_sampling_factors", state->factors);
        if (planar
            && (codec.comp_info[0].h_samp_factor != 2 || codec.comp_info[0].v_samp_factor != 2
                || codec.comp_info[1].h_samp_factor != 1 || codec.comp_info[1].v_samp_factor != 1
                || codec.comp_info[2].h_samp_factor != 1 || codec.comp_info[2].v_samp_factor != 1)) {
            result.error = QStringLiteral("Direct YUV420 requires a real 4:2:0 fixture, not a conversion fallback");
            return result;
        }
        codec.dct_method = JDCT_ISLOW;
        codec.do_fancy_upsampling = TRUE;
        codec.do_block_smoothing = TRUE;
        codec.raw_data_out = planar ? TRUE : FALSE;
        codec.out_color_space = planar ? JCS_YCbCr : JCS_EXT_BGRA;
        if (!jpeg_start_decompress(&codec)) {
            result.error = QStringLiteral("JPEG decoder suspended at start");
            return result;
        }
        setup.finish(result.record, QStringLiteral("header_codec_setup"));
        const StageClock allocate;
        auto& frame = *state->frame;
        frame.size = expected;
        frame.format = planar ? TrickplayBenchFormat::Yuv420 : TrickplayBenchFormat::Rgb32;
        qint64 allocated = 0;
        for (int plane = 0; plane < (planar ? 3 : 1); ++plane) {
            const qint64 stride
                = planar ? qint64(codec.comp_info[plane].width_in_blocks) * DCTSIZE : qint64(expected.width()) * 4;
            const qint64 groupRows = planar ? codec.comp_info[plane].v_samp_factor * DCTSIZE : 1;
            const qint64 visibleRows = planar ? codec.comp_info[plane].downsampled_height : expected.height();
            const qint64 paddedRows = ((visibleRows + groupRows - 1) / groupRows) * groupRows;
            if (stride <= 0 || stride > INT_MAX || paddedRows <= 0 || paddedRows > INT_MAX
                || stride > std::numeric_limits<qsizetype>::max() / paddedRows) {
                result.error = QStringLiteral("JPEG output allocation overflows platform limits");
                return result;
            }
            frame.strides[plane] = int(stride);
            frame.planes[plane].resize(qsizetype(stride * paddedRows));
            allocated += stride * paddedRows;
            state->strides.append(int(stride));
            state->heights.append(paddedRows);
        }
        allocate.finish(result.record, QStringLiteral("output_allocation"));
        const StageClock decode;
        if (planar) {
            JSAMPROW rows[3][2 * DCTSIZE] {};
            JSAMPARRAY planes[3] { rows[0], rows[1], rows[2] };
            while (codec.output_scanline < codec.output_height) {
                if (cancel.load(std::memory_order_relaxed)) {
                    result.error = QStringLiteral("Decode cancelled");
                    return result;
                }
                const JDIMENSION group = codec.output_scanline / (codec.max_v_samp_factor * DCTSIZE);
                for (int plane = 0; plane < 3; ++plane) {
                    const int count = codec.comp_info[plane].v_samp_factor * DCTSIZE;
                    const qsizetype start = qsizetype(group) * count * frame.strides[plane];
                    for (int row = 0; row < count; ++row)
                        rows[plane][row] = reinterpret_cast<JSAMPROW>(
                            frame.planes[plane].data() + start + qsizetype(row) * frame.strides[plane]);
                }
                if (!jpeg_read_raw_data(&codec, planes, codec.max_v_samp_factor * DCTSIZE)) {
                    result.error = QStringLiteral("JPEG raw decode suspended");
                    return result;
                }
            }
        } else {
            auto *base = reinterpret_cast<JSAMPLE *>(frame.planes[0].data());
            JSAMPROW rows[16] {};
            while (codec.output_scanline < codec.output_height) {
                if (cancel.load(std::memory_order_relaxed)) {
                    result.error = QStringLiteral("Decode cancelled");
                    return result;
                }
                const JDIMENSION count = std::min<JDIMENSION>(16, codec.output_height - codec.output_scanline);
                for (JDIMENSION row = 0; row < count; ++row)
                    rows[row] = base + qsizetype(codec.output_scanline + row) * frame.strides[0];
                const JDIMENSION returned = jpeg_read_scanlines(&codec, rows, count);
                if (!returned || returned > count) {
                    result.error = QStringLiteral("JPEG RGB scanline decode suspended or returned invalid row count");
                    return result;
                }
            }
        }
        if (!jpeg_finish_decompress(&codec)) {
            result.error = QStringLiteral("JPEG finish suspended");
            return result;
        }
        decode.finish(result.record, QStringLiteral("pixel_decode"));
        const StageClock teardown;
        jpeg_destroy_decompress(&codec);
        state->created = false;
        teardown.finish(result.record, QStringLiteral("codec_destroy"));
        result.frame = std::move(state->frame);
        result.record.insert("cpu_decoded_allocation_bytes", allocated);
        result.record.insert("strides_bytes", state->strides);
        result.record.insert("allocation_rows", state->heights);
        result.record.insert("idct", QStringLiteral("JDCT_ISLOW"));
        result.record.insert("do_fancy_upsampling", true);
        result.record.insert("do_block_smoothing", true);
        result.record.insert("raw_data_out", planar);
        result.record.insert("rgb_scanline_batch_rows", planar ? QVariant() : QVariant(16));
        result.record.insert(
            "output_color_space", planar ? QStringLiteral("JCS_YCbCr planar") : QStringLiteral("JCS_EXT_BGRA"));
        result.record.insert("alpha",
            planar ? QStringLiteral("shader opaque") : QStringLiteral("BGRA alpha forced 255 by JCS_EXT_BGRA"));
        total.finish(result.record, QStringLiteral("total"));
        return result;
    }
    DecodeResult decodeQt(const QByteArray& bytes, const QSize& expected, const QRect& firstCrop)
    {
        DecodeResult result;
        const StageClock total;
        result.frame = std::make_shared<TrickplayBenchFrame>();
        const StageClock decode;
        result.frame->rgbImage = decodeTrickplayFrame(bytes, firstCrop, expected, true, &result.error);
        decode.finish(result.record, QStringLiteral("production_decode_combined"));
        if (result.frame->rgbImage.isNull()) {
            result.frame.reset();
            return result;
        }
        auto& frame = *result.frame;
        frame.size = frame.rgbImage.size();
        frame.strides[0] = frame.rgbImage.bytesPerLine();
        // Borrow the actual production decoder allocation, retaining its QImage owner.
        frame.planes[0] = QByteArray::fromRawData(
            reinterpret_cast<const char *>(frame.rgbImage.constBits()), frame.rgbImage.sizeInBytes());
        result.record.insert("cpu_decoded_allocation_bytes", frame.rgbImage.sizeInBytes());
        result.record.insert("strides_bytes", QVariantList { frame.strides[0] });
        result.record.insert("allocation_rows", QVariantList { frame.size.height() });
        result.record.insert("header_codec_setup_wall_ns", QVariant());
        result.record.insert("header_codec_setup_thread_cpu_ns", QVariant());
        result.record.insert("output_allocation_wall_ns", QVariant());
        result.record.insert("output_allocation_thread_cpu_ns", QVariant());
        result.record.insert("pixel_decode_wall_ns", QVariant());
        result.record.insert("pixel_decode_thread_cpu_ns", QVariant());
        result.record.insert("stage_limitation",
            QStringLiteral("Production decodeTrickplayFrame exposes only combined header/allocation/decode; no "
                           "invented stage split"));
        result.record.insert("idct", QStringLiteral("Qt JPEG plugin default (not exposed by production decoder)"));
        result.record.insert("output_color_space", QStringLiteral("QImage::Format_RGB32, native byte order"));
        total.finish(result.record, QStringLiteral("total"));
        return result;
    }

    QString option(const QStringList& args, const QString& name)
    {
        for (qsizetype i = 1; i < args.size(); ++i) {
            if (args[i] == name)
                return i + 1 < args.size() ? args[i + 1] : QString();
            if (args[i].startsWith(name + QLatin1Char('=')))
                return args[i].mid(name.size() + 1);
        }
        return {};
    }
    bool hasOption(const QStringList& args, const QString& name)
    {
        return std::any_of(args.cbegin(), args.cend(), [&name](const QString& argument) {
            return argument == name || argument.startsWith(name + QLatin1Char('='));
        });
    }
    int startupError(const QStringList& args, const QString& reason)
    {
        const QByteArray line = QJsonDocument::fromVariant(QVariantMap { { "kind", "error" }, { "message", reason } })
                                    .toJson(QJsonDocument::Compact)
            + '\n';
        const QByteArray output = QByteArrayLiteral("BENCH ") + line;
        std::fwrite(output.constData(), 1, size_t(output.size()), stdout);
        std::fflush(stdout);
        QString path = option(args, QStringLiteral("--bench-log"));
        if (path.isEmpty())
            path = QDir(persistentDataRoot()).filePath(QStringLiteral("trickplay-benchmark-startup-error.jsonl"));
        if (QDir().mkpath(QFileInfo(path).absolutePath())) {
            QFile file(path);
            if (file.open(QIODevice::WriteOnly | QIODevice::Append)) {
                file.write(line);
                file.flush();
            }
        }
        return 2;
    }
    QVariantMap rhiStatistics(const QVariantMap& sample)
    {
        QVariantMap result;
        for (auto it = sample.cbegin(); it != sample.cend(); ++it) {
            if (it.key().startsWith(QStringLiteral("rhi_")))
                result.insert(it.key(), it.value());
        }
        return result;
    }
    QVariant counterDelta(const QVariant& value, const QVariant& baseline)
    {
        return !value.isValid() || value.isNull() || !baseline.isValid() || baseline.isNull()
            ? QVariant()
            : QVariant(value.toDouble() - baseline.toDouble());
    }
    QVariantMap summary(std::vector<double> values)
    {
        if (values.empty())
            return { { "count", 0 }, { "median", QVariant() }, { "p95", QVariant() }, { "max", QVariant() } };
        std::sort(values.begin(), values.end());
        const size_t count = values.size();
        const double median = count % 2 ? values[count / 2] : (values[count / 2 - 1] + values[count / 2]) / 2;
        return { { "count", qulonglong(count) }, { "median", median },
            { "p95", values[size_t(std::ceil(count * .95)) - 1] }, { "max", values.back() } };
    }
    QVariantMap summarizeRecords(const std::vector<QVariantMap>& records, const QString& kind, const QString& phase)
    {
        QMap<QString, std::vector<double>> columns;
        for (const auto& record : records) {
            if (record.value("kind").toString() != kind
                || (!phase.isEmpty() && record.value("phase").toString() != phase))
                continue;
            for (auto it = record.cbegin(); it != record.cend(); ++it) {
                if ((!it.key().endsWith(QStringLiteral("_ns")) && !it.key().endsWith(QStringLiteral("_ms")))
                    || !it.value().isValid() || it.value().isNull())
                    continue;
                bool ok = false;
                const double value = it.value().toDouble(&ok);
                if (ok)
                    columns[it.key()].push_back(value);
            }
        }
        QVariantMap result;
        for (auto it = columns.cbegin(); it != columns.cend(); ++it)
            result.insert(it.key(), summary(it.value()));
        return result;
    }
    QVariantMap timingColumns(const QVariantMap& record)
    {
        QVariantMap result { { "kind", record.value("kind") }, { "phase", record.value("phase") } };
        for (auto it = record.cbegin(); it != record.cend(); ++it) {
            if (!it.key().endsWith(QStringLiteral("_ns")) && !it.key().endsWith(QStringLiteral("_ms")))
                continue;
            const int type = it.value().userType();
            if (!it.value().isValid() || it.value().isNull() || type == QMetaType::Double || type == QMetaType::LongLong
                || type == QMetaType::ULongLong || type == QMetaType::Int || type == QMetaType::UInt)
                result.insert(it.key(), it.value());
        }
        return result;
    }

    class Benchmark final : public QObject {
    public:
        Benchmark(QGuiApplication& app, NativeAppWindow& window, TrickplayBenchmarkItem& item, QQuickItem& caption)
            : QObject(&app)
            , m_app(app)
            , m_window(window)
            , m_item(item)
            , m_caption(caption)
            , m_worker(new QObject)
        {
            m_worker->moveToThread(&m_decodeThread);
            connect(&m_decodeThread, &QThread::finished, m_worker, &QObject::deleteLater);
            m_decodeThread.start();
            connect(
                &m_item, &TrickplayBenchmarkItem::sampleReady, this,
                [this](QVariantMap sample) {
                    if (!m_pending || sample.value("serial").toULongLong() != m_serial)
                        return;
                    m_sample = std::move(sample);
                    m_haveSample = true;
                    completeFrame();
                },
                Qt::QueuedConnection);
            connect(
                &m_item, &TrickplayBenchmarkItem::gpuSampleReady, this,
                [this](QVariantMap sample) { receiveGpuSample(std::move(sample)); }, Qt::QueuedConnection);
            connect(
                &m_item, &TrickplayBenchmarkItem::renderFailure, this, [this](const QString& reason) { fail(reason); },
                Qt::QueuedConnection);
            // Snapshot the submitted serial at scenegraph synchronization, not at
            // swap: an older frame can swap while the GUI submits the next request.
            connect(
                &m_window, &QQuickWindow::beforeSynchronizing, this,
                [this] {
                    m_renderSerial.store(m_submittedSerial.load(std::memory_order_acquire), std::memory_order_release);
                },
                Qt::DirectConnection);
            connect(
                &m_window, &QQuickWindow::frameSwapped, this,
                [this] {
                    const quint64 serial = m_renderSerial.load(std::memory_order_acquire);
                    const qint64 swapTime = nowNs();
                    QMetaObject::invokeMethod(
                        this,
                        [this, serial, swapTime] {
                            if (!m_firstSwap) {
                                m_firstSwap = true;
                                QTimer::singleShot(1000, this, [this] { start(); });
                            }
                            if (!m_pending || serial != m_serial || m_haveSwap)
                                return;
                            m_swapNs = swapTime;
                            m_haveSwap = true;
                            completeFrame();
                        },
                        Qt::QueuedConnection);
                },
                Qt::DirectConnection);
            m_watchdog.setInterval(1000);
            connect(&m_watchdog, &QTimer::timeout, this, [this] {
                if (!m_finished && nowNs() - m_lastProgress > 60000000000LL)
                    fail(QStringLiteral("Benchmark stalled for 60 seconds (decode or onscreen frame acknowledgement)"));
            });
            m_watchdog.start();
            connect(&m_window, &NativeAppWindow::closeRequested, this, [this] { abort(); });
            connect(&m_window, &NativeAppWindow::platformCloseRequested, this, [this] { abort(); });
            connect(&m_app, &QCoreApplication::aboutToQuit, this, [this] {
                m_cancel.store(true);
                m_memory.stop();
                m_decodeThread.quit();
                if (!m_decodeThread.wait(5000)) {
                    write({ { "kind", "error" },
                        { "message", "Decoder failed to stop within five seconds; bounded emergency exit" } });
                    std::_Exit(2);
                }
            });
        }
        ~Benchmark() override
        {
            disconnect(&m_window, nullptr, this, nullptr);
            disconnect(&m_item, nullptr, this, nullptr);
            m_watchdog.stop();
            m_memory.stop();
            m_cancel.store(true);
            m_decodeThread.quit();
            if (!m_decodeThread.wait(5000))
                std::_Exit(2);
        }
        bool initialize()
        {
            const auto args = m_app.arguments();
            m_freshTextures = args.contains(QStringLiteral("--bench-fresh-textures"));
            m_autoExit = args.contains(QStringLiteral("--bench-auto-exit"));
            const QString seedText = option(args, QStringLiteral("--bench-seed"));
            if (hasOption(args, QStringLiteral("--bench-seed")) && seedText.isEmpty())
                return fail(QStringLiteral("--bench-seed requires an unsigned 32-bit integer"));
            if (!seedText.isEmpty()) {
                bool ok = false;
                const qulonglong parsed = seedText.toULongLong(&ok);
                if (!ok || parsed > std::numeric_limits<quint32>::max())
                    return fail(QStringLiteral("--bench-seed must be an unsigned 32-bit integer"));
                m_seed = quint32(parsed);
            } else {
                m_seed = std::random_device {}();
            }
            m_logPath = option(args, QStringLiteral("--bench-log"));
            if (hasOption(args, QStringLiteral("--bench-log")) && m_logPath.isEmpty())
                return fail(QStringLiteral("--bench-log requires a path"));
            if (hasOption(args, QStringLiteral("--bench-screenshot-dir"))
                && option(args, QStringLiteral("--bench-screenshot-dir")).isEmpty())
                return fail(QStringLiteral("--bench-screenshot-dir requires a path"));
            if (m_logPath.isEmpty()) {
                QString root = persistentDataRoot();
                if (root.isEmpty())
                    root = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
                m_logPath = QDir(root).filePath(QStringLiteral("trickplay-benchmark-%1.jsonl")
                        .arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-hhmmss-zzz"))));
            }
            if (!QDir().mkpath(QFileInfo(m_logPath).absolutePath()))
                return fail(QStringLiteral("Could not create benchmark log directory: ") + m_logPath);
            m_log.setFileName(m_logPath);
            if (!m_log.open(QIODevice::WriteOnly | QIODevice::Truncate))
                return fail(QStringLiteral("Could not open benchmark log: ") + m_log.errorString());
            write({ { "kind", "startup" }, { "log_path", m_logPath }, { "seed", m_seed }, { "warmup_runs_per_case", 3 },
                { "measured_runs_per_case", 3 }, { "cold_decodes_per_run", ColdCount },
                { "warm_crop_frames_per_run", WarmCount }, { "startup_delay_after_first_swap_ms", 1000 },
                { "fresh_textures", m_freshTextures },
                { "cold_texture_mode",
                    m_freshTextures ? "fresh textures: clear/trim between replacement loads"
                                    : "reuse textures between replacement loads" },
                { "replacement_memory_scope",
                    m_freshTextures ? "decoded previous output released before next decode"
                                    : "old presented source and newly decoding output may temporarily overlap; peak "
                                      "RSS includes this replacement overlap" } });
            setCaption(QStringLiteral(
                "Trickplay benchmark\nPriming blank Qt/text scene before memory baseline…\nSeed %1\nLog: %2")
                    .arg(m_seed)
                    .arg(m_logPath));
            return true;
        }
        void surfaceFailure(const QString& reason)
        {
            fail(reason);
        }

    private:
        static constexpr int ColdCount = 12;
        static constexpr int WarmCount = 120;
        struct Run {
            QString name;
            bool measured;
            int repetition;
        };
        enum class Action {
            ColdBlank,
            ColdFrame,
            WarmFrame,
            DrainOne,
            DrainTwo,
            InitialDrain,
            VerifyFrame,
            VerifyWarmFrame,
            VerifyDrainOne,
            VerifyDrainTwo
        };
        void setCaption(const QString& value)
        {
            m_caption.setProperty("text", value);
        }
        void write(const QVariantMap& record)
        {
            const QByteArray line = QJsonDocument::fromVariant(record).toJson(QJsonDocument::Compact) + '\n';
            if (m_log.isOpen()) {
                if (m_log.write(line) != line.size())
                    m_logFailed = true;
                if (!m_log.flush())
                    m_logFailed = true;
            }
            const QByteArray output = QByteArrayLiteral("BENCH ") + line;
            std::fwrite(output.constData(), 1, size_t(output.size()), stdout);
            std::fflush(stdout);
        }
        // Serialize and flush only outside all run timing, not on every timed frame.
        void flushRecords()
        {
            flushBuffer(m_records);
        }
        void flushBuffer(const std::vector<QVariantMap>& records)
        {
            QByteArray fileBytes;
            QByteArray stdoutBytes;
            for (const auto& record : records) {
                const QByteArray line = QJsonDocument::fromVariant(record).toJson(QJsonDocument::Compact) + '\n';
                fileBytes.append(line);
                stdoutBytes.append(QByteArrayLiteral("BENCH ") + line);
            }
            if (m_log.isOpen() && (m_log.write(fileBytes) != fileBytes.size() || !m_log.flush()))
                m_logFailed = true;
            std::fwrite(stdoutBytes.constData(), 1, size_t(stdoutBytes.size()), stdout);
            std::fflush(stdout);
        }
        void receiveGpuSample(QVariantMap sample)
        {
            const quint64 serial = sample.value("serial").toULongLong();
            const auto origin = m_gpuOrigins.find(serial);
            if (!serial || origin == m_gpuOrigins.end())
                return; // Priming, blank, unrequested, and verification frames are excluded.
            QVariantMap record = std::move(origin.value());
            m_gpuOrigins.erase(origin);
            for (auto it = sample.cbegin(); it != sample.cend(); ++it)
                record.insert(it.key(), it.value());
            record.insert("kind", QStringLiteral("gpu_frame"));
            record.insert("gpu_time_semantics",
                QStringLiteral(
                    "Exact originating requested serial; whole Qt GPU frame, not isolated upload or preview draw."));
            m_gpuRecords.push_back(std::move(record));
        }
        void finishGpuRun()
        {
            // Vulkan reads the previous use of each of its two frame slots.
            // Both actual blank swaps have completed, so the last two timed
            // requests have had a chance to publish their nonblocking queries.
            quint64 unmatched = 0;
            for (auto it = m_gpuOrigins.begin(); it != m_gpuOrigins.end();) {
                if (it.value().value("run_index").toInt() == m_runIndex) {
                    ++unmatched;
                    it = m_gpuOrigins.erase(it);
                } else {
                    ++it;
                }
            }
            auto record = tag(QStringLiteral("gpu_run_summary"));
            record.insert("origin_attribution_supported", m_sample.value("gpu_origin_attribution_supported"));
            record.insert("gpu_timestamps_supported", m_sample.value("gpu_timestamps_supported"));
            record.insert("requested_timed_frames", ColdCount + WarmCount);
            record.insert("gpu_event_count", qulonglong(m_gpuRecords.size()));
            record.insert("pending_origins_after_two_drains", unmatched);
            record.insert(
                "requests_without_exact_gpu_event", qint64(ColdCount + WarmCount) - qint64(m_gpuRecords.size()));
            record.insert("scope",
                QStringLiteral(
                    "Whole Qt GPU frame, exact originating serial on Vulkan; no isolated upload/draw estimate."));
            for (const QString& phase : { QStringLiteral("cold"), QStringLiteral("warm") }) {
                std::vector<double> values;
                quint64 events = 0;
                for (const auto& raw : m_gpuRecords) {
                    if (raw.value("phase").toString() != phase)
                        continue;
                    ++events;
                    if (raw.value("gpu_frame_ms").isValid() && !raw.value("gpu_frame_ms").isNull())
                        values.push_back(raw.value("gpu_frame_ms").toDouble());
                }
                record.insert(
                    phase, QVariantMap { { "event_count", events }, { "gpu_frame_ms", summary(std::move(values)) } });
            }
            if (m_runs[size_t(m_runIndex)].measured) {
                auto& aggregate = m_gpuAggregates[m_runs[size_t(m_runIndex)].name];
                for (const auto& raw : m_gpuRecords)
                    aggregate.push_back(timingColumns(raw));
            }
            m_gpuRecords.push_back(std::move(record));
            flushBuffer(m_gpuRecords);
            m_gpuRecords.clear();
            if (m_logFailed)
                fail(QStringLiteral("Writing the exact-origin GPU benchmark log failed"));
        }
        bool fail(const QString& reason)
        {
            if (m_finished)
                return false;
            m_finished = true;
            m_pending = false;
            m_watchdog.stop();
            m_cancel.store(true);
            m_memory.stop();
            flushRecords();
            flushBuffer(m_gpuRecords);
            write({ { "kind", "error" }, { "message", reason }, { "run_index", m_runIndex },
                { "memory", memoryMap(readMemory()) } });
            setCaption(QStringLiteral("Benchmark FAILED\n%1\nLog: %2").arg(reason, m_logPath));
            QTimer::singleShot(0, &m_app, [this] { m_app.exit(2); });
            return false;
        }
        void abort()
        {
            if (!m_finished)
                fail(QStringLiteral("Window closed before benchmark completed"));
            else
                m_app.quit();
        }
        void start()
        {
            if (m_finished)
                return;
            // Nothing from the JPEG fixture or decoded output is resident at baseline.
            trimAllocator();
            m_globalBaseline = readMemory();
            QFile manifestFile(QStringLiteral(":/bench/fixture.json"));
            QFile jpegFile(QStringLiteral(":/bench/trickplay-p95.jpg"));
            if (!manifestFile.open(QIODevice::ReadOnly) || !jpegFile.open(QIODevice::ReadOnly)) {
                fail(QStringLiteral("Embedded JPEG fixture or manifest is missing"));
                return;
            }
            QJsonParseError parseError;
            const QJsonDocument document = QJsonDocument::fromJson(manifestFile.readAll(), &parseError);
            if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
                fail(QStringLiteral("Fixture manifest is invalid JSON"));
                return;
            }
            m_manifest = document.object().toVariantMap();
            m_sheet = QSize(m_manifest.value("sheetWidth").toInt(), m_manifest.value("sheetHeight").toInt());
            m_thumbnail
                = QSize(m_manifest.value("thumbnailWidth").toInt(), m_manifest.value("thumbnailHeight").toInt());
            m_columns = m_manifest.value("columns").toInt();
            m_rows = m_manifest.value("rows").toInt();
            if (!m_sheet.isValid() || !m_thumbnail.isValid() || m_columns <= 0 || m_rows <= 0
                || qint64(m_columns) * m_thumbnail.width() > m_sheet.width()
                || qint64(m_rows) * m_thumbnail.height() > m_sheet.height()
                || qint64(m_sheet.width()) * m_sheet.height() > TrickplayDecodedByteBudget / 4) {
                fail(QStringLiteral("Fixture must have valid tiles and fit the production resident RGB budget for a "
                                    "fair full-sheet comparison"));
                return;
            }
            m_item.setWidth(m_thumbnail.width());
            m_item.setHeight(m_thumbnail.height());
            m_caption.setY(m_item.y() + m_item.height() + 24);
            m_caption.setHeight(std::max(qreal(250), m_window.height() - m_caption.y() - 24));
            m_jpeg = jpegFile.readAll();
            if (m_jpeg.isEmpty() || quint64(m_jpeg.size()) > std::numeric_limits<unsigned long>::max()) {
                fail(QStringLiteral("Fixture bytes are empty or exceed libjpeg source size limits"));
                return;
            }
            const QString hash
                = QString::fromLatin1(QCryptographicHash::hash(m_jpeg, QCryptographicHash::Sha256).toHex());
            if (m_manifest.value("sha256").toString().isEmpty()
                || m_manifest.value("sha256").toString().compare(hash, Qt::CaseInsensitive) != 0) {
                fail(QStringLiteral("Embedded fixture SHA-256 does not match manifest"));
                return;
            }
            // Use a fixed tile trace independent of case/run order and random seed.
            const qint64 tileCount = qint64(m_columns) * m_rows;
            for (int index = 0; index < ColdCount + WarmCount; ++index) {
                const qint64 tile = (qint64(index) * 37 + 11) % tileCount;
                m_trace.emplace_back(int(tile % m_columns) * m_thumbnail.width(),
                    int(tile / m_columns) * m_thumbnail.height(), m_thumbnail.width(), m_thumbnail.height());
            }
            QString cpuModel;
#if defined(Q_OS_LINUX)
            QFile cpuInfo(QStringLiteral("/proc/cpuinfo"));
            if (cpuInfo.open(QIODevice::ReadOnly)) {
                const auto lines = cpuInfo.readAll().split('\n');
                for (const auto& line : lines) {
                    if (line.startsWith("model name") || line.startsWith("Hardware")) {
                        cpuModel = QString::fromLocal8Bit(line.mid(line.indexOf(':') + 1).trimmed());
                        break;
                    }
                }
            }
#endif
            QVariantMap metadata { { "kind", "metadata" }, { "fixture", m_manifest }, { "fixture_sha256", hash },
                { "jpeg_compressed_bytes", m_jpeg.size() }, { "qt_version", QString::fromLatin1(qVersion()) },
                { "os", QSysInfo::prettyProductName() }, { "kernel", QSysInfo::kernelVersion() },
                { "cpu_architecture", QSysInfo::currentCpuArchitecture() },
                { "cpu_model", cpuModel.isEmpty() ? QVariant() : QVariant(cpuModel) },
                { "logical_cpu_count", QThread::idealThreadCount() }, { "qpa", QGuiApplication::platformName() },
                { "jpeg_library_api_version", JPEG_LIB_VERSION }, { "jpeg_sample_bits", BITS_IN_JSAMPLE },
                { "thread_cpu_supported", threadCpuNs() >= 0 },
                { "global_pre_fixture_baseline", memoryMap(m_globalBaseline) },
                { "fixture_resident_memory", memoryMap(readMemory()) }, { "memory_sample_interval_requested_ms", 1 },
                { "memory_notes",
                    "RSS is process resident memory, NOT VRAM. Peak sampling is a lower bound; VmHWM is cumulative. "
                    "Blank Qt/text/rendering is primed before fixed baseline, but allocator/codec/driver caches and "
                    "retained timing scalar records still warm/grow across runs. malloc_trim is applied outside "
                    "timings between runs, and between cold loads only in --bench-fresh-textures mode. Default "
                    "replacement loads may temporarily retain both the previous presented output and new decode." },
                { "gpu_isolated_upload_ms", QVariant() }, { "gpu_isolated_draw_ms", QVariant() },
                { "gpu_notes",
                    "No readbacks, fences, blocking GPU waits or custom timer queries are introduced. Legacy "
                    "last-completed whole-frame GPU time has unknown origin. Separate gpu_frame/gpu_aggregate "
                    "records attribute Qt's existing Vulkan frame-slot queries to exact requested serials; "
                    "they still measure the whole Qt GPU frame, never isolated upload or preview draw." },
                { "qt_decode_notes",
                    "Actual production decodeTrickplayFrame, resident sheet, valid thumbnail crop. Qt stage splits and "
                    "IDCT choice are not exposed; combined time and native RGB32 allocation are measured." },
                { "direct_jpeg_flags",
                    "normal libjpeg API; JDCT_ISLOW, fancy upsampling=true, block smoothing=true; RGB=JCS_EXT_BGRA, "
                    "YUV=raw_data_out JCS_YCbCr" },
                { "jpeg_simd_environment", QString::fromLocal8Bit(qgetenv("JSIMD_FORCENONE")) },
                { "display_device_pixel_ratio", m_window.devicePixelRatio() }, { "window_width", m_window.width() },
                { "window_height", m_window.height() }, { "preview_width", m_item.width() },
                { "preview_height", m_item.height() },
                { "jpeg_yuv_range", "full range; opaque alpha; renderer bilinear sampling" },
                { "surface_color_mode",
                    "SDR swapchain requested; HDR startup request disabled for every path; no offscreen HDR conversion "
                    "pass" } };
#ifdef LIBJPEG_TURBO_VERSION
#define BENCH_STRINGIFY_IMPL(value) #value
#define BENCH_STRINGIFY(value) BENCH_STRINGIFY_IMPL(value)
            metadata.insert("libjpeg_turbo_version", QStringLiteral(BENCH_STRINGIFY(LIBJPEG_TURBO_VERSION)));
#undef BENCH_STRINGIFY
#undef BENCH_STRINGIFY_IMPL
#else
            metadata.insert("libjpeg_turbo_version", QVariant());
#endif
            write(metadata);
            const QStringList names { QStringLiteral("qt-rgb32"), QStringLiteral("jpeg-rgb32"),
                QStringLiteral("jpeg-yuv420") };
            for (const auto& name : names)
                for (int repetition = 1; repetition <= 3; ++repetition)
                    m_runs.push_back({ name, false, repetition });
            std::vector<Run> measured;
            for (const auto& name : names)
                for (int repetition = 1; repetition <= 3; ++repetition)
                    measured.push_back({ name, true, repetition });
            std::mt19937 generator(m_seed);
            std::shuffle(measured.begin(), measured.end(), generator);
            QVariantList order;
            for (const auto& run : measured) {
                m_runs.push_back(run);
                order.append(QVariantMap { { "case", run.name }, { "repetition", run.repetition } });
            }
            QVariantList trace;
            for (const auto& crop : m_trace)
                trace.append(QVariantMap {
                    { "x", crop.x() }, { "y", crop.y() }, { "width", crop.width() }, { "height", crop.height() } });
            write({ { "kind", "schedule" }, { "seed", m_seed }, { "measured_order", order },
                { "fixed_tile_trace", trace },
                { "warmup_order",
                    "three complete qt-rgb32, then three complete jpeg-rgb32, then three complete jpeg-yuv420" } });
            m_action = Action::InitialDrain;
            submitBlank();
        }
        QVariantMap tag(QString kind, QString phase = {}) const
        {
            const auto& run = m_runs[size_t(m_runIndex)];
            return { { "kind", kind }, { "case", run.name }, { "measured", run.measured },
                { "repetition", run.repetition }, { "run_index", m_runIndex }, { "phase", phase } };
        }
        void beginRun()
        {
            if (m_finished)
                return;
            ++m_runIndex;
            if (m_runIndex >= int(m_runs.size())) {
                finish();
                return;
            }
            trimAllocator();
            m_runBaseline = readMemory();
            m_rhiRunBaseline = rhiStatistics(m_sample);
            m_memory.reset(m_runBaseline);
            m_records.clear();
            m_records.reserve(ColdCount * 2 + WarmCount + 4);
            m_gpuRecords.reserve(ColdCount + WarmCount + 1);
            m_cold = 0;
            m_warm = 0;
            m_runStartNs = nowNs();
            auto record = tag(QStringLiteral("run_start"));
            record.insert("memory", memoryMap(m_runBaseline));
            record.insert("baseline_rss_delta_global_bytes", delta(m_runBaseline.rss, m_globalBaseline.rss));
            record.insert("rhi_blank_baseline", m_rhiRunBaseline);
            m_records.push_back(std::move(record));
            requestDecode();
        }
        void requestDecode()
        {
            if (m_finished)
                return;
            const auto run = m_runs[size_t(m_runIndex)];
            m_selectionNs = nowNs();
            setCaption(QStringLiteral(
                "%1 — %2 run %3/3\nCold decode/upload %4/%5\nSeed %6 · run %7/18\nPrevious submit→swap %8 ms\nLog: %9")
                    .arg(run.name, run.measured ? QStringLiteral("MEASURED") : QStringLiteral("WARM-UP"))
                    .arg(run.repetition)
                    .arg(m_cold + 1)
                    .arg(ColdCount)
                    .arg(m_seed)
                    .arg(m_runIndex + 1)
                    .arg(m_lastLatencyMs, 0, 'f', 3)
                    .arg(m_logPath));
            m_lastProgress = nowNs();
            const QByteArray bytes = m_jpeg; // shared immutable compressed backing
            const QSize size = m_sheet;
            const QRect crop = m_trace[size_t(m_cold)];
            const qint64 dispatchNs = nowNs();
            QMetaObject::invokeMethod(
                m_worker,
                [this, name = run.name, bytes, size, crop, dispatchNs] {
                    const qint64 workerStartNs = nowNs();
                    DecodeResult result = name == QStringLiteral("qt-rgb32")
                        ? decodeQt(bytes, size, crop)
                        : decodeJpeg(bytes, size, name == QStringLiteral("jpeg-yuv420"), m_cancel);
                    const qint64 workerReturnNs = nowNs();
                    QMetaObject::invokeMethod(
                        this,
                        [this, result = std::move(result), dispatchNs, workerStartNs, workerReturnNs]() mutable {
                            const qint64 guiReturnNs = nowNs();
                            if (m_finished)
                                return;
                            if (!result.error.isEmpty() || !result.frame) {
                                fail(result.error.isEmpty() ? QStringLiteral("Decoder returned no frame")
                                                            : result.error);
                                return;
                            }
                            auto record = tag(QStringLiteral("decode"), QStringLiteral("cold"));
                            record.insert("selection_index", m_cold);
                            m_workerQueueNs = workerStartNs - dispatchNs;
                            m_workerHandoffNs = guiReturnNs - workerReturnNs;
                            record.insert("gui_request_to_worker_queue_ns", m_workerQueueNs);
                            record.insert("worker_return_to_gui_ns", m_workerHandoffNs);
                            for (auto it = result.record.cbegin(); it != result.record.cend(); ++it)
                                record.insert(it.key(), it.value());
                            record.insert("memory_after_decode", memoryMap(readMemory()));
                            m_records.push_back(std::move(record));
                            m_frame = std::move(result.frame);
                            m_action = Action::ColdFrame;
                            submitFrame(m_trace[size_t(m_cold)]);
                        },
                        Qt::QueuedConnection);
                },
                Qt::QueuedConnection);
        }
        void prepareSubmit()
        {
            m_pending = true;
            m_haveSample = false;
            m_haveSwap = false;
            m_sample.clear();
            ++m_serial;
            m_submitNs = nowNs();
            m_lastProgress = m_submitNs;
            m_submittedSerial.store(m_serial, std::memory_order_release);
        }
        void submitBlank()
        {
            prepareSubmit();
            m_frame.reset();
            m_item.clearFrame(m_serial);
            m_window.update();
        }
        void submitFrame(const QRect& crop)
        {
            prepareSubmit();
            if (m_action == Action::WarmFrame)
                m_selectionNs = m_submitNs;
            if (m_gpuOriginAttributionSupported && (m_action == Action::ColdFrame || m_action == Action::WarmFrame)) {
                auto origin = tag(QStringLiteral("gpu_frame"),
                    m_action == Action::ColdFrame ? QStringLiteral("cold") : QStringLiteral("warm"));
                origin.insert("selection_index", m_action == Action::ColdFrame ? m_cold : m_warm);
                origin.insert("serial", m_serial);
                m_gpuOrigins.insert(m_serial, std::move(origin));
            }
            m_item.setFrame(m_frame, crop, m_serial);
            m_window.update();
        }
        void completeFrame()
        {
            if (!m_pending || !m_haveSample || !m_haveSwap || m_finished)
                return;
            m_pending = false;
            m_lastProgress = nowNs();
            if (m_action == Action::ColdFrame || m_action == Action::WarmFrame) {
                auto record = tag(QStringLiteral("frame"),
                    m_action == Action::ColdFrame ? QStringLiteral("cold") : QStringLiteral("warm"));
                for (auto it = m_sample.cbegin(); it != m_sample.cend(); ++it)
                    record.insert(it.key(), it.value());
                record.insert("selection_index", m_action == Action::ColdFrame ? m_cold : m_warm);
                record.insert("submit_to_frame_swapped_ns", m_swapNs - m_submitNs);
                record.insert("selection_to_frame_swapped_ns", m_swapNs - m_selectionNs);
                record.insert("gui_request_to_worker_queue_ns",
                    m_action == Action::ColdFrame ? QVariant(m_workerQueueNs) : QVariant());
                record.insert("worker_return_to_gui_ns",
                    m_action == Action::ColdFrame ? QVariant(m_workerHandoffNs) : QVariant());
                record.insert("frame_swap_semantics",
                    "Qt frameSwapped acknowledgement, includes scenegraph scheduling/presentation/vsync; not GPU "
                    "execution time");
                m_lastLatencyMs = double(m_swapNs - m_submitNs) / 1000000;
                if (m_action == Action::WarmFrame && m_sample.value("uploaded").toBool()) {
                    fail(QStringLiteral("Renderer uploaded pixels during crop-only warm phase"));
                    return;
                }
                if (m_action == Action::ColdFrame && !m_sample.value("uploaded").toBool()) {
                    fail(QStringLiteral("Renderer did not upload the newly decoded cold frame"));
                    return;
                }
                m_records.push_back(std::move(record));
            }
            switch (m_action) {
            case Action::InitialDrain:
                m_rhiGlobalBaseline = rhiStatistics(m_sample);
                m_gpuOriginAttributionSupported = m_sample.value("gpu_origin_attribution_supported").toBool();
                write({ { "kind", "graphics_metadata" }, { "backend", m_sample.value("backend") },
                    { "device", m_sample.value("device") },
                    { "gpu_timestamps_supported", m_sample.value("gpu_timestamps_supported") },
                    { "gpu_origin_attribution_supported", m_gpuOriginAttributionSupported },
                    { "driver", m_sample.value("driver") },
                    { "driver_note", "Driver is null unless renderer API exposes it; no version is inferred." },
                    { "rhi_fixed_blank_baseline_before_test_textures", m_rhiGlobalBaseline } });
                if (m_logFailed) {
                    fail(QStringLiteral("Initial metadata log write failed"));
                    return;
                }
                beginRun();
                break;
            case Action::ColdBlank:
                trimAllocator();
                requestDecode();
                break;
            case Action::ColdFrame:
                if (++m_cold < ColdCount) {
                    if (m_freshTextures) {
                        m_action = Action::ColdBlank;
                        submitBlank();
                    } else {
                        requestDecode();
                    }
                } else {
                    m_action = Action::WarmFrame;
                    setCaption(QStringLiteral("%1 — %2 run %3/3\nWarm crop-only trace: %4 frames, no re-upload\nCold "
                                              "final submit→swap: %5 ms\nSeed %6 · run %7/18\nLog: %8")
                            .arg(m_runs[size_t(m_runIndex)].name,
                                m_runs[size_t(m_runIndex)].measured ? QStringLiteral("MEASURED")
                                                                    : QStringLiteral("WARM-UP"))
                            .arg(m_runs[size_t(m_runIndex)].repetition)
                            .arg(WarmCount)
                            .arg(m_lastLatencyMs, 0, 'f', 3)
                            .arg(m_seed)
                            .arg(m_runIndex + 1)
                            .arg(m_logPath));
                    submitFrame(m_trace[size_t(ColdCount)]);
                }
                break;
            case Action::WarmFrame:
                if (++m_warm < WarmCount) {
                    submitFrame(m_trace[size_t(ColdCount + m_warm)]);
                } else {
                    finishRun();
                    if (m_finished)
                        return;
                    m_action = Action::DrainOne;
                    submitBlank();
                }
                break;
            case Action::DrainOne:
                m_action = Action::DrainTwo;
                submitBlank();
                break;
            case Action::DrainTwo:
                trimAllocator();
                {
                    auto record = tag(QStringLiteral("run_drained"));
                    record.insert("memory_after_gpu_source_release_and_trim", memoryMap(readMemory()));
                    record.insert("rhi_after_gpu_source_release", rhiStatistics(m_sample));
                    write(record);
                }
                finishGpuRun();
                if (m_finished)
                    return;
                beginRun();
                break;
            case Action::VerifyFrame:
                saveVerification();
                break;
            case Action::VerifyWarmFrame:
                saveVerification();
                break;
            case Action::VerifyDrainOne:
                m_action = Action::VerifyDrainTwo;
                submitBlank();
                break;
            case Action::VerifyDrainTwo:
                trimAllocator();
                requestVerification();
                break;
            }
        }
        void finishRun()
        {
            const Memory after = readMemory();
            const auto [peak, samples] = m_memory.peak();
            auto record = tag(QStringLiteral("run_summary"));
            record.insert("run_wall_including_untimed_cold_clear_decode_dispatch_ns", nowNs() - m_runStartNs);
            record.insert("decode", summarizeRecords(m_records, QStringLiteral("decode"), {}));
            record.insert("cold_frames", summarizeRecords(m_records, QStringLiteral("frame"), QStringLiteral("cold")));
            record.insert("warm_frames", summarizeRecords(m_records, QStringLiteral("frame"), QStringLiteral("warm")));
            record.insert("memory_before", memoryMap(m_runBaseline));
            record.insert("memory_after_before_drain", memoryMap(after));
            record.insert("memory_sampled_peak", memoryMap(peak));
            record.insert("memory_sample_count", samples);
            record.insert("rss_peak_delta_run_bytes", delta(peak.rss, m_runBaseline.rss));
            record.insert("rss_peak_delta_global_bytes", delta(peak.rss, m_globalBaseline.rss));
            record.insert("rss_after_delta_run_bytes", delta(after.rss, m_runBaseline.rss));
            record.insert("rss_after_delta_global_bytes", delta(after.rss, m_globalBaseline.rss));
            record.insert("anonymous_peak_delta_run_bytes", delta(peak.anonymous, m_runBaseline.anonymous));
            record.insert("anonymous_peak_delta_global_bytes", delta(peak.anonymous, m_globalBaseline.anonymous));
            record.insert("anonymous_after_delta_run_bytes", delta(after.anonymous, m_runBaseline.anonymous));
            record.insert("anonymous_after_delta_global_bytes", delta(after.anonymous, m_globalBaseline.anonymous));
            record.insert("requested_gpu_texture_bytes", m_sample.value("gpu_texture_bytes"));
            QVariantMap rhiPeaks;
            const QStringList rhiKeys { "rhi_allocator_used_bytes", "rhi_allocator_unused_bytes",
                "rhi_allocator_allocated_bytes", "rhi_allocator_block_count", "rhi_allocator_allocation_count",
                "rhi_d3d12_total_usage_bytes" };
            for (const auto& key : rhiKeys) {
                QVariant maximum;
                for (const auto& frame : m_records) {
                    if (frame.value("kind").toString() != QStringLiteral("frame") || !frame.value(key).isValid()
                        || frame.value(key).isNull())
                        continue;
                    if (!maximum.isValid() || frame.value(key).toULongLong() > maximum.toULongLong())
                        maximum = frame.value(key);
                }
                rhiPeaks.insert(key, maximum);
            }
            record.insert("rhi_fixed_blank_baseline", m_rhiGlobalBaseline);
            record.insert("rhi_run_blank_baseline", m_rhiRunBaseline);
            record.insert("rhi_frame_sampled_peak", rhiPeaks);
            record.insert("rhi_allocator_stats_supported", m_sample.value("rhi_allocator_stats_supported"));
            record.insert("rhi_allocator_scope", m_sample.value("rhi_allocator_scope"));
            record.insert("rhi_allocator_peak_used_bytes", rhiPeaks.value("rhi_allocator_used_bytes"));
            record.insert("rhi_allocator_peak_allocated_bytes", rhiPeaks.value("rhi_allocator_allocated_bytes"));
            record.insert("rhi_allocator_peak_used_delta_global_bytes",
                counterDelta(
                    rhiPeaks.value("rhi_allocator_used_bytes"), m_rhiGlobalBaseline.value("rhi_allocator_used_bytes")));
            record.insert("rhi_allocator_peak_used_delta_run_bytes",
                counterDelta(
                    rhiPeaks.value("rhi_allocator_used_bytes"), m_rhiRunBaseline.value("rhi_allocator_used_bytes")));
            record.insert("rhi_allocator_peak_allocated_delta_global_bytes",
                counterDelta(rhiPeaks.value("rhi_allocator_allocated_bytes"),
                    m_rhiGlobalBaseline.value("rhi_allocator_allocated_bytes")));
            record.insert("rhi_d3d12_peak_total_usage_bytes", rhiPeaks.value("rhi_d3d12_total_usage_bytes"));
            record.insert("rhi_pipeline_creation_run_delta_ms",
                counterDelta(m_sample.value("rhi_total_pipeline_creation_ms"),
                    m_rhiRunBaseline.value("rhi_total_pipeline_creation_ms")));
            record.insert("rhi_statistics_notes",
                "Whole-QRhi allocator, including Qt resources and deferred deletions, sampled at recorded frames; not "
                "physical VRAM or exact allocator peak. Vulkan/D3D12 allocator counters only; unsupported OpenGL/other "
                "counters remain null. Pipeline creation is a cumulative QRhi diagnostic, not isolated preview/GPU "
                "draw time.");
            qint64 cpuBytes = 0;
            for (const auto& frame : m_frame->planes)
                cpuBytes += frame.size();
            record.insert("cpu_decoded_allocation_bytes", cpuBytes);
            if (m_runs[size_t(m_runIndex)].measured) {
                auto& aggregate = m_aggregates[m_runs[size_t(m_runIndex)].name];
                for (const auto& raw : m_records) {
                    // Cross-run retention contains timing scalars only, never raw metadata maps.
                    if (raw.value("kind").toString() == QStringLiteral("decode")
                        || raw.value("kind").toString() == QStringLiteral("frame"))
                        aggregate.push_back(timingColumns(raw));
                }
                QVariantMap scalars;
                for (auto it = record.cbegin(); it != record.cend(); ++it) {
                    if (it.key().endsWith(QStringLiteral("_bytes")) || it.key().endsWith(QStringLiteral("_ms")))
                        scalars.insert(it.key(), it.value());
                }
                m_memorySummaries[m_runs[size_t(m_runIndex)].name].push_back(std::move(scalars));
            }
            m_records.push_back(std::move(record));
            flushRecords();
            m_records.clear();
            if (m_logFailed)
                fail(QStringLiteral("Writing the persistent benchmark log failed"));
        }
        void finish()
        {
            // All measured phases and their final release/drain have completed.
            m_memory.stop();
            for (auto it = m_aggregates.cbegin(); it != m_aggregates.cend(); ++it) {
                QVariantMap memorySummary;
                const QStringList keys { "rss_peak_delta_run_bytes", "rss_peak_delta_global_bytes",
                    "rss_after_delta_run_bytes", "rss_after_delta_global_bytes", "anonymous_peak_delta_run_bytes",
                    "anonymous_peak_delta_global_bytes", "anonymous_after_delta_run_bytes",
                    "anonymous_after_delta_global_bytes", "cpu_decoded_allocation_bytes", "requested_gpu_texture_bytes",
                    "rhi_allocator_peak_used_bytes", "rhi_allocator_peak_allocated_bytes",
                    "rhi_allocator_peak_used_delta_global_bytes", "rhi_allocator_peak_used_delta_run_bytes",
                    "rhi_allocator_peak_allocated_delta_global_bytes", "rhi_d3d12_peak_total_usage_bytes",
                    "rhi_pipeline_creation_run_delta_ms" };
                for (const auto& key : keys) {
                    std::vector<double> values;
                    for (const auto& run : m_memorySummaries.value(it.key())) {
                        if (run.value(key).isValid() && !run.value(key).isNull())
                            values.push_back(run.value(key).toDouble());
                    }
                    memorySummary.insert(key, summary(std::move(values)));
                }
                write({ { "kind", "aggregate" }, { "case", it.key() }, { "measured_runs", 3 },
                    { "decode", summarizeRecords(it.value(), QStringLiteral("decode"), {}) },
                    { "cold_frames", summarizeRecords(it.value(), QStringLiteral("frame"), QStringLiteral("cold")) },
                    { "warm_frames", summarizeRecords(it.value(), QStringLiteral("frame"), QStringLiteral("warm")) },
                    { "memory_run_distribution", memorySummary },
                    { "quantile_definition", "median midpoint for even N; p95 nearest-rank ceil(0.95*N)" } });
            }
            for (auto it = m_aggregates.cbegin(); it != m_aggregates.cend(); ++it) {
                const auto gpu = m_gpuAggregates.constFind(it.key());
                for (const QString& phase : { QStringLiteral("cold"), QStringLiteral("warm") }) {
                    std::vector<double> values;
                    quint64 events = 0;
                    if (gpu != m_gpuAggregates.cend()) {
                        for (const auto& raw : gpu.value()) {
                            if (raw.value("phase").toString() != phase)
                                continue;
                            ++events;
                            if (raw.value("gpu_frame_ms").isValid() && !raw.value("gpu_frame_ms").isNull())
                                values.push_back(raw.value("gpu_frame_ms").toDouble());
                        }
                    }
                    write({ { "kind", "gpu_aggregate" }, { "case", it.key() }, { "phase", phase },
                        { "measured_runs", 3 }, { "event_count", events },
                        { "requested_timed_frames", 3 * (phase == QStringLiteral("cold") ? ColdCount : WarmCount) },
                        { "origin_attribution_supported", m_sample.value("gpu_origin_attribution_supported") },
                        { "gpu_frame_ms", summary(std::move(values)) },
                        { "scope",
                            "Whole Qt GPU frame attributed to exact requested serial; unsupported backends have zero "
                            "samples and null statistics, never fake isolated upload/draw timings." } });
                }
            }
            write({ { "kind", "performance_complete" }, { "warmup_runs", 9 }, { "measured_runs", 9 },
                { "seed", m_seed }, { "memory_after_timed_runs_and_drain", memoryMap(readMemory()) } });
            const QString requested = option(m_app.arguments(), QStringLiteral("--bench-screenshot-dir"));
            m_screenshotDir = requested.isEmpty() ? QFileInfo(m_logPath).absolutePath()
                    + QStringLiteral("/trickplay-verification-") + QString::number(m_seed)
                                                  : requested;
            if (!QDir().mkpath(m_screenshotDir)) {
                fail(QStringLiteral("Could not create verification screenshot directory: ") + m_screenshotDir);
                return;
            }
            requestVerification();
        }
        void requestVerification()
        {
            if (m_finished)
                return;
            const QString name = m_verificationCases.at(m_verifyIndex);
            setCaption(QStringLiteral(
                "Timing complete — visual verification ONLY\n%1 · identical fixed tile for all cases\nGPU readback and "
                "PNG saving are excluded from every benchmark metric.\nSeed %2\nLog: %3")
                    .arg(name)
                    .arg(m_seed)
                    .arg(m_logPath));
            const QByteArray bytes = m_jpeg;
            const QSize size = m_sheet;
            const QRect crop = m_trace.front();
            m_lastProgress = nowNs();
            QMetaObject::invokeMethod(
                m_worker,
                [this, name, bytes, size, crop] {
                    DecodeResult result = name == QStringLiteral("qt-rgb32")
                        ? decodeQt(bytes, size, crop)
                        : decodeJpeg(bytes, size, name == QStringLiteral("jpeg-yuv420"), m_cancel);
                    QMetaObject::invokeMethod(
                        this,
                        [this, result = std::move(result)]() mutable {
                            if (m_finished)
                                return;
                            if (!result.error.isEmpty() || !result.frame) {
                                fail(QStringLiteral("Verification decode failed: ") + result.error);
                                return;
                            }
                            m_frame = std::move(result.frame);
                            m_action = Action::VerifyFrame;
                            submitFrame(m_trace.front());
                        },
                        Qt::QueuedConnection);
                },
                Qt::QueuedConnection);
        }
        void saveVerification()
        {
            const QString name = m_verificationCases.at(m_verifyIndex);
            const bool warmProof = m_action == Action::VerifyWarmFrame;
            const QRect crop = warmProof ? m_trace[1] : m_trace.front();
            if (warmProof
                && (!m_sample.value("uploaded").isValid() || m_sample.value("uploaded").isNull()
                    || m_sample.value("uploaded").toBool())) {
                fail(QStringLiteral("Same-pointer visual crop verification did not explicitly confirm uploaded=false"));
                return;
            }
            const QImage screen = m_window.grabWindow();
            if (screen.isNull()) {
                fail(QStringLiteral("Verification QQuickWindow::grabWindow returned no onscreen image"));
                return;
            }
            const qreal scaleX = qreal(screen.width()) / m_window.width();
            const qreal scaleY = qreal(screen.height()) / m_window.height();
            const QRect previewBounds(qRound(m_item.x() * scaleX), qRound(m_item.y() * scaleY),
                qRound(m_item.width() * scaleX), qRound(m_item.height() * scaleY));
            const QImage preview = screen.copy(previewBounds).convertToFormat(QImage::Format_RGB32);
            const QString fullPath
                = QDir(m_screenshotDir)
                      .filePath(
                          name + (warmProof ? QStringLiteral("-warm-window.png") : QStringLiteral("-window.png")));
            const QString previewPath
                = QDir(m_screenshotDir)
                      .filePath(
                          name + (warmProof ? QStringLiteral("-warm-preview.png") : QStringLiteral("-preview.png")));
            if (preview.isNull() || !screen.save(fullPath, "PNG") || !preview.save(previewPath, "PNG")) {
                fail(QStringLiteral("Could not save real onscreen verification screenshots for ") + name);
                return;
            }
            QVariantMap comparison;
            if (warmProof) {
                if (m_expectedWarmPreview.isNull()) {
                    fail(QStringLiteral("Independent CPU reference for the second verification crop is missing"));
                    return;
                }
                const QImage expected
                    = m_expectedWarmPreview.scaled(preview.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
                quint64 errorSum = 0;
                quint64 changedPixels = 0;
                int maximum = 0;
                for (int y = 0; y < preview.height(); ++y) {
                    const auto *actual = reinterpret_cast<const QRgb *>(preview.constScanLine(y));
                    const auto *reference = reinterpret_cast<const QRgb *>(expected.constScanLine(y));
                    for (int x = 0; x < preview.width(); ++x) {
                        const int r = std::abs(qRed(actual[x]) - qRed(reference[x]));
                        const int g = std::abs(qGreen(actual[x]) - qGreen(reference[x]));
                        const int b = std::abs(qBlue(actual[x]) - qBlue(reference[x]));
                        errorSum += r + g + b;
                        maximum = std::max({ maximum, r, g, b });
                        changedPixels += (r || g || b) ? 1 : 0;
                    }
                }
                const double mae = double(errorSum) / (qint64(preview.width()) * preview.height() * 3);
                const double threshold = m_verifyIndex == 0 ? 3.0 : 5.0;
                comparison = { { "reference", "independent CPU Qt-decoded SECOND fixture crop" },
                    { "rgb_mean_absolute_error_0_255", mae }, { "rgb_max_absolute_error_0_255", maximum },
                    { "changed_pixels", changedPixels }, { "pixel_count", qint64(preview.width()) * preview.height() },
                    { "mae_failure_threshold_0_255", threshold } };
                if (mae > threshold) {
                    write({ { "kind", "visual_verification_warm_failed" }, { "case", name },
                        { "comparison", comparison }, { "preview_png", previewPath } });
                    fail(QStringLiteral("Same-pointer warm crop is stale or does not match the independent second "
                                        "fixture crop; timings are invalid"));
                    return;
                }
            } else if (m_verifyIndex == 0) {
                // Keep only this small CPU crop after releasing the Qt atlas.
                // It is never derived from the first GPU screenshot.
                m_expectedWarmPreview = m_frame->rgbImage.copy(m_trace[1]);
                const QImage expected = m_frame->rgbImage.copy(m_trace.front())
                                            .scaled(preview.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
                quint64 errorSum = 0;
                int maximum = 0;
                for (int y = 0; y < preview.height(); ++y) {
                    const auto *actualRow = reinterpret_cast<const QRgb *>(preview.constScanLine(y));
                    const auto *expectedRow = reinterpret_cast<const QRgb *>(expected.constScanLine(y));
                    for (int x = 0; x < preview.width(); ++x) {
                        for (int shift : { 0, 8, 16 }) {
                            const int difference
                                = std::abs(int((actualRow[x] >> shift) & 255) - int((expectedRow[x] >> shift) & 255));
                            errorSum += difference;
                            maximum = std::max(maximum, difference);
                        }
                    }
                }
                const double mae = double(errorSum) / (qint64(preview.width()) * preview.height() * 3);
                comparison = { { "reference", "independent CPU-decoded fixture crop" },
                    { "rgb_mean_absolute_error_0_255", mae }, { "rgb_max_absolute_error_0_255", maximum } };
                if (mae > 3.0) {
                    write({ { "kind", "visual_verification_failed" }, { "case", name }, { "comparison", comparison },
                        { "preview_png", previewPath } });
                    fail(QStringLiteral("Rendered RGB preview does not match the fixture crop; timings are invalid"));
                    return;
                }
                m_referencePreview = preview;
            } else if (preview.size() == m_referencePreview.size()) {
                quint64 absoluteSum = 0;
                quint64 changedPixels = 0;
                int maximum = 0;
                for (int y = 0; y < preview.height(); ++y) {
                    const auto *actual = reinterpret_cast<const QRgb *>(preview.constScanLine(y));
                    const auto *reference = reinterpret_cast<const QRgb *>(m_referencePreview.constScanLine(y));
                    for (int x = 0; x < preview.width(); ++x) {
                        const int r = std::abs(qRed(actual[x]) - qRed(reference[x]));
                        const int g = std::abs(qGreen(actual[x]) - qGreen(reference[x]));
                        const int b = std::abs(qBlue(actual[x]) - qBlue(reference[x]));
                        absoluteSum += r + g + b;
                        maximum = std::max({ maximum, r, g, b });
                        changedPixels += (r || g || b) ? 1 : 0;
                    }
                }
                comparison = { { "reference", "qt-rgb32" },
                    { "rgb_mean_absolute_error_0_255",
                        double(absoluteSum) / (qint64(preview.width()) * preview.height() * 3) },
                    { "rgb_max_absolute_error_0_255", maximum }, { "changed_pixels", changedPixels },
                    { "pixel_count", qint64(preview.width()) * preview.height() },
                    { "interpretation",
                        "Diagnostic, not an equivalence assertion: Qt/native RGB and raw YUV differ in chroma "
                        "reconstruction, hardware bilinear filtering, shader rounding and display color "
                        "conversion." } };
                if (comparison.value("rgb_mean_absolute_error_0_255").toDouble() > 5.0) {
                    write({ { "kind", "visual_verification_failed" }, { "case", name }, { "comparison", comparison },
                        { "preview_png", previewPath } });
                    fail(
                        QStringLiteral("Rendered comparison preview differs materially from RGB; timings are invalid"));
                    return;
                }
            } else {
                comparison = { { "error", "Preview dimensions differ; comparison unavailable" } };
            }
            write({ { "kind", warmProof ? "visual_verification_warm" : "visual_verification" }, { "case", name },
                { "excluded_from_all_performance_metrics", true }, { "window_png", fullPath },
                { "preview_png", previewPath }, { "preview_width", preview.width() },
                { "preview_height", preview.height() }, { "tile_x", crop.x() }, { "tile_y", crop.y() },
                { "same_frame_pointer", warmProof }, { "uploaded", m_sample.value("uploaded") },
                { "comparison", comparison } });
            if (m_logFailed) {
                fail(QStringLiteral("Verification log write failed"));
                return;
            }
            if (!warmProof) {
                m_action = Action::VerifyWarmFrame;
                submitFrame(m_trace[1]); // Exactly the same owned frame pointer; only the crop changes.
                return;
            }
            if (++m_verifyIndex < m_verificationCases.size()) {
                m_action = Action::VerifyDrainOne;
                submitBlank();
                return;
            }
            m_referencePreview = {};
            m_expectedWarmPreview = {};
            m_finished = true;
            m_watchdog.stop();
            m_memory.stop();
            write({ { "kind", "complete" }, { "warmup_runs", 9 }, { "measured_runs", 9 }, { "verification_images", 6 },
                { "same_pointer_second_crop_verified_against_cpu_fixture", true },
                { "visual_verified_against_cpu_fixture", true }, { "seed", m_seed },
                { "memory_final_after_untimed_verification", memoryMap(readMemory()) } });
            setCaption(QStringLiteral("Trickplay benchmark complete\n9 full warm-up runs + 9 measured runs\nEach run: "
                                      "12 cold decodes/uploads + 120 crop-only frames\nSeed %1\nRaw log: %2\nSame-tile "
                                      "screenshots: %3\nFinal preview: direct planar YUV (verification only)")
                    .arg(m_seed)
                    .arg(m_logPath)
                    .arg(m_screenshotDir));
            if (m_logFailed)
                m_app.exit(2);
            else if (m_autoExit)
                QTimer::singleShot(0, &m_app, [this] { m_app.exit(0); });
        }
        QGuiApplication& m_app;
        NativeAppWindow& m_window;
        TrickplayBenchmarkItem& m_item;
        QQuickItem& m_caption;
        QThread m_decodeThread;
        QObject *m_worker;
        MemorySampler m_memory;
        QTimer m_watchdog;
        std::atomic<bool> m_cancel { false };
        std::atomic<quint64> m_renderSerial { 0 };
        std::atomic<quint64> m_submittedSerial { 0 };
        QFile m_log;
        QString m_logPath;
        QString m_screenshotDir;
        const QStringList m_verificationCases { QStringLiteral("qt-rgb32"), QStringLiteral("jpeg-rgb32"),
            QStringLiteral("jpeg-yuv420") };
        int m_verifyIndex = 0;
        QImage m_referencePreview;
        QImage m_expectedWarmPreview;
        bool m_logFailed = false;
        bool m_autoExit = false;
        bool m_freshTextures = false;
        bool m_gpuOriginAttributionSupported = false;
        bool m_finished = false;
        bool m_firstSwap = false;
        quint32 m_seed = 0;
        QByteArray m_jpeg;
        QVariantMap m_manifest;
        QSize m_sheet;
        QSize m_thumbnail;
        int m_columns = 0;
        int m_rows = 0;
        Memory m_globalBaseline;
        Memory m_runBaseline;
        QVariantMap m_rhiGlobalBaseline;
        QVariantMap m_rhiRunBaseline;
        std::vector<Run> m_runs;
        std::vector<QRect> m_trace;
        int m_runIndex = -1;
        int m_cold = 0;
        int m_warm = 0;
        std::shared_ptr<const TrickplayBenchFrame> m_frame;
        std::vector<QVariantMap> m_records;
        QMap<QString, std::vector<QVariantMap>> m_aggregates;
        QMap<QString, std::vector<QVariantMap>> m_memorySummaries;
        QMap<quint64, QVariantMap> m_gpuOrigins;
        std::vector<QVariantMap> m_gpuRecords;
        QMap<QString, std::vector<QVariantMap>> m_gpuAggregates;
        Action m_action = Action::InitialDrain;
        bool m_pending = false;
        bool m_haveSample = false;
        bool m_haveSwap = false;
        quint64 m_serial = 0;
        qint64 m_submitNs = 0;
        qint64 m_swapNs = 0;
        qint64 m_runStartNs = 0;
        qint64 m_selectionNs = 0;
        qint64 m_workerQueueNs = 0;
        qint64 m_workerHandoffNs = 0;
        qint64 m_lastProgress = nowNs();
        double m_lastLatencyMs = 0;
        QVariantMap m_sample;
    };
} // namespace

int runTrickplayBenchmark(int argc, char **argv)
{
    const QString appRoot = resolveAppRoot(argv[0]);
    if (appRoot.isEmpty() || !configurePlatformEnvironment(appRoot)) {
        QStringList arguments;
        for (int index = 0; index < argc; ++index)
            arguments.append(QString::fromLocal8Bit(argv[index]));
        return startupError(
            arguments, QStringLiteral("Could not resolve application root or configure platform environment"));
    }
    std::setlocale(LC_NUMERIC, "C");
    qputenv("LC_NUMERIC", QByteArrayLiteral("C"));
    QCoreApplication::setOrganizationName(QStringLiteral("spool"));
    QCoreApplication::setApplicationName(QStringLiteral("Spool"));
    const auto graphicsApi = GraphicsStartup::configureBeforeApplication(false);
    QGuiApplication app(argc, argv);
    // Keep the selected hardware API but disable startup HDR/scRGB: this
    // experiment's preview shaders and caption draw straight into one SDR scene.
    const QByteArray hdrRequest = GraphicsStartup::prepareBeforeWindow(graphicsApi, true);
    NativeAppWindow window(QStringLiteral("com.sachk.spool"));
    GraphicsStartup::attachToWindow(window, hdrRequest);
    auto configuration = window.graphicsConfiguration();
    configuration.setTimestamps(true);
    window.setGraphicsConfiguration(configuration);
    configurePlatformWindow(window);
    window.setTitle(QStringLiteral("Spool — trickplay RGB / planar YUV benchmark"));
#if !defined(SPOOL_WEBOS)
    window.resize(1000, 700);
#endif
    window.setColor(QColor(QStringLiteral("#10151d")));
    auto *item = new TrickplayBenchmarkItem(window.contentItem());
    item->setX(32);
    item->setY(32);
    item->setWidth(320);
    item->setHeight(180);
    QQmlComponent component(window.engine());
    component.setData(R"QML(import QtQuick
Text {
    color: "#e5edf7"
    font.pixelSize: 19
    wrapMode: Text.WordWrap
    text: "Preparing benchmark…"
})QML",
        QUrl(QStringLiteral("qrc:/bench/trickplay-benchmark-caption.qml")));
    if (component.isLoading()) {
        QEventLoop imports;
        QTimer deadline;
        deadline.setSingleShot(true);
        QObject::connect(&component, &QQmlComponent::statusChanged, &imports, [&imports](QQmlComponent::Status status) {
            if (status != QQmlComponent::Loading)
                imports.quit();
        });
        QObject::connect(&deadline, &QTimer::timeout, &imports, &QEventLoop::quit);
        deadline.start(15000);
        imports.exec();
    }
    if (!component.isReady())
        return startupError(app.arguments(),
            component.isLoading() ? QStringLiteral("Timed out loading benchmark caption imports")
                                  : QStringLiteral("Could not load benchmark caption: ") + component.errorString());
    std::unique_ptr<QObject> captionOwner(component.create());
    auto *caption = qobject_cast<QQuickItem *>(captionOwner.get());
    if (!caption)
        return startupError(
            app.arguments(), QStringLiteral("Could not create benchmark caption: ") + component.errorString());
    caption->setParentItem(window.contentItem());
    caption->setX(32);
    caption->setY(236);
    caption->setWidth(std::max(400, window.width() - 64));
    caption->setHeight(std::max(qreal(250), window.height() - caption->y() - 24));
    QObject::connect(&window, &QQuickWindow::widthChanged, caption,
        [&window, caption] { caption->setWidth(std::max(400, window.width() - 64)); });
    QObject::connect(&window, &QQuickWindow::heightChanged, caption,
        [&window, caption] { caption->setHeight(std::max(qreal(250), window.height() - caption->y() - 24)); });
    Benchmark benchmark(app, window, *item, *caption);
    if (!benchmark.initialize())
        return 2;
    if (!window.prepareForUiSurface()) {
        benchmark.surfaceFailure(QStringLiteral("Could not prepare native UI surface"));
        return 2;
    }
    window.bringToFront();
    window.update();
    return app.exec();
}
} // namespace Spool
