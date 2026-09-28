#include "library/rekordbox/rekordboxtrackhealth.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QFile>
#include <QTemporaryDir>

#include "test/mixxxtest.h"

using mixxx::rekordbox::beginHealthCheck;
using mixxx::rekordbox::cancelHealthChecksUnderPath;
using mixxx::rekordbox::checkTrackFiles;
using mixxx::rekordbox::endHealthCheck;
using mixxx::rekordbox::HealthCheckToken;
using mixxx::rekordbox::TrackHealth;
using mixxx::rekordbox::TrackProblem;

namespace {

// Enough of an ANLZ file for the check, which only reads the magic.
const QByteArray kGoodAnlz = QByteArrayLiteral("PMAI") + QByteArray(24, '\0');

class RekordboxTrackHealthTest : public MixxxTest {
  protected:
    void SetUp() override {
        MixxxTest::SetUp();
        ASSERT_TRUE(m_tempDir.isValid());
        m_audioPath = m_tempDir.filePath(QStringLiteral("track.mp3"));
        m_datPath = m_tempDir.filePath(QStringLiteral("ANLZ0000.DAT"));
        m_extPath = m_tempDir.filePath(QStringLiteral("ANLZ0000.EXT"));
        m_2exPath = m_tempDir.filePath(QStringLiteral("ANLZ0000.2EX"));
    }

    static void writeFile(const QString& path, const QByteArray& data) {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        ASSERT_EQ(data.size(), file.write(data));
        file.close();
    }

    void writeAudio() {
        writeFile(m_audioPath, QByteArray(1024, '\x55'));
    }

    void writeAllGoodAnlz() {
        writeFile(m_datPath, kGoodAnlz);
        writeFile(m_extPath, kGoodAnlz);
        writeFile(m_2exPath, kGoodAnlz);
    }

    QTemporaryDir m_tempDir;
    QString m_audioPath;
    QString m_datPath;
    QString m_extPath;
    QString m_2exPath;
};

} // namespace

TEST_F(RekordboxTrackHealthTest, GoodTrackHasNoProblem) {
    writeAudio();
    writeAllGoodAnlz();

    const TrackHealth health = checkTrackFiles(m_audioPath, m_datPath);
    EXPECT_EQ(TrackProblem::None, health.problem);
    EXPECT_TRUE(health.badPath.isEmpty());
}

TEST_F(RekordboxTrackHealthTest, OlderExportWithOnlyDatIsFine) {
    // Exports from before the nxs2 line have no .EXT or .2EX at all.
    writeAudio();
    writeFile(m_datPath, kGoodAnlz);

    EXPECT_EQ(TrackProblem::None, checkTrackFiles(m_audioPath, m_datPath).problem);
}

TEST_F(RekordboxTrackHealthTest, MissingAudioIsAudioMissing) {
    writeAllGoodAnlz();

    const TrackHealth health = checkTrackFiles(m_audioPath, m_datPath);
    EXPECT_EQ(TrackProblem::AudioMissing, health.problem);
    EXPECT_EQ(m_audioPath, health.badPath);
}

TEST_F(RekordboxTrackHealthTest, EmptyAudioIsAudioMissing) {
    writeFile(m_audioPath, QByteArray());
    writeAllGoodAnlz();

    const TrackHealth health = checkTrackFiles(m_audioPath, m_datPath);
    EXPECT_EQ(TrackProblem::AudioMissing, health.problem);
    EXPECT_EQ(m_audioPath, health.badPath);
}

TEST_F(RekordboxTrackHealthTest, ZeroedExtIsAnalysisDamaged) {
    // The shape of P045/00030973/ANLZ0000.EXT on the Lexar stick, which
    // aborted the app on 2026-09-27: one byte, then nothing but zeros.
    writeAudio();
    writeAllGoodAnlz();
    QByteArray zeroed(169863, '\0');
    zeroed[0] = '\x02';
    writeFile(m_extPath, zeroed);

    const TrackHealth health = checkTrackFiles(m_audioPath, m_datPath);
    EXPECT_EQ(TrackProblem::AnalysisDamaged, health.problem);
    EXPECT_EQ(m_extPath, health.badPath);
}

TEST_F(RekordboxTrackHealthTest, Truncated2ExIsWaveformDamaged) {
    // The .2EX holds only the three band waveform; cues and grid are fine.
    writeAudio();
    writeAllGoodAnlz();
    writeFile(m_2exPath, QByteArrayLiteral("PM"));

    const TrackHealth health = checkTrackFiles(m_audioPath, m_datPath);
    EXPECT_EQ(TrackProblem::WaveformDamaged, health.problem);
    EXPECT_EQ(m_2exPath, health.badPath);
}

TEST_F(RekordboxTrackHealthTest, DamagedExtWinsOverDamaged2Ex) {
    writeAudio();
    writeAllGoodAnlz();
    writeFile(m_extPath, QByteArrayLiteral("PM"));
    writeFile(m_2exPath, QByteArrayLiteral("PM"));

    const TrackHealth health = checkTrackFiles(m_audioPath, m_datPath);
    EXPECT_EQ(TrackProblem::AnalysisDamaged, health.problem);
    EXPECT_EQ(m_extPath, health.badPath);
}

TEST_F(RekordboxTrackHealthTest, NoAnalysisPathIsNotDamage) {
    // A PDB with an empty analyze_path leaves the device's directory in the
    // row. It exists and cannot be read as a file, and is still not damage.
    writeAudio();

    const TrackHealth health = checkTrackFiles(m_audioPath, m_tempDir.path());
    EXPECT_EQ(TrackProblem::None, health.problem);
    EXPECT_TRUE(health.badPath.isEmpty());
}

TEST_F(RekordboxTrackHealthTest, MissingAudioWinsOverDamagedAnalysis) {
    writeAllGoodAnlz();
    QByteArray zeroed(64, '\0');
    zeroed[0] = '\x02';
    writeFile(m_extPath, zeroed);

    const TrackHealth health = checkTrackFiles(m_audioPath, m_datPath);
    EXPECT_EQ(TrackProblem::AudioMissing, health.problem);
    EXPECT_EQ(m_audioPath, health.badPath);
}

// The registry an eject uses to stop a check before it unmounts. It is
// process-wide, so every test here ends the checks it begins.

TEST(RekordboxHealthCheckRegistryTest, CancelStopsCheckOnThatPath) {
    const HealthCheckToken token = beginHealthCheck(QStringLiteral("/media/blaise/Lexar"));
    EXPECT_FALSE(token->load());

    // SystemSettings may name the mount point with a trailing slash.
    cancelHealthChecksUnderPath(QStringLiteral("/media/blaise/Lexar/"));
    EXPECT_TRUE(token->load());

    endHealthCheck(token);
}

TEST(RekordboxHealthCheckRegistryTest, CancelStopsCheckUnderThatPath) {
    const HealthCheckToken token =
            beginHealthCheck(QStringLiteral("/media/blaise/Lexar/export"));

    cancelHealthChecksUnderPath(QStringLiteral("/media/blaise/Lexar"));
    EXPECT_TRUE(token->load());

    endHealthCheck(token);
}

TEST(RekordboxHealthCheckRegistryTest, CancelLeavesSiblingWithCommonPrefix) {
    const HealthCheckToken lexar = beginHealthCheck(QStringLiteral("/media/blaise/Lexar"));
    const HealthCheckToken lexar2 = beginHealthCheck(QStringLiteral("/media/blaise/Lexar2"));

    cancelHealthChecksUnderPath(QStringLiteral("/media/blaise/Lexar"));
    EXPECT_TRUE(lexar->load());
    EXPECT_FALSE(lexar2->load());

    endHealthCheck(lexar);
    endHealthCheck(lexar2);
}

TEST(RekordboxHealthCheckRegistryTest, CancelAfterEndLeavesTokenAlone) {
    const HealthCheckToken token = beginHealthCheck(QStringLiteral("/media/blaise/Lexar"));
    endHealthCheck(token);

    cancelHealthChecksUnderPath(QStringLiteral("/media/blaise/Lexar"));
    EXPECT_FALSE(token->load());
}

TEST(RekordboxHealthCheckRegistryTest, EmptyMountPointCancelsNothing) {
    const HealthCheckToken token = beginHealthCheck(QStringLiteral("/media/blaise/Lexar"));

    cancelHealthChecksUnderPath(QString());
    EXPECT_FALSE(token->load());

    endHealthCheck(token);
}
