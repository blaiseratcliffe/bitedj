#include "library/rekordbox/rekordboxtrackhealth.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLatin1String>
#include <QList>
#include <QMutex>
#include <QMutexLocker>
#include <QStringList>
#include <utility>

namespace mixxx {
namespace rekordbox {

namespace {

// Every ANLZ file, .DAT, .EXT and .2EX alike, opens with this tag.
const QByteArray kAnlzMagic = QByteArrayLiteral("PMAI");

// True when `path` exists but is not a readable ANLZ file. A file that does
// not exist is not damaged: older exports simply have fewer siblings.
bool isDamagedAnlzFile(const QString& path) {
    QFile file(path);
    if (!file.exists()) {
        return false;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        return true;
    }
    // The file that aborted the app on 2026-09-27 was an .EXT zeroed after its
    // first byte by an unclean unmount; a short read fails this the same way.
    return file.read(kAnlzMagic.size()) != kAnlzMagic;
}

// The checks now running, so an eject can stop the ones reading its drive.
// There is only ever one in practice, but the registry does not rely on it.
struct RunningCheck {
    // Cleaned, so it compares with a cleaned mount point.
    QString devicePath;
    HealthCheckToken token;
};
QMutex s_runningChecksMutex;
QList<RunningCheck> s_runningChecks;

} // anonymous namespace

TrackHealth checkTrackFiles(const QString& location, const QString& anlzDatPath) {
    TrackHealth health;

    // First, because a track with no audio cannot be loaded at all, which
    // matters more than anything wrong with its analysis.
    const QFileInfo audioInfo(location);
    if (!audioInfo.exists() || audioInfo.size() == 0) {
        health.problem = TrackProblem::AudioMissing;
        health.badPath = location;
        return health;
    }

    // A track exported without analysis has an empty analyze_path, which
    // insertTrack() turns into the device's own directory. That exists and
    // cannot be read as a file, so looking at it would call a track with no
    // analysis damaged. Anything that is not a .DAT has nothing to check.
    if (!anlzDatPath.endsWith(QLatin1String("DAT"), Qt::CaseInsensitive)) {
        return health;
    }

    // The same sibling naming readAnalyzeFiles() and getTrack() use to find
    // the .EXT and .2EX. The .DAT and .EXT come first because they hold the
    // cues and the grid, which matter more than the waveform in the .2EX.
    const QString stem = anlzDatPath.left(anlzDatPath.length() - 3);
    const QStringList cueAndGridPaths = {anlzDatPath, stem + QLatin1String("EXT")};
    for (const QString& anlzPath : cueAndGridPaths) {
        if (isDamagedAnlzFile(anlzPath)) {
            health.problem = TrackProblem::AnalysisDamaged;
            health.badPath = anlzPath;
            return health;
        }
    }

    const QString waveformPath = stem + QLatin1String("2EX");
    if (isDamagedAnlzFile(waveformPath)) {
        health.problem = TrackProblem::WaveformDamaged;
        health.badPath = waveformPath;
    }

    return health;
}

HealthCheckToken beginHealthCheck(const QString& devicePath) {
    HealthCheckToken token = std::make_shared<std::atomic<bool>>(false);
    const QMutexLocker locked(&s_runningChecksMutex);
    s_runningChecks.append(RunningCheck{QDir::cleanPath(devicePath), token});
    return token;
}

void endHealthCheck(const HealthCheckToken& token) {
    const QMutexLocker locked(&s_runningChecksMutex);
    for (auto it = s_runningChecks.begin(); it != s_runningChecks.end();) {
        if (it->token == token) {
            it = s_runningChecks.erase(it);
        } else {
            ++it;
        }
    }
}

void cancelHealthChecksUnderPath(const QString& mountPoint) {
    const QString wanted = QDir::cleanPath(mountPoint);
    if (wanted.isEmpty()) {
        // Would otherwise match every path through the "/" prefix below.
        return;
    }
    // With the separator, so /media/blaise/Lexar does not also stop a check
    // of /media/blaise/Lexar2.
    QString prefix = wanted;
    if (!prefix.endsWith(QLatin1Char('/'))) {
        prefix.append(QLatin1Char('/'));
    }
    const QMutexLocker locked(&s_runningChecksMutex);
    for (const RunningCheck& check : std::as_const(s_runningChecks)) {
        if (check.devicePath == wanted || check.devicePath.startsWith(prefix)) {
            check.token->store(true);
        }
    }
}

} // namespace rekordbox
} // namespace mixxx
