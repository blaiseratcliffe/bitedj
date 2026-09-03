#include "library/rekordbox/rekordbox3bandimport.h"

#include <QFile>
#include <QtDebug>

#include "track/track.h"
#include "waveform/rekordbox3bandwaveform.h"

namespace mixxx {
namespace rekordbox {

// The source lines below are qWarning() rather than qInfo() on purpose.
// kLogLevelDefault is LogLevel::Warning (util/logging.h), and the appliance
// launches without --logLevel, so anything at info or below never reaches
// ~/bitedj.log. Telling native data from the fallback is the whole point of
// these lines, so they have to be at a level the appliance actually records.
// check-log.sh filters them, since they are normal operation and not a fault.
void read3BandWaveform(TrackPointer pTrack, const QString& anlz2ExPath) {
    if (!pTrack) {
        return;
    }

    // Unconditionally first: getTrack() hands back a cached Track, and a track
    // without its own .2EX must not keep the previous one's waveform.
    pTrack->setRekordbox3BandWaveform(nullptr);

    if (anlz2ExPath.isEmpty() || !QFile(anlz2ExPath).exists()) {
        // Worth a line even though a missing sibling is the normal case for
        // anything a CDJ-3000 never analyzed: without it, a track that has no
        // .2EX and a feature that never ran look identical in the log.
        qWarning() << "3Band source: no .2EX sibling" << anlz2ExPath;
        return;
    }

    // Parsing, validation and the kaitai exception guard all live in
    // Rekordbox3BandWaveform::fromAnlz2Ex(); a null result means the file was
    // missing, truncated, foreign, or held neither tag.
    const ConstRekordbox3BandWaveformPointer p3Band =
            Rekordbox3BandWaveform::fromAnlz2Ex(anlz2ExPath);

    if (!p3Band) {
        qWarning() << "3Band source: none" << anlz2ExPath;
        return;
    }

    pTrack->setRekordbox3BandWaveform(p3Band);

    if (p3Band->hasDetail()) {
        qWarning() << "3Band source: native PWV7" << anlz2ExPath;
    } else {
        qWarning() << "3Band source: native PWV6 only" << anlz2ExPath;
    }
}

} // namespace rekordbox
} // namespace mixxx
