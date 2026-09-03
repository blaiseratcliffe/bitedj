#pragma once

#include <QString>

#include "track/track_decl.h"

namespace mixxx {
namespace rekordbox {

/// Reads PWV6/PWV7 from a rekordbox .2EX file and attaches the result to the
/// track. Always clears any previously attached data first, so a track that has
/// none never shows another track's waveform. Safe against missing, truncated
/// and foreign files.
///
/// Logs exactly one line per call, which is what hardware verification greps
/// for. The Mixxx fallback is not one of the outcomes here: it belongs to
/// `resolveRekordbox3BandWaveform()`, because it is only built when something
/// actually asks to draw.
void read3BandWaveform(TrackPointer pTrack, const QString& anlz2ExPath);

} // namespace rekordbox
} // namespace mixxx
