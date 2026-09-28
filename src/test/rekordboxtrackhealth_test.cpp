#include "library/rekordbox/rekordboxtrackhealth.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QFile>
#include <QTemporaryDir>

#include "test/mixxxtest.h"

using mixxx::rekordbox::checkTrackFiles;
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

TEST_F(RekordboxTrackHealthTest, Truncated2ExIsAnalysisDamaged) {
    writeAudio();
    writeAllGoodAnlz();
    writeFile(m_2exPath, QByteArrayLiteral("PM"));

    const TrackHealth health = checkTrackFiles(m_audioPath, m_datPath);
    EXPECT_EQ(TrackProblem::AnalysisDamaged, health.problem);
    EXPECT_EQ(m_2exPath, health.badPath);
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
