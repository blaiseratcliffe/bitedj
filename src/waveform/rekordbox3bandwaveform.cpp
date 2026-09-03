#include "waveform/rekordbox3bandwaveform.h"

#include <rekordbox_anlz.h>

#include <QFile>
#include <QtDebug>
#include <cmath>
#include <exception>
#include <fstream>
#include <string>
#include <utility>

#include "track/track.h"

namespace {

/// PWV7 is sampled at a fixed 150 entries per second. The tag header carries
/// this as its third u4 field, always 0x0096_0000.
constexpr double kPwv7EntriesPerSecond = 150.0;

/// Bound on the allocation a single tag may ask for: three hours at the PWV7
/// rate. Anything larger is a corrupt or foreign file, not a DJ set.
constexpr uint32_t kMaxEntries = 150 * 60 * 60 * 3;

/// Copy one tag's entry blob into band samples, rejecting anything whose
/// header does not agree with the payload. `entries` is a raw byte blob that
/// kaitai happens to hand back as a std::string, not text.
QVector<mixxx::BandSample> readBandSamples(
        uint32_t lenEntryBytes,
        uint32_t lenEntries,
        const std::string& entries,
        const QString& tagName,
        const QString& path) {
    QVector<mixxx::BandSample> samples;

    if (lenEntryBytes != 3) {
        qWarning() << "Rekordbox 3-band:" << tagName << "has entry size"
                   << lenEntryBytes << "instead of 3, ignoring it in" << path;
        return samples;
    }
    if (lenEntries > kMaxEntries) {
        qWarning() << "Rekordbox 3-band:" << tagName << "claims" << lenEntries
                   << "entries, which is implausible, ignoring it in" << path;
        return samples;
    }
    if (static_cast<size_t>(lenEntries) * 3 != entries.size()) {
        qWarning() << "Rekordbox 3-band:" << tagName << "claims" << lenEntries
                   << "entries but carries" << entries.size()
                   << "bytes, ignoring it in" << path;
        return samples;
    }
    if (lenEntries == 0) {
        return samples;
    }

    const auto* pBytes = reinterpret_cast<const uint8_t*>(entries.data());
    samples.reserve(static_cast<int>(lenEntries));
    for (uint32_t i = 0; i < lenEntries; i++) {
        const uint8_t* pEntry = pBytes + (static_cast<size_t>(i) * 3);
        samples.append(mixxx::BandSample{pEntry[0], pEntry[1], pEntry[2]});
    }
    return samples;
}

/// Shared by both index accessors. Rounds rather than truncates so that 1.0
/// lands on the last entry instead of one past it.
int indexForPosition(const QVector<mixxx::BandSample>& samples, double normalized) {
    if (samples.isEmpty()) {
        return -1;
    }
    if (!(normalized > 0.0)) {
        // Also catches NaN.
        return 0;
    }
    if (normalized >= 1.0) {
        return samples.size() - 1;
    }
    return static_cast<int>(std::lround(normalized * (samples.size() - 1)));
}

} // anonymous namespace

namespace mixxx {

Rekordbox3BandWaveform::Rekordbox3BandWaveform(
        Source source,
        QVector<BandSample> detail,
        QVector<BandSample> preview,
        double detailEntriesPerSecond)
        : m_source(source),
          m_detail(std::move(detail)),
          m_preview(std::move(preview)),
          m_detailEntriesPerSecond(detailEntriesPerSecond) {
}

// static
QSharedPointer<const Rekordbox3BandWaveform> Rekordbox3BandWaveform::fromAnlz2Ex(
        const QString& path) {
    if (path.isEmpty() || !QFile(path).exists()) {
        return {};
    }

    QVector<BandSample> detail;
    QVector<BandSample> preview;

    // A truncated or foreign file must not take the load down with it: kaitai
    // throws kaitai::kstruct_error, and the ifstream can throw as well.
    try {
        std::ifstream ifs(path.toStdString(), std::ifstream::binary);
        kaitai::kstream ks(&ifs);

        rekordbox_anlz_t anlz = rekordbox_anlz_t(&ks);

        for (const auto& section : *anlz.sections()) {
            switch (section->fourcc()) {
            case rekordbox_anlz_t::SECTION_TAGS_WAVE_3BAND_SCROLL: {
                auto* pTag =
                        static_cast<rekordbox_anlz_t::wave_3band_scroll_tag_t*>(
                                section->body());
                if (!pTag) {
                    break;
                }
                detail = readBandSamples(pTag->len_entry_bytes(),
                        pTag->len_entries(),
                        pTag->entries(),
                        QStringLiteral("PWV7"),
                        path);
            } break;
            case rekordbox_anlz_t::SECTION_TAGS_WAVE_3BAND_PREVIEW: {
                auto* pTag =
                        static_cast<rekordbox_anlz_t::wave_3band_preview_tag_t*>(
                                section->body());
                if (!pTag) {
                    break;
                }
                preview = readBandSamples(pTag->len_entry_bytes(),
                        pTag->len_entries(),
                        pTag->entries(),
                        QStringLiteral("PWV6"),
                        path);
            } break;
            default:
                break;
            }
        }
    } catch (const std::exception& e) {
        qWarning() << "Rekordbox 3-band: cannot read" << path << ":" << e.what();
        return {};
    }

    if (detail.isEmpty() && preview.isEmpty()) {
        return {};
    }

    // A PWV6-only file has no detail series, so it has no rate either.
    const double entriesPerSecond = detail.isEmpty() ? 0.0 : kPwv7EntriesPerSecond;

    return QSharedPointer<const Rekordbox3BandWaveform>(
            new Rekordbox3BandWaveform(Source::NativePwv7,
                    std::move(detail),
                    std::move(preview),
                    entriesPerSecond));
}

// static
QSharedPointer<const Rekordbox3BandWaveform> Rekordbox3BandWaveform::fromMixxxWaveform(
        const ConstWaveformPointer& pWaveform) {
    if (!pWaveform) {
        return {};
    }
    const int dataSize = pWaveform->getDataSize();
    if (dataSize <= 0) {
        return {};
    }

    QVector<BandSample> detail;
    detail.reserve(dataSize);
    for (int i = 0; i < dataSize; i++) {
        // Mixxx stores 0..255 per band, the native data 0..127. Halving keeps
        // downstream code with a single range to reason about, and 255 maps
        // onto 127 exactly.
        detail.append(BandSample{
                static_cast<uint8_t>(pWaveform->getLow(i) / 2),
                static_cast<uint8_t>(pWaveform->getMid(i) / 2),
                static_cast<uint8_t>(pWaveform->getHigh(i) / 2)});
    }

    // The Waveform keeps its visual sample rate private, so the rate is not
    // recoverable here. Callers fall back to normalized position mapping.
    return QSharedPointer<const Rekordbox3BandWaveform>(
            new Rekordbox3BandWaveform(Source::MixxxFallback,
                    std::move(detail),
                    QVector<BandSample>(),
                    0.0));
}

// static
QSharedPointer<const Rekordbox3BandWaveform>
Rekordbox3BandWaveform::fromMixxxWaveformKeepingPreview(
        const ConstWaveformPointer& pWaveform, const QVector<BandSample>& preview) {
    const QSharedPointer<const Rekordbox3BandWaveform> pDetail =
            fromMixxxWaveform(pWaveform);
    if (!pDetail) {
        return {};
    }
    // Source stays MixxxFallback: what the scrolling waveform draws is the
    // approximation, and that is what the log line and the docs are about.
    return QSharedPointer<const Rekordbox3BandWaveform>(
            new Rekordbox3BandWaveform(Source::MixxxFallback,
                    pDetail->detail(),
                    preview,
                    0.0));
}

int Rekordbox3BandWaveform::detailIndexForPosition(double normalized) const {
    return indexForPosition(m_detail, normalized);
}

int Rekordbox3BandWaveform::previewIndexForPosition(double normalized) const {
    return indexForPosition(m_preview, normalized);
}

ConstRekordbox3BandWaveformPointer resolveRekordbox3BandWaveform(const TrackPointer& pTrack) {
    if (!pTrack) {
        return {};
    }

    const ConstRekordbox3BandWaveformPointer& pNative =
            pTrack->getRekordbox3BandWaveform();
    if (pNative && pNative->hasDetail()) {
        // Either the importer's native data, or a fallback this function
        // already built and cached, which is what keeps the log line below
        // from repeating on every frame.
        return pNative;
    }

    // A `.2EX` with PWV6 but no PWV7 has an overview and nothing to scroll.
    // Approximate the detail rather than drawing an empty pane, and keep the
    // native preview so the overview still shows rekordbox's own numbers.
    const QVector<BandSample> nativePreview =
            pNative ? pNative->preview() : QVector<BandSample>();

    ConstRekordbox3BandWaveformPointer pFallback =
            Rekordbox3BandWaveform::fromMixxxWaveformKeepingPreview(
                    pTrack->getWaveform(), nativePreview);
    if (!pFallback) {
        // Mixxx's own analysis is not ready yet; try again on a later call.
        // A PWV6-only native object is still worth keeping for the overview.
        return pNative;
    }

    if (nativePreview.isEmpty()) {
        qWarning() << "3Band source: Mixxx fallback" << pTrack->getLocation();
    } else {
        qWarning() << "3Band source: Mixxx fallback, native PWV6 overview"
                   << pTrack->getLocation();
    }
    pTrack->setRekordbox3BandWaveform(pFallback);
    return pFallback;
}

} // namespace mixxx
