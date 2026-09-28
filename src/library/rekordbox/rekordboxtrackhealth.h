#pragma once

#include <QString>

namespace mixxx {
namespace rekordbox {

/// What is wrong with a track on a rekordbox stick, as far as a cheap look at
/// its files can tell. Stored as-is in rekordbox_library.problem, so the
/// values are fixed.
enum class TrackProblem : int {
    None = 0,
    // The audio file is gone or empty. The track cannot be loaded at all.
    AudioMissing = 1,
    // An ANLZ file exists but does not start like one. The track loads, but
    // without whatever that file held (cues, beat grid, three band waveform).
    AnalysisDamaged = 2,
};

struct TrackHealth {
    TrackProblem problem = TrackProblem::None;
    // The file that gave the verdict; empty when there is no problem.
    QString badPath;
};

/// Checks one track's files: that `location` (the audio file) exists and is
/// not empty, and that each of the .DAT, .EXT and .2EX ANLZ files next to
/// `anlzDatPath` (the PDB's analyze_path, ending in .DAT) starts with the
/// "PMAI" magic. A sibling that does not exist is fine, since older exports
/// have no .EXT or .2EX. A missing audio file wins over damaged analysis.
///
/// Only the header is read. Damage further into an ANLZ file is not caught
/// here; readAnalyze() skips such a file when the track is loaded. Never
/// throws, and touches no database, so it can run on any thread.
TrackHealth checkTrackFiles(const QString& location, const QString& anlzDatPath);

} // namespace rekordbox
} // namespace mixxx
