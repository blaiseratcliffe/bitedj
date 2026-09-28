#pragma once

#include <QString>
#include <atomic>
#include <memory>

namespace mixxx {
namespace rekordbox {

/// What is wrong with a track on a rekordbox stick, as far as a cheap look at
/// its files can tell. Stored as-is in rekordbox_library.problem, so the
/// values are fixed.
enum class TrackProblem : int {
    None = 0,
    // The audio file is gone or empty. The track cannot be loaded at all.
    AudioMissing = 1,
    // The .DAT or .EXT exists but does not start like an ANLZ file. The track
    // loads, but without the cues or beat grid that file held.
    AnalysisDamaged = 2,
    // Only the .2EX is damaged. It holds nothing but the three band waveform,
    // so cues and grid are intact and the app draws its own waveform instead.
    // Logged, but neither coloured in the list nor named in the notice: the
    // track plays exactly as it should.
    WaveformDamaged = 3,
};

struct TrackHealth {
    TrackProblem problem = TrackProblem::None;
    // The file that gave the verdict; empty when there is no problem.
    QString badPath;
};

/// Checks one track's files: that `location` (the audio file) exists and is
/// not empty, and that each of the .DAT, .EXT and .2EX ANLZ files next to
/// `anlzDatPath` (the PDB's analyze_path) starts with the "PMAI" magic. A
/// sibling that does not exist is fine, since older exports have no .EXT or
/// .2EX. A missing audio file wins over damaged analysis, and a damaged .DAT
/// or .EXT wins over a damaged .2EX.
///
/// When `anlzDatPath` does not end in .DAT the track has no analysis to
/// check: a PDB with an empty analyze_path leaves the device's directory
/// here, which is not a damaged file. Only the audio is judged then.
///
/// Only the header is read. Damage further into an ANLZ file is not caught
/// here; readAnalyze() skips such a file when the track is loaded. Never
/// throws, and touches no database, so it can run on any thread.
TrackHealth checkTrackFiles(const QString& location, const QString& anlzDatPath);

/// A running check's stop flag, shared between the worker and the registry
/// below. The worker polls it once per track.
using HealthCheckToken = std::shared_ptr<std::atomic<bool>>;

/// Registers a check over the files under `devicePath` and returns its stop
/// flag, cleared. Pair with endHealthCheck() once the worker has finished.
HealthCheckToken beginHealthCheck(const QString& devicePath);

/// Forgets a check registered by beginHealthCheck(). A later cancel no longer
/// reaches its token.
void endHealthCheck(const HealthCheckToken& token);

/// Stops every running check whose device path is `mountPoint` or lies under
/// it. Called by the eject before it unmounts: a check keeps opening files on
/// the stick, which would hold the filesystem busy. Thread-safe.
void cancelHealthChecksUnderPath(const QString& mountPoint);

} // namespace rekordbox
} // namespace mixxx
