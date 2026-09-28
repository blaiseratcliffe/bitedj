#pragma once

#include <QString>

#include "audio/types.h"
#include "track/track_decl.h"

namespace mixxx {
namespace rekordbox {

/// Import beats and/or cues from a rekordbox ANLZ file onto a track.
///
/// `ignoreCues` picks which half of the file is read: beats when true, cues
/// when false. Both halves are never wanted from the same file — the beat grid
/// is only correct in the legacy `.DAT`, while cues are preferred from the
/// `.EXT` when one exists.
///
/// The cue pass treats the ANLZ file as the authority for the whole track:
/// hot cues land in the hot cue bank, memory cues in the memory cue bank (see
/// `kHotCueBankStart` / `kMemoryCueBankStart` in `track/cueinfo.h`), and any
/// hotcue slot the file does not describe is cleared. Cues that survive are
/// updated in place, because this runs on every load of a track that a deck
/// may already be playing.
///
/// Returns false when nothing was imported: the file does not exist, or it
/// could not be parsed (a damaged file is logged and skipped, never thrown
/// out of here). readAnalyzeFiles() uses that to take cues from the `.DAT`
/// when the `.EXT` is damaged.
///
/// Declared here rather than kept file-local so that it can be tested
/// directly; the definition lives in rekordboxfeature.cpp.
bool readAnalyze(TrackPointer track,
        audio::SampleRate sampleRate,
        int timingOffset,
        bool ignoreCues,
        const QString& anlzPath);

/// Imports a track's beats and cues from its ANLZ files, given the `.DAT`
/// path from the device database. Beats always come from the `.DAT`. Cues
/// come from the `.EXT` next to it when there is one, and from the `.DAT`
/// when there is not, or when the `.EXT` cannot be read.
void readAnalyzeFiles(TrackPointer track,
        audio::SampleRate sampleRate,
        int timingOffset,
        const QString& anlzDatPath);

} // namespace rekordbox
} // namespace mixxx
