#pragma once

#include <QSharedPointer>
#include <QString>
#include <QVector>
#include <cstdint>

#include "track/track_decl.h"
#include "util/class.h"
#include "waveform/waveform.h"

namespace mixxx {

/// One column of three band energy, as the CDJ-3000 stores it.
///
/// The byte order inside a `.2EX` entry is byte0 = low, byte1 = mid,
/// byte2 = high. That was **measured** against real analysis files, not taken
/// from documentation: a 435 Hz pure sine gives 32/31/0, band limited noise
/// around 1 kHz gives 7/38/5, and bass heavy music gives 56/27/7. Some
/// third party write-ups claim the order is mid, high, low; they are wrong.
struct BandSample {
    uint8_t low;
    uint8_t mid;
    uint8_t high;
};

/// The three band waveform data for one track. Immutable once constructed.
///
/// Two independent series live here, matching the two rekordbox tags:
/// `detail()` is PWV7, the scrolling waveform, and `preview()` is PWV6, the
/// whole-track overview. A file may carry either, both, or neither.
class Rekordbox3BandWaveform {
  public:
    /// Where the samples came from. `NativePwv7` means the data is rekordbox's
    /// own analysis, whichever of PWV7 and PWV6 the file happened to hold;
    /// the distinction the enum draws is native versus derived, not which tag.
    enum class Source {
        NativePwv7,
        MixxxFallback,
        None,
    };

    /// Parse the PWV6/PWV7 tags out of a rekordbox `ANLZ0000.2EX` file.
    /// Returns a null pointer on any failure, including a missing, truncated
    /// or foreign file, or one that holds neither tag.
    static QSharedPointer<const Rekordbox3BandWaveform> fromAnlz2Ex(const QString& path);

    /// Build the fallback approximation from Mixxx's own analysis, for tracks
    /// that have no rekordbox `.2EX` file. Detail only: there is no preview
    /// series, and no meaningful entries-per-second either, because the
    /// waveform does not expose its visual sample rate.
    static QSharedPointer<const Rekordbox3BandWaveform> fromMixxxWaveform(
            const ConstWaveformPointer& pWaveform);

    /// The same fallback, but keeping a native PWV6 preview that the `.2EX`
    /// did carry. A file with PWV6 and no PWV7 would otherwise leave the
    /// scrolling waveform empty while the overview drew fine, which skips the
    /// middle rung of the intended ladder: native detail, then a Mixxx
    /// approximation, then nothing. No real file seen so far carries one tag
    /// without the other, so this path is defensive.
    static QSharedPointer<const Rekordbox3BandWaveform> fromMixxxWaveformKeepingPreview(
            const ConstWaveformPointer& pWaveform,
            const QVector<BandSample>& preview);

    Source source() const {
        return m_source;
    }

    bool hasDetail() const {
        return !m_detail.isEmpty();
    }

    bool hasPreview() const {
        return !m_preview.isEmpty();
    }

    /// PWV7, the scrolling waveform.
    const QVector<BandSample>& detail() const {
        return m_detail;
    }

    /// PWV6, the whole-track preview.
    const QVector<BandSample>& preview() const {
        return m_preview;
    }

    /// 150 for native data. 0.0 when unknown, which is the case for the
    /// Mixxx fallback; map by normalized position instead.
    double detailEntriesPerSecond() const {
        return m_detailEntriesPerSecond;
    }

    /// Map a normalized [0,1] track position onto an entry index. Both clamp,
    /// and both return -1 when the series is empty. The mapping is
    /// `normalized * (count - 1)` rounded, so 0.0 lands on the first entry and
    /// 1.0 on the last one exactly.
    int detailIndexForPosition(double normalized) const;
    int previewIndexForPosition(double normalized) const;

  private:
    Rekordbox3BandWaveform(
            Source source,
            QVector<BandSample> detail,
            QVector<BandSample> preview,
            double detailEntriesPerSecond);

    const Source m_source;
    const QVector<BandSample> m_detail;
    const QVector<BandSample> m_preview;
    const double m_detailEntriesPerSecond;

    DISALLOW_COPY_AND_ASSIGN(Rekordbox3BandWaveform);
};

typedef QSharedPointer<const Rekordbox3BandWaveform> ConstRekordbox3BandWaveformPointer;

/// Returns the three band data to draw for this track: native if the importer
/// attached some, otherwise a Mixxx-derived approximation built once and cached
/// on the track, otherwise null. Logs the source once per track, not once per
/// frame, which is why the fallback is written back through the track's setter.
ConstRekordbox3BandWaveformPointer resolveRekordbox3BandWaveform(const TrackPointer& pTrack);

} // namespace mixxx
