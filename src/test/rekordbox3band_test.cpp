#include "waveform/rekordbox3bandwaveform.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QStringList>
#include <QTemporaryDir>
#include <QVector>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

#include "library/rekordbox/rekordbox3bandimport.h"
#include "preferences/configobject.h"
#include "test/mixxxtest.h"
#include "track/track.h"
#include "waveform/renderers/allshader/rekordbox3bandcalibration.h"
#include "waveform/waveform.h"
#include "waveform/widgets/waveformwidgettype.h"

namespace {

constexpr auto kSampleRate = mixxx::audio::SampleRate(44100);

/// PWV6 is always exactly this many entries, whatever the track's length.
constexpr int kPwv6Entries = 1200;

/// The third u4 of a PWV7 tag header. 0x96 is 150, the entries per second.
constexpr uint32_t kPwv7RateField = 0x00960000;

/// Builds `.2EX` files byte by byte, so the tests drive the real kaitai parser
/// rather than a stand-in. Every fixture here is synthetic: no rekordbox
/// analysis data is committed to this repository.
///
/// Layout follows lib/rekordbox-metadata/rekordbox_anlz.ksy. A three band tag
/// is `fourcc` + u4 len_header + u4 len_tag, then u4 len_entry_bytes, u4
/// len_entries, then the entry blob. PWV7 has one more u4 between the count and
/// the blob and PWV6 does not, which is why their headers are 24 and 20 bytes.
class Anlz2ExBuilder {
  public:
    /// A malformed tag is built through this, by lying about `lenEntryBytes` or
    /// `declaredEntries` relative to what `payload` actually holds.
    void addBandTag(const char* fourcc,
            bool hasRateField,
            uint32_t lenEntryBytes,
            uint32_t declaredEntries,
            const QByteArray& payload) {
        QByteArray body;
        appendU32(&body, lenEntryBytes);
        appendU32(&body, declaredEntries);
        if (hasRateField) {
            appendU32(&body, kPwv7RateField);
        }
        body.append(payload);

        QByteArray section;
        section.append(fourcc, 4);
        appendU32(&section, static_cast<uint32_t>(hasRateField ? 24 : 20));
        appendU32(&section, static_cast<uint32_t>(12 + body.size()));
        section.append(body);
        m_sections.append(section);
    }

    void addPwv7(const QVector<mixxx::BandSample>& entries) {
        addBandTag("PWV7", true, 3, static_cast<uint32_t>(entries.size()), pack(entries));
    }

    void addPwv6(const QVector<mixxx::BandSample>& entries) {
        addBandTag("PWV6", false, 3, static_cast<uint32_t>(entries.size()), pack(entries));
    }

    /// A tag the importer has no interest in, so that the section walk has to
    /// skip something to reach the three band tags. The fourcc is deliberately
    /// one the generated parser does not know, which makes it an unknown_tag_t
    /// whose body is never inspected.
    void addFillerTag() {
        QByteArray section;
        section.append("PZZZ", 4);
        appendU32(&section, 12); // len_header
        appendU32(&section, 20); // len_tag: the 12 byte header plus 8 of body
        section.append(8, '\0');
        m_sections.append(section);
    }

    QByteArray build() const {
        QByteArray header;
        header.append("PMAI", 4);
        appendU32(&header, 0x1c); // len_header
        appendU32(&header,
                static_cast<uint32_t>(0x1c + m_sections.size())); // len_file
        header.append(0x1c - header.size(), '\0');
        return header + m_sections;
    }

    static QByteArray pack(const QVector<mixxx::BandSample>& entries) {
        QByteArray payload;
        payload.reserve(entries.size() * 3);
        for (const mixxx::BandSample& entry : entries) {
            payload.append(static_cast<char>(entry.low));
            payload.append(static_cast<char>(entry.mid));
            payload.append(static_cast<char>(entry.high));
        }
        return payload;
    }

  private:
    static void appendU32(QByteArray* pOut, uint32_t value) {
        // Every multi-byte field in an ANLZ file is big endian.
        for (int shift = 24; shift >= 0; shift -= 8) {
            pOut->append(static_cast<char>((value >> shift) & 0xff));
        }
    }

    QByteArray m_sections;
};

/// A recognisable ramp, distinct in all three bands so a swapped byte shows up.
QVector<mixxx::BandSample> rampEntries(int count) {
    QVector<mixxx::BandSample> entries;
    entries.reserve(count);
    for (int i = 0; i < count; ++i) {
        entries.append(mixxx::BandSample{
                static_cast<uint8_t>((i * 1) % 127),
                static_cast<uint8_t>((i * 3) % 127),
                static_cast<uint8_t>((i * 7) % 127)});
    }
    return entries;
}

class Rekordbox3BandTest : public MixxxTest {
  protected:
    void SetUp() override {
        MixxxTest::SetUp();
        ASSERT_TRUE(m_tempDir.isValid());
    }

    TrackPointer createTrack() {
        const auto pTrack = Track::newTemporary(mixxx::FileAccess(
                mixxx::FileInfo(getTestDir().filePath(QStringLiteral("sine-30.wav")))));
        pTrack->setAudioProperties(
                mixxx::audio::ChannelCount(2),
                kSampleRate,
                mixxx::audio::Bitrate(),
                mixxx::Duration::fromSeconds(180));
        return pTrack;
    }

    /// Writes the builder's bytes into the temporary directory. Out-parameter
    /// rather than a return value because ASSERT_* may only be used in a
    /// function returning void.
    void writeFile(const Anlz2ExBuilder& builder, QString* pPath) {
        const QString path = m_tempDir.filePath(
                QStringLiteral("ANLZ%1.2EX").arg(m_fileCounter++));
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        const QByteArray data = builder.build();
        ASSERT_EQ(data.size(), file.write(data));
        file.close();
        *pPath = path;
    }

    /// A path inside the temporary directory that deliberately does not exist.
    QString missingPath() const {
        return m_tempDir.filePath(QStringLiteral("ANLZ-absent.2EX"));
    }

    QTemporaryDir m_tempDir;
    int m_fileCounter = 0;
};

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

// PWV6 is the whole track overview and is always 1200 entries.
TEST_F(Rekordbox3BandTest, Pwv6PreviewParses) {
    const QVector<mixxx::BandSample> entries = rampEntries(kPwv6Entries);
    Anlz2ExBuilder builder;
    builder.addFillerTag();
    builder.addPwv6(entries);

    QString path;
    ASSERT_NO_FATAL_FAILURE(writeFile(builder, &path));

    const auto pBands = mixxx::Rekordbox3BandWaveform::fromAnlz2Ex(path);
    ASSERT_FALSE(pBands.isNull());
    EXPECT_EQ(mixxx::Rekordbox3BandWaveform::Source::NativePwv7, pBands->source());
    ASSERT_TRUE(pBands->hasPreview());
    ASSERT_EQ(kPwv6Entries, pBands->preview().size());
    for (int i = 0; i < kPwv6Entries; ++i) {
        ASSERT_EQ(entries[i].low, pBands->preview()[i].low) << "entry " << i;
        ASSERT_EQ(entries[i].mid, pBands->preview()[i].mid) << "entry " << i;
        ASSERT_EQ(entries[i].high, pBands->preview()[i].high) << "entry " << i;
    }
}

// PWV7 is the scrolling waveform, sampled at a fixed 150 entries per second.
TEST_F(Rekordbox3BandTest, Pwv7DetailParsesAtOneFiftyEntriesPerSecond) {
    const QVector<mixxx::BandSample> entries = rampEntries(4500); // 30 seconds
    Anlz2ExBuilder builder;
    builder.addPwv7(entries);
    builder.addFillerTag();

    QString path;
    ASSERT_NO_FATAL_FAILURE(writeFile(builder, &path));

    const auto pBands = mixxx::Rekordbox3BandWaveform::fromAnlz2Ex(path);
    ASSERT_FALSE(pBands.isNull());
    ASSERT_TRUE(pBands->hasDetail());
    EXPECT_EQ(4500, pBands->detail().size());
    EXPECT_DOUBLE_EQ(150.0, pBands->detailEntriesPerSecond());
    EXPECT_FALSE(pBands->hasPreview());
    EXPECT_EQ(entries[123].low, pBands->detail()[123].low);
    EXPECT_EQ(entries[123].mid, pBands->detail()[123].mid);
    EXPECT_EQ(entries[123].high, pBands->detail()[123].high);
}

// The byte order inside an entry is byte0 = low, byte1 = mid, byte2 = high.
// This was measured against real analysis files (a 435 Hz sine gives 32/31/0,
// band limited noise around 1 kHz gives 7/38/5, bass heavy music 56/27/7), and
// third party write-ups claiming mid/high/low are wrong. The payload here is
// written as literal bytes rather than through BandSample so that a matching
// mistake in the builder cannot hide a matching mistake in the parser.
TEST_F(Rekordbox3BandTest, ByteOrderIsLowMidHigh) {
    QByteArray payload;
    payload.append(static_cast<char>(0x11)); // entry 0 low
    payload.append(static_cast<char>(0x22)); // entry 0 mid
    payload.append(static_cast<char>(0x33)); // entry 0 high
    payload.append(static_cast<char>(0x44)); // entry 1 low
    payload.append(static_cast<char>(0x55)); // entry 1 mid
    payload.append(static_cast<char>(0x66)); // entry 1 high

    Anlz2ExBuilder builder;
    builder.addBandTag("PWV7", true, 3, 2, payload);
    builder.addBandTag("PWV6", false, 3, 2, payload);

    QString path;
    ASSERT_NO_FATAL_FAILURE(writeFile(builder, &path));

    const auto pBands = mixxx::Rekordbox3BandWaveform::fromAnlz2Ex(path);
    ASSERT_FALSE(pBands.isNull());
    ASSERT_EQ(2, pBands->detail().size());
    ASSERT_EQ(2, pBands->preview().size());

    EXPECT_EQ(0x11, pBands->detail()[0].low);
    EXPECT_EQ(0x22, pBands->detail()[0].mid);
    EXPECT_EQ(0x33, pBands->detail()[0].high);
    EXPECT_EQ(0x44, pBands->detail()[1].low);
    EXPECT_EQ(0x55, pBands->detail()[1].mid);
    EXPECT_EQ(0x66, pBands->detail()[1].high);

    // The preview series is read by the same code path and must agree.
    EXPECT_EQ(0x11, pBands->preview()[0].low);
    EXPECT_EQ(0x22, pBands->preview()[0].mid);
    EXPECT_EQ(0x33, pBands->preview()[0].high);
}

// A tag that declares anything other than three bytes per entry is not a three
// band tag whatever its fourcc says. It is dropped, and the rest of the file is
// still read.
TEST_F(Rekordbox3BandTest, WrongEntrySizeIsIgnored) {
    const QVector<mixxx::BandSample> preview = rampEntries(kPwv6Entries);

    Anlz2ExBuilder builder;
    // Four bytes per entry, and a payload that is genuinely 4 * 10 bytes, so
    // the parse succeeds and only the entry size check can reject it.
    builder.addBandTag("PWV7", true, 4, 10, QByteArray(40, '\x7f'));
    builder.addPwv6(preview);

    QString path;
    ASSERT_NO_FATAL_FAILURE(writeFile(builder, &path));

    const auto pBands = mixxx::Rekordbox3BandWaveform::fromAnlz2Ex(path);
    ASSERT_FALSE(pBands.isNull());
    EXPECT_FALSE(pBands->hasDetail());
    EXPECT_TRUE(pBands->hasPreview());
    EXPECT_EQ(kPwv6Entries, pBands->preview().size());
    // No detail series means no rate to report.
    EXPECT_DOUBLE_EQ(0.0, pBands->detailEntriesPerSecond());
}

// A truncated file must be refused rather than yielding a run of zero entries
// that would draw as silence. The declared count runs past the end of the tag,
// which kaitai reports as a stream error, and the whole file is rejected.
TEST_F(Rekordbox3BandTest, TruncatedEntryDataIsRejected) {
    Anlz2ExBuilder builder;
    builder.addBandTag("PWV7", true, 3, 100, QByteArray(30, '\x40'));

    QString path;
    ASSERT_NO_FATAL_FAILURE(writeFile(builder, &path));

    const auto pBands = mixxx::Rekordbox3BandWaveform::fromAnlz2Ex(path);
    EXPECT_TRUE(pBands.isNull());
}

// A file that holds neither tag, and a file that is not an ANLZ file at all,
// are both a null pointer rather than an exception escaping into the caller.
TEST_F(Rekordbox3BandTest, FileWithoutBandTagsIsNull) {
    Anlz2ExBuilder builder;
    builder.addFillerTag();

    QString path;
    ASSERT_NO_FATAL_FAILURE(writeFile(builder, &path));
    EXPECT_TRUE(mixxx::Rekordbox3BandWaveform::fromAnlz2Ex(path).isNull());

    const QString foreign = m_tempDir.filePath(QStringLiteral("foreign.2EX"));
    QFile file(foreign);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("not an anlz file at all, not even close");
    file.close();
    EXPECT_TRUE(mixxx::Rekordbox3BandWaveform::fromAnlz2Ex(foreign).isNull());

    EXPECT_TRUE(mixxx::Rekordbox3BandWaveform::fromAnlz2Ex(missingPath()).isNull());
    EXPECT_TRUE(mixxx::Rekordbox3BandWaveform::fromAnlz2Ex(QString()).isNull());
}

// PWV6 without PWV7: the overview is drawable, the scrolling waveform is not.
TEST_F(Rekordbox3BandTest, PreviewOnlyFileHasNoDetail) {
    Anlz2ExBuilder builder;
    builder.addPwv6(rampEntries(kPwv6Entries));

    QString path;
    ASSERT_NO_FATAL_FAILURE(writeFile(builder, &path));

    const auto pBands = mixxx::Rekordbox3BandWaveform::fromAnlz2Ex(path);
    ASSERT_FALSE(pBands.isNull());
    EXPECT_TRUE(pBands->hasPreview());
    EXPECT_FALSE(pBands->hasDetail());
    EXPECT_TRUE(pBands->detail().isEmpty());
}

// A PWV6-only file must not leave the scrolling pane empty. The ladder is
// native detail, then a Mixxx approximation, then nothing, and skipping the
// middle rung was the original behaviour here: the overview drew rekordbox's
// own numbers while the waveform drew nothing at all.
TEST_F(Rekordbox3BandTest, PreviewOnlyFileFallsBackForTheScrollingWaveform) {
    Anlz2ExBuilder builder;
    builder.addPwv6(rampEntries(kPwv6Entries));

    QString path;
    ASSERT_NO_FATAL_FAILURE(writeFile(builder, &path));

    const TrackPointer pTrack = createTrack();
    pTrack->setWaveform(ConstWaveformPointer(new Waveform(44100, 441000, 441, -1)));
    mixxx::rekordbox::read3BandWaveform(pTrack, path);

    // The importer attached PWV6 and nothing else.
    ASSERT_FALSE(pTrack->getRekordbox3BandWaveform().isNull());
    EXPECT_FALSE(pTrack->getRekordbox3BandWaveform()->hasDetail());

    // Resolving fills the detail from Mixxx while keeping the native preview,
    // so the overview still shows rekordbox's numbers and the waveform draws.
    const mixxx::ConstRekordbox3BandWaveformPointer pResolved =
            mixxx::resolveRekordbox3BandWaveform(pTrack);
    ASSERT_FALSE(pResolved.isNull());
    EXPECT_TRUE(pResolved->hasDetail());
    EXPECT_TRUE(pResolved->hasPreview());
    EXPECT_EQ(kPwv6Entries, pResolved->preview().size());
    EXPECT_EQ(mixxx::Rekordbox3BandWaveform::Source::MixxxFallback, pResolved->source());
}

// ---------------------------------------------------------------------------
// The importer's contract with the track
// ---------------------------------------------------------------------------

// A track with no .2EX sibling gets no three band data, and nothing else about
// the track is disturbed. Mixxx's own waveform in particular must survive: the
// fallback is built later, by resolveRekordbox3BandWaveform(), not here.
TEST_F(Rekordbox3BandTest, MissingFileLeavesTrackUntouched) {
    const TrackPointer pTrack = createTrack();
    const ConstWaveformPointer pWaveform(new Waveform(44100, 441000, 441, -1));
    pTrack->setWaveform(pWaveform);

    mixxx::rekordbox::read3BandWaveform(pTrack, missingPath());

    EXPECT_TRUE(pTrack->getRekordbox3BandWaveform().isNull());
    EXPECT_EQ(pWaveform.data(), pTrack->getWaveform().data());
}

// getTrack() hands back a cached Track, so the importer clears first and
// unconditionally. Without that, a track with no .2EX of its own keeps showing
// the previous track's waveform, which is the worst possible failure: it looks
// like it works.
TEST_F(Rekordbox3BandTest, ImportClearsStaleDataWhenFileIsGone) {
    Anlz2ExBuilder builder;
    builder.addPwv7(rampEntries(3000));
    builder.addPwv6(rampEntries(kPwv6Entries));

    QString path;
    ASSERT_NO_FATAL_FAILURE(writeFile(builder, &path));

    const TrackPointer pTrack = createTrack();
    mixxx::rekordbox::read3BandWaveform(pTrack, path);
    ASSERT_FALSE(pTrack->getRekordbox3BandWaveform().isNull());
    ASSERT_TRUE(pTrack->getRekordbox3BandWaveform()->hasDetail());

    // Same track object, now standing in for a track that has no .2EX.
    mixxx::rekordbox::read3BandWaveform(pTrack, missingPath());
    EXPECT_TRUE(pTrack->getRekordbox3BandWaveform().isNull());

    // And a file that exists but is unusable clears it too.
    mixxx::rekordbox::read3BandWaveform(pTrack, path);
    ASSERT_FALSE(pTrack->getRekordbox3BandWaveform().isNull());
    Anlz2ExBuilder emptyBuilder;
    emptyBuilder.addFillerTag();
    QString emptyPath;
    ASSERT_NO_FATAL_FAILURE(writeFile(emptyBuilder, &emptyPath));
    mixxx::rekordbox::read3BandWaveform(pTrack, emptyPath);
    EXPECT_TRUE(pTrack->getRekordbox3BandWaveform().isNull());
}

// A good file attaches data and reports which series it found.
TEST_F(Rekordbox3BandTest, ImportAttachesNativeData) {
    Anlz2ExBuilder builder;
    builder.addPwv7(rampEntries(3000));
    builder.addPwv6(rampEntries(kPwv6Entries));

    QString path;
    ASSERT_NO_FATAL_FAILURE(writeFile(builder, &path));

    const TrackPointer pTrack = createTrack();
    mixxx::rekordbox::read3BandWaveform(pTrack, path);

    const mixxx::ConstRekordbox3BandWaveformPointer pBands =
            pTrack->getRekordbox3BandWaveform();
    ASSERT_FALSE(pBands.isNull());
    EXPECT_EQ(mixxx::Rekordbox3BandWaveform::Source::NativePwv7, pBands->source());
    EXPECT_EQ(3000, pBands->detail().size());
    EXPECT_EQ(kPwv6Entries, pBands->preview().size());
}

// ---------------------------------------------------------------------------
// Position mapping
// ---------------------------------------------------------------------------

// The mapping is `normalized * (count - 1)` rounded, so 0.0 lands on the first
// entry and 1.0 on the last one exactly rather than one past it. Both accessors
// clamp, and both return -1 for an empty series.
TEST_F(Rekordbox3BandTest, PositionMappingIsExactAtBothEnds) {
    Anlz2ExBuilder builder;
    builder.addPwv7(rampEntries(5));
    builder.addPwv6(rampEntries(9));

    QString path;
    ASSERT_NO_FATAL_FAILURE(writeFile(builder, &path));

    const auto pBands = mixxx::Rekordbox3BandWaveform::fromAnlz2Ex(path);
    ASSERT_FALSE(pBands.isNull());
    ASSERT_EQ(5, pBands->detail().size());
    ASSERT_EQ(9, pBands->preview().size());

    EXPECT_EQ(0, pBands->detailIndexForPosition(0.0));
    EXPECT_EQ(2, pBands->detailIndexForPosition(0.5));
    EXPECT_EQ(4, pBands->detailIndexForPosition(1.0));

    EXPECT_EQ(0, pBands->previewIndexForPosition(0.0));
    EXPECT_EQ(4, pBands->previewIndexForPosition(0.5));
    EXPECT_EQ(8, pBands->previewIndexForPosition(1.0));

    // Out of range positions clamp rather than indexing out of the vector.
    EXPECT_EQ(0, pBands->detailIndexForPosition(-1.0));
    EXPECT_EQ(4, pBands->detailIndexForPosition(2.0));
    EXPECT_EQ(0, pBands->previewIndexForPosition(-0.001));
    EXPECT_EQ(8, pBands->previewIndexForPosition(1.001));
}

// An absent series answers -1 rather than 0, so a caller cannot mistake "no
// data" for "the first entry".
TEST_F(Rekordbox3BandTest, PositionMappingOnAnEmptySeriesIsMinusOne) {
    Anlz2ExBuilder builder;
    builder.addPwv6(rampEntries(kPwv6Entries));

    QString path;
    ASSERT_NO_FATAL_FAILURE(writeFile(builder, &path));

    const auto pBands = mixxx::Rekordbox3BandWaveform::fromAnlz2Ex(path);
    ASSERT_FALSE(pBands.isNull());
    ASSERT_FALSE(pBands->hasDetail());

    EXPECT_EQ(-1, pBands->detailIndexForPosition(0.0));
    EXPECT_EQ(-1, pBands->detailIndexForPosition(0.5));
    EXPECT_EQ(-1, pBands->detailIndexForPosition(1.0));
}

// ---------------------------------------------------------------------------
// Preference stability
// ---------------------------------------------------------------------------

// The waveform types are referenced from the sorted preferences by number, and
// they are persisted by number in mixxx.cfg. Renumbering any of them silently
// changes which waveform an existing installation draws, so the values are
// pinned here. Compile time, because a change must not even build.
static_assert(WaveformWidgetType::AllShaderRGBWaveform == 17, "RGB moved");
static_assert(WaveformWidgetType::AllShaderFilteredWaveform == 19, "Filt moved");
static_assert(WaveformWidgetType::AllShaderTexturedFiltered == 22, "Filt HD moved");
static_assert(WaveformWidgetType::AllShaderTexturedStacked == 24, "Stack HD moved");
static_assert(WaveformWidgetType::AllShaderRGBStackedWaveform == 25, "Stack moved");
static_assert(WaveformWidgetType::AllShaderRekordbox3BandWaveform == 26, "3Band wrong");
static_assert(WaveformWidgetType::Count_WaveformwidgetType == 27, "Count wrong");

TEST(Rekordbox3BandEnumTest, WaveformWidgetTypeValuesAreStable) {
    // The static_asserts above are the real test; this one exists so that the
    // guarantee is visible in the test list and so a failure names the file.
    EXPECT_EQ(26, static_cast<int>(WaveformWidgetType::AllShaderRekordbox3BandWaveform));
    EXPECT_EQ(27, static_cast<int>(WaveformWidgetType::Count_WaveformwidgetType));
}

// WaveformWidgetFactory writes the chosen type through
// `m_config->setValue(ConfigKey("[Waveform]", "WaveformType"), type)` and reads
// it back at startup with `getValueString(...).toInt(&ok)`. Constructing the
// factory needs a GL context, which a headless unit test does not have, so this
// exercises that ConfigValue round trip directly, across a real save and
// reload, rather than faking a factory.
TEST_F(Rekordbox3BandTest, WaveformTypeConfigValueRoundTrips) {
    const ConfigKey key(QStringLiteral("[Waveform]"), QStringLiteral("WaveformType"));

    config()->setValue(key, WaveformWidgetType::AllShaderRekordbox3BandWaveform);
    saveAndReloadConfig();

    bool ok = false;
    const int stored = config()->getValueString(key).toInt(&ok);
    ASSERT_TRUE(ok) << "the stored value is not an integer: "
                    << config()->getValueString(key).toStdString();
    EXPECT_EQ(26, stored);
    EXPECT_EQ(WaveformWidgetType::AllShaderRekordbox3BandWaveform,
            static_cast<WaveformWidgetType::Type>(stored));
}

// ---------------------------------------------------------------------------
// The pure functions the renderer and the reference rasterizer share
// ---------------------------------------------------------------------------

TEST(Rekordbox3BandCalibrationTest, ScaleHeightZeroSampleDrawsNothing) {
    // Silence is background, not a hairline, so the minimum visible height must
    // not apply to a zero sample.
    EXPECT_FLOAT_EQ(0.0f, mixxx::scaleHeight(0, 127.0f, 1.0f, 1.0f, 100.0f, 1.0f));
    // Degenerate inputs are zero rather than a division or a negative height.
    EXPECT_FLOAT_EQ(0.0f, mixxx::scaleHeight(64, 0.0f, 1.0f, 1.0f, 100.0f, 1.0f));
    EXPECT_FLOAT_EQ(0.0f, mixxx::scaleHeight(64, 127.0f, 1.0f, 1.0f, 0.0f, 1.0f));
    EXPECT_FLOAT_EQ(0.0f, mixxx::scaleHeight(64, 127.0f, 1.0f, 0.0f, 100.0f, 1.0f));
}

TEST(Rekordbox3BandCalibrationTest, ScaleHeightAtFullScaleFillsTheHalfHeight) {
    EXPECT_FLOAT_EQ(100.0f, mixxx::scaleHeight(127, 127.0f, 1.0f, 1.0f, 100.0f, 1.0f));
    // Half of full scale is half the pane, with the default unit gamma.
    EXPECT_FLOAT_EQ(50.0f, mixxx::scaleHeight(64, 128.0f, 1.0f, 1.0f, 100.0f, 1.0f));
}

TEST(Rekordbox3BandCalibrationTest, ScaleHeightAppliesGamma) {
    // A gamma below one lifts quiet passages: 0.25 ^ 0.5 = 0.5.
    EXPECT_FLOAT_EQ(50.0f, mixxx::scaleHeight(32, 128.0f, 0.5f, 1.0f, 100.0f, 1.0f));
    // A gamma above one pushes them down: 0.5 ^ 2 = 0.25.
    EXPECT_FLOAT_EQ(25.0f, mixxx::scaleHeight(64, 128.0f, 2.0f, 1.0f, 100.0f, 1.0f));
    // Unit gamma is the identity, and takes the shortcut path.
    EXPECT_FLOAT_EQ(25.0f, mixxx::scaleHeight(32, 128.0f, 1.0f, 1.0f, 100.0f, 1.0f));
}

TEST(Rekordbox3BandCalibrationTest, ScaleHeightRaisesQuietSamplesToTheFloor) {
    // 1/127 of a 100 pixel half height is 0.79 px, which would disappear.
    EXPECT_FLOAT_EQ(1.0f, mixxx::scaleHeight(1, 127.0f, 1.0f, 1.0f, 100.0f, 1.0f));
    EXPECT_FLOAT_EQ(3.0f, mixxx::scaleHeight(1, 127.0f, 1.0f, 1.0f, 100.0f, 3.0f));
    // The floor never pushes a band outside a pane shorter than the floor.
    EXPECT_FLOAT_EQ(0.5f, mixxx::scaleHeight(1, 127.0f, 1.0f, 1.0f, 0.5f, 4.0f));
}

TEST(Rekordbox3BandCalibrationTest, ScaleHeightClampsToTheHalfHeight) {
    // A boosted band scale cannot draw outside the pane.
    EXPECT_FLOAT_EQ(100.0f, mixxx::scaleHeight(127, 127.0f, 1.0f, 4.0f, 100.0f, 1.0f));
    // Nor can a sample above the declared full scale.
    EXPECT_FLOAT_EQ(100.0f,
            mixxx::scaleHeight(
                    static_cast<uint8_t>(255), 127.0f, 1.0f, 1.0f, 100.0f, 1.0f));
}

TEST(Rekordbox3BandCalibrationTest, ColumnRangeCoversStartMiddleAndEnd) {
    // The whole of a 100 entry series across 10 pixels: ten slices of the 0..99
    // index range, each widened to whole entries so no entry is skipped.
    const mixxx::Rekordbox3BandColumnRange first =
            mixxx::columnRangeForPixel(0, 10, 0.0, 1.0, 100);
    EXPECT_EQ(0, first.begin);
    EXPECT_EQ(10, first.end);

    const mixxx::Rekordbox3BandColumnRange middle =
            mixxx::columnRangeForPixel(5, 10, 0.0, 1.0, 100);
    EXPECT_EQ(49, middle.begin);
    EXPECT_EQ(60, middle.end);

    // The right hand edge of the last column lands exactly on the last index,
    // and the range is half open, so the end has to be that index plus one or
    // entry 99 is never reduced into any column at all. Taking the ceiling here
    // instead silently drops the final entry of every full width window, which
    // is one column in 1200 of the PWV6 overview.
    const mixxx::Rekordbox3BandColumnRange last =
            mixxx::columnRangeForPixel(9, 10, 0.0, 1.0, 100);
    EXPECT_EQ(89, last.begin);
    EXPECT_EQ(100, last.end);
    // Nothing runs off the end.
    EXPECT_LE(last.end, 100);

    // Every entry lands in some column: no gaps and nothing dropped.
    QVector<bool> covered(100, false);
    for (int pixel = 0; pixel < 10; ++pixel) {
        const mixxx::Rekordbox3BandColumnRange range =
                mixxx::columnRangeForPixel(pixel, 10, 0.0, 1.0, 100);
        for (int i = range.begin; i < range.end; ++i) {
            covered[i] = true;
        }
    }
    for (int i = 0; i < 100; ++i) {
        EXPECT_TRUE(covered[i]) << "entry " << i << " is never drawn";
    }
}

TEST(Rekordbox3BandCalibrationTest, ColumnRangeIsAlwaysAtLeastOneEntryWide) {
    // Zoomed in far enough that many pixels share one entry, every column still
    // has something to reduce.
    for (int pixel = 0; pixel < 200; ++pixel) {
        const mixxx::Rekordbox3BandColumnRange range =
                mixxx::columnRangeForPixel(pixel, 200, 0.10, 0.11, 1000);
        ASSERT_GE(range.end - range.begin, 1) << "pixel " << pixel;
        ASSERT_GE(range.begin, 0) << "pixel " << pixel;
        ASSERT_LE(range.end, 1000) << "pixel " << pixel;
    }
}

TEST(Rekordbox3BandCalibrationTest, ColumnRangeClampsIntoTheSeries) {
    // A window that runs past either end of the track is clamped rather than
    // indexing out of the vector.
    const mixxx::Rekordbox3BandColumnRange before =
            mixxx::columnRangeForPixel(0, 10, -0.5, 0.5, 100);
    EXPECT_EQ(0, before.begin);
    EXPECT_GE(before.end, 1);

    const mixxx::Rekordbox3BandColumnRange after =
            mixxx::columnRangeForPixel(9, 10, 0.5, 1.5, 100);
    EXPECT_LE(after.end, 100);
    EXPECT_LT(after.begin, after.end);

    // An empty series has no range at all.
    const mixxx::Rekordbox3BandColumnRange none =
            mixxx::columnRangeForPixel(0, 10, 0.0, 1.0, 0);
    EXPECT_EQ(0, none.begin);
    EXPECT_EQ(0, none.end);

    const mixxx::Rekordbox3BandColumnRange noPixels =
            mixxx::columnRangeForPixel(0, 0, 0.0, 1.0, 100);
    EXPECT_EQ(0, noPixels.begin);
    EXPECT_EQ(0, noPixels.end);
}

TEST(Rekordbox3BandCalibrationTest, ColourForCoverageReturnsTheEightTableColours) {
    const mixxx::Rekordbox3BandCalibration cal;

    EXPECT_EQ(cal.background.rgb(),
            mixxx::colourForCoverage(cal, false, false, false).rgb());
    EXPECT_EQ(cal.high.rgb(), mixxx::colourForCoverage(cal, false, false, true).rgb());
    EXPECT_EQ(cal.mid.rgb(), mixxx::colourForCoverage(cal, false, true, false).rgb());
    EXPECT_EQ(cal.midHigh.rgb(), mixxx::colourForCoverage(cal, false, true, true).rgb());
    EXPECT_EQ(cal.low.rgb(), mixxx::colourForCoverage(cal, true, false, false).rgb());
    EXPECT_EQ(cal.lowHigh.rgb(), mixxx::colourForCoverage(cal, true, false, true).rgb());
    EXPECT_EQ(cal.lowMid.rgb(), mixxx::colourForCoverage(cal, true, true, false).rgb());
    EXPECT_EQ(cal.lowMidHigh.rgb(),
            mixxx::colourForCoverage(cal, true, true, true).rgb());

    // The eight are distinct, so a coverage combination cannot be confused for
    // another one on screen.
    const QRgb table[8] = {
            mixxx::colourForCoverage(cal, false, false, false).rgb(),
            mixxx::colourForCoverage(cal, false, false, true).rgb(),
            mixxx::colourForCoverage(cal, false, true, false).rgb(),
            mixxx::colourForCoverage(cal, false, true, true).rgb(),
            mixxx::colourForCoverage(cal, true, false, false).rgb(),
            mixxx::colourForCoverage(cal, true, false, true).rgb(),
            mixxx::colourForCoverage(cal, true, true, false).rgb(),
            mixxx::colourForCoverage(cal, true, true, true).rgb(),
    };
    for (int i = 0; i < 8; ++i) {
        for (int j = i + 1; j < 8; ++j) {
            EXPECT_NE(table[i], table[j]) << "coverage " << i << " and " << j;
        }
    }
}

// An antialiased edge has to meet the flat interior it borders with no seam, so
// at whole coverage the partial blend must reproduce the coverage table exactly
// rather than approximately. All eight corners, not just the four singles.
TEST(Rekordbox3BandCalibrationTest, BlendPartialCoverageMatchesTheTableAtEveryCorner) {
    const mixxx::Rekordbox3BandCalibration cal;

    for (int low = 0; low < 2; ++low) {
        for (int mid = 0; mid < 2; ++mid) {
            for (int high = 0; high < 2; ++high) {
                const QColor expected =
                        mixxx::colourForCoverage(cal, low != 0, mid != 0, high != 0);
                const QColor actual = mixxx::blendPartialCoverage(cal,
                        static_cast<float>(low),
                        static_cast<float>(mid),
                        static_cast<float>(high));
                // Compared as 8 bit RGB, which is what both the table and the
                // framebuffer are; QColor's own operator== compares its
                // internal 16 bit channels and its colour spec as well.
                EXPECT_EQ(expected.rgb(), actual.rgb())
                        << "coverage low=" << low << " mid=" << mid
                        << " high=" << high;
            }
        }
    }

    // Coverages outside [0,1] clamp onto the corners rather than extrapolating
    // into a colour that is in no table.
    EXPECT_EQ(cal.lowMidHigh.rgb(),
            mixxx::blendPartialCoverage(cal, 2.0f, 2.0f, 2.0f).rgb());
    EXPECT_EQ(cal.background.rgb(),
            mixxx::blendPartialCoverage(cal, -1.0f, -1.0f, -1.0f).rgb());
}

// ---------------------------------------------------------------------------
// Antialiasing the band boundaries
// ---------------------------------------------------------------------------

namespace {

/// The rasterizer as it was before antialiasing existed, kept verbatim so that
/// "antialiasWidthPx = 0 changes nothing" is measured against the real thing
/// rather than against a description of it.
QColor flatColourAt(const mixxx::Rekordbox3BandCalibration& cal,
        const float heights[3],
        float dy) {
    return mixxx::colourForCoverage(
            cal, dy <= heights[0], dy <= heights[1], dy <= heights[2]);
}

QColor spanColourAt(const mixxx::Rekordbox3BandCalibration& cal,
        const float heights[3],
        float halfHeight,
        float dy) {
    mixxx::Rekordbox3BandSpan spans[mixxx::kRekordbox3BandMaxSpans];
    const int count = mixxx::rekordbox3BandSpans(
            cal, heights[0], heights[1], heights[2], halfHeight, spans);
    return mixxx::rekordbox3BandColourAt(cal, spans, count, dy);
}

} // namespace

// The compatibility escape hatch. Turning antialiasing off has to give back the
// hard-edged picture exactly, or there is no way to tell a rendering change from
// an antialiasing change when one of them goes wrong.
//
// Odd pane heights are in the sample deliberately: only there does a pixel
// centre land exactly on the centre line, which is the one place the flat
// rasterizer and the span list could disagree about a silent band.
TEST(Rekordbox3BandAntialiasTest, ZeroWidthReproducesTheFlatRasterizer) {
    mixxx::Rekordbox3BandCalibration cal;
    cal.antialiasWidthPx = 0.0f;

    // A fixed sequence rather than a random one, so a failure is reproducible.
    uint32_t state = 12345u;
    const auto nextValue = [&state]() {
        state = state * 1103515245u + 12345u;
        return static_cast<uint8_t>((state >> 16) & 0x7Fu);
    };

    const int heightsToTest[] = {200, 101, 60, 7};
    for (int paneHeight : heightsToTest) {
        const float half = static_cast<float>(paneHeight) / 2.0f;
        for (int column = 0; column < 2000; ++column) {
            float heights[3];
            for (int band = 0; band < 3; ++band) {
                heights[band] = mixxx::scaleHeight(nextValue(),
                        cal.pwv7FullScale,
                        cal.gamma,
                        1.0f,
                        half,
                        cal.minVisibleHeightPx);
            }
            for (int y = 0; y < paneHeight; ++y) {
                const float dy = std::fabs(static_cast<float>(y) + 0.5f - half);
                ASSERT_EQ(flatColourAt(cal, heights, dy).rgb(),
                        spanColourAt(cal, heights, half, dy).rgb())
                        << "pane " << paneHeight << " row " << y << " heights "
                        << heights[0] << " " << heights[1] << " " << heights[2];
            }
        }
    }
}

// The whole point of the span list: a transition is a straight line between two
// table colours, so the middle of one is the same colour blendPartialCoverage()
// gives for that band at half coverage. Exactly, not nearly.
TEST(Rekordbox3BandAntialiasTest, StripMidpointIsHalfCoverage) {
    const mixxx::Rekordbox3BandCalibration cal;
    ASSERT_GT(cal.antialiasWidthPx, 0.0f);

    // Far enough apart that no strip is truncated by a neighbour.
    const float heights[3] = {20.0f, 40.0f, 60.0f};
    const float half = 100.0f;

    struct Expectation {
        float dy;
        float low;
        float mid;
        float high;
    };
    // At the low boundary the low band is half covered and the other two, which
    // reach further, still cover fully.
    const Expectation expectations[] = {
            {20.0f, 0.5f, 1.0f, 1.0f},
            {40.0f, 0.0f, 0.5f, 1.0f},
            {60.0f, 0.0f, 0.0f, 0.5f},
    };

    for (const Expectation& e : expectations) {
        const QColor actual = spanColourAt(cal, heights, half, e.dy);
        const QColor expected = mixxx::blendPartialCoverage(cal, e.low, e.mid, e.high);
        EXPECT_EQ(expected.rgb(), actual.rgb())
                << "at dy " << e.dy << ", coverage " << e.low << " " << e.mid
                << " " << e.high;
    }
}

// Two heights closer together than the strip width must not produce strips that
// overlap, invert, or have negative height. The spans have to stay a contiguous
// walk outwards from the centre whatever the input.
TEST(Rekordbox3BandAntialiasTest, CrowdedHeightsStayOrdered) {
    mixxx::Rekordbox3BandCalibration cal;
    cal.antialiasWidthPx = 4.0f; // far wider than the gaps below, on purpose

    const float half = 100.0f;
    uint32_t state = 999u;
    const auto nextHeight = [&state]() {
        state = state * 1103515245u + 12345u;
        // 0 to 3 pixels, so the three boundaries crowd inside one strip width
        return static_cast<float>((state >> 16) & 0xFFFu) * 3.0f / 4095.0f;
    };

    for (int trial = 0; trial < 20000; ++trial) {
        float heights[3] = {nextHeight(), nextHeight(), nextHeight()};
        switch (trial % 5) {
        case 1:
            heights[1] = heights[0]; // two exactly equal
            break;
        case 2:
            heights[0] = heights[1] = heights[2]; // all three exactly equal
            break;
        case 3:
            heights[0] = heights[1] = heights[2] = half; // the clamp ceiling
            break;
        case 4:
            heights[0] = 0.0f; // one silent
            break;
        default:
            break;
        }

        mixxx::Rekordbox3BandSpan spans[mixxx::kRekordbox3BandMaxSpans];
        const int count = mixxx::rekordbox3BandSpans(
                cal, heights[0], heights[1], heights[2], half, spans);
        ASSERT_GE(count, 0);
        ASSERT_LE(count, mixxx::kRekordbox3BandMaxSpans);

        float previousOuter = 0.0f;
        for (int i = 0; i < count; ++i) {
            ASSERT_GE(spans[i].outer, spans[i].inner)
                    << "span " << i << " of trial " << trial << " is inverted";
            ASSERT_FLOAT_EQ(previousOuter, spans[i].inner)
                    << "span " << i << " of trial " << trial << " is not contiguous";
            ASSERT_LE(spans[i].outer, half)
                    << "span " << i << " of trial " << trial << " leaves the pane";
            previousOuter = spans[i].outer;
        }
        if (count > 0) {
            ASSERT_FLOAT_EQ(0.0f, spans[0].inner) << "trial " << trial;
        }
    }
}

// Softening an edge must not bleed a band that is not there into view. A band
// at zero height draws nothing today and has to keep drawing nothing, or a
// silent high band would fringe every transient with white.
TEST(Rekordbox3BandAntialiasTest, SilentBandStaysInvisible) {
    const mixxx::Rekordbox3BandCalibration cal;
    ASSERT_GT(cal.antialiasWidthPx, 0.0f);

    const float half = 100.0f; // even pane, so no pixel centre sits at dy == 0
    for (int silent = 0; silent < 3; ++silent) {
        float heights[3] = {30.0f, 50.0f, 70.0f};
        heights[silent] = 0.0f;

        mixxx::Rekordbox3BandSpan spans[mixxx::kRekordbox3BandMaxSpans];
        const int count = mixxx::rekordbox3BandSpans(
                cal, heights[0], heights[1], heights[2], half, spans);

        // No span that draws anything may carry a colour that the silent band
        // is part of. Checked against the table rather than against a hue, so
        // it stays true if the palette is recalibrated.
        for (int i = 0; i < count; ++i) {
            if (spans[i].outer <= spans[i].inner) {
                continue; // zero width, draws nothing
            }
            for (int other = 0; other < 8; ++other) {
                const bool low = (other & 4) != 0;
                const bool mid = (other & 2) != 0;
                const bool high = (other & 1) != 0;
                const bool covers[3] = {low, mid, high};
                if (!covers[silent]) {
                    continue;
                }
                const QColor forbidden = mixxx::colourForCoverage(cal, low, mid, high);
                EXPECT_NE(forbidden.rgb(), spans[i].innerColour.rgb())
                        << "silent band " << silent << " appears at span " << i;
                EXPECT_NE(forbidden.rgb(), spans[i].outerColour.rgb())
                        << "silent band " << silent << " appears at span " << i;
            }
        }

        // The column also has to end where the bands that are left end, rather
        // than being extended outwards by a strip the silent band contributed.
        float liveMaximum = 0.0f;
        for (int band = 0; band < 3; ++band) {
            liveMaximum = std::max(liveMaximum, heights[band]);
        }
        ASSERT_GT(count, 0);
        EXPECT_FLOAT_EQ(liveMaximum + cal.antialiasWidthPx / 2.0f, spans[count - 1].outer);

        // And the innermost pixel shows the two live bands together. Sampled at
        // 0.5 because that is where the first pixel centre of an even pane is;
        // distance zero itself is the degenerate point the flat rasterizer
        // resolved in favour of every band including the silent one, and
        // ZeroWidthReproducesTheFlatRasterizer is what pins that down.
        const bool live[3] = {silent != 0, silent != 1, silent != 2};
        EXPECT_EQ(mixxx::colourForCoverage(cal, live[0], live[1], live[2]).rgb(),
                spanColourAt(cal, heights, half, 0.5f).rgb());
    }
}

// ---------------------------------------------------------------------------
// Keeping the C++ and the Python halves of the harness from drifting
// ---------------------------------------------------------------------------

namespace {

/// The appliance repository's copy of the calibration, which
/// `toolchain/waveform/render-3band.py` reads. The engine repository is nested
/// inside the appliance one, so this walks up from the test directory rather
/// than hardcoding a path: the test binary's idea of the source tree comes from
/// computeResourcePath(), which differs between build layouts.
QString findCalibrationJson(const QDir& testDir) {
    QDir dir = testDir;
    for (int level = 0; level < 8; ++level) {
        const QString candidate = dir.filePath(QStringLiteral(
                "toolchain/waveform/rekordbox3band-calibration.json"));
        if (QFile::exists(candidate)) {
            return candidate;
        }
        if (!dir.cdUp()) {
            break;
        }
    }
    return QString();
}

void expectJsonColour(const QJsonObject& json, const QString& key, const QColor& expected) {
    ASSERT_TRUE(json.contains(key)) << "the JSON has no key " << key.toStdString();
    const QColor actual = QColor::fromString(json.value(key).toString());
    ASSERT_TRUE(actual.isValid())
            << key.toStdString() << " is not a colour: "
            << json.value(key).toString().toStdString();
    EXPECT_EQ(expected.rgb(), actual.rgb()) << key.toStdString();
}

void expectJsonFloat(const QJsonObject& json, const QString& key, float expected) {
    ASSERT_TRUE(json.contains(key)) << "the JSON has no key " << key.toStdString();
    ASSERT_TRUE(json.value(key).isDouble()) << key.toStdString() << " is not a number";
    EXPECT_FLOAT_EQ(expected, static_cast<float>(json.value(key).toDouble()))
            << key.toStdString();
}

} // namespace

// C++ is the authority. The JSON exists so the Python reference rasterizer can
// use the same numbers, and this test is what stops the two from drifting: a
// change to Rekordbox3BandCalibration that is not mirrored into the JSON fails
// here rather than quietly making the harness compare two different pictures.
TEST_F(Rekordbox3BandTest, CalibrationJsonMatchesTheCppDefaults) {
    const QString path = findCalibrationJson(getTestDir());
    if (path.isEmpty()) {
        GTEST_SKIP() << "toolchain/waveform/rekordbox3band-calibration.json was "
                        "not found above "
                     << getTestDir().absolutePath().toStdString()
                     << ". It lives in the appliance repository, which contains "
                        "this one; run the tests from a checkout that has both.";
    }

    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly)) << path.toStdString();
    const QByteArray raw = file.readAll();
    file.close();

    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(raw, &error);
    ASSERT_EQ(QJsonParseError::NoError, error.error)
            << path.toStdString() << ": " << error.errorString().toStdString();
    ASSERT_TRUE(document.isObject()) << path.toStdString() << " is not a JSON object";
    const QJsonObject json = document.object();

    const mixxx::Rekordbox3BandCalibration cal;

    expectJsonColour(json, QStringLiteral("low"), cal.low);
    expectJsonColour(json, QStringLiteral("mid"), cal.mid);
    expectJsonColour(json, QStringLiteral("high"), cal.high);
    expectJsonColour(json, QStringLiteral("background"), cal.background);
    expectJsonColour(json, QStringLiteral("lowMid"), cal.lowMid);
    expectJsonColour(json, QStringLiteral("midHigh"), cal.midHigh);
    expectJsonColour(json, QStringLiteral("lowHigh"), cal.lowHigh);
    expectJsonColour(json, QStringLiteral("lowMidHigh"), cal.lowMidHigh);

    expectJsonFloat(json, QStringLiteral("highAlpha"), cal.highAlpha);
    expectJsonFloat(json, QStringLiteral("midAlpha"), cal.midAlpha);
    expectJsonFloat(json, QStringLiteral("lowBleed"), cal.lowBleed);
    expectJsonFloat(json, QStringLiteral("gamma"), cal.gamma);
    expectJsonFloat(json, QStringLiteral("lowHeightScale"), cal.lowHeightScale);
    expectJsonFloat(json, QStringLiteral("midHeightScale"), cal.midHeightScale);
    expectJsonFloat(json, QStringLiteral("highHeightScale"), cal.highHeightScale);
    expectJsonFloat(json, QStringLiteral("pwv7FullScale"), cal.pwv7FullScale);
    expectJsonFloat(json, QStringLiteral("pwv6FullScale"), cal.pwv6FullScale);
    expectJsonFloat(json, QStringLiteral("minVisibleHeightPx"), cal.minVisibleHeightPx);
    expectJsonFloat(json, QStringLiteral("antialiasWidthPx"), cal.antialiasWidthPx);
    expectJsonFloat(json, QStringLiteral("opacity"), cal.opacity);

    ASSERT_TRUE(json.contains(QStringLiteral("columnRule")));
    const QString rule = json.value(QStringLiteral("columnRule")).toString();
    mixxx::Rekordbox3BandColumnRule parsedRule = mixxx::Rekordbox3BandColumnRule::Nearest;
    if (rule == QLatin1String("Nearest")) {
        parsedRule = mixxx::Rekordbox3BandColumnRule::Nearest;
    } else if (rule == QLatin1String("MaxOverRange")) {
        parsedRule = mixxx::Rekordbox3BandColumnRule::MaxOverRange;
    } else if (rule == QLatin1String("MeanOverRange")) {
        parsedRule = mixxx::Rekordbox3BandColumnRule::MeanOverRange;
    } else {
        FAIL() << "columnRule is " << rule.toStdString()
               << ", which is not one of Nearest, MaxOverRange, MeanOverRange";
    }
    EXPECT_EQ(cal.columnRule, parsedRule);

    // Drift in the other direction: a key added to the JSON that nothing here
    // checks would be an untested number the Python half reads.
    const QStringList known{
            QStringLiteral("low"),
            QStringLiteral("mid"),
            QStringLiteral("high"),
            QStringLiteral("background"),
            QStringLiteral("lowMid"),
            QStringLiteral("midHigh"),
            QStringLiteral("lowHigh"),
            QStringLiteral("lowMidHigh"),
            QStringLiteral("highAlpha"),
            QStringLiteral("midAlpha"),
            QStringLiteral("lowBleed"),
            QStringLiteral("gamma"),
            QStringLiteral("lowHeightScale"),
            QStringLiteral("midHeightScale"),
            QStringLiteral("highHeightScale"),
            QStringLiteral("pwv7FullScale"),
            QStringLiteral("pwv6FullScale"),
            QStringLiteral("minVisibleHeightPx"),
            QStringLiteral("antialiasWidthPx"),
            QStringLiteral("opacity"),
            QStringLiteral("columnRule"),
    };
    const QStringList keys = json.keys();
    for (const QString& key : keys) {
        if (key.startsWith(QLatin1Char('_'))) {
            // Reserved for comments.
            continue;
        }
        EXPECT_TRUE(known.contains(key))
                << "the JSON has key " << key.toStdString()
                << " which no C++ field is checked against; add it here and to "
                   "Rekordbox3BandCalibration, or drop it";
    }
}

// ---------------------------------------------------------------------------
// The reference comparison harness
// ---------------------------------------------------------------------------

namespace {

/// Column reduction, the same rule the GPU renderer applies. Its own copy lives
/// in an anonymous namespace inside waveformrendererrekordbox3band.cpp and is
/// not reachable from here; if one changes, change both.
mixxx::BandSample reduceColumn(const QVector<mixxx::BandSample>& samples,
        const mixxx::Rekordbox3BandColumnRange& range,
        mixxx::Rekordbox3BandColumnRule rule) {
    switch (rule) {
    case mixxx::Rekordbox3BandColumnRule::Nearest:
        return samples[range.begin + (range.end - range.begin - 1) / 2];
    case mixxx::Rekordbox3BandColumnRule::MeanOverRange: {
        unsigned int low = 0;
        unsigned int mid = 0;
        unsigned int high = 0;
        for (int i = range.begin; i < range.end; ++i) {
            low += samples[i].low;
            mid += samples[i].mid;
            high += samples[i].high;
        }
        const unsigned int count = static_cast<unsigned int>(range.end - range.begin);
        return {static_cast<uint8_t>(low / count),
                static_cast<uint8_t>(mid / count),
                static_cast<uint8_t>(high / count)};
    }
    case mixxx::Rekordbox3BandColumnRule::MaxOverRange:
    default: {
        mixxx::BandSample result{0, 0, 0};
        for (int i = range.begin; i < range.end; ++i) {
            result.low = std::max(result.low, samples[i].low);
            result.mid = std::max(result.mid, samples[i].mid);
            result.high = std::max(result.high, samples[i].high);
        }
        return result;
    }
    }
}

/// A deterministic CPU rasterization of one window of a band series.
///
/// It draws the same picture the GPU renderer draws, by construction: the same
/// columnRangeForPixel(), the same scaleHeight(), and above all the same
/// rekordbox3BandSpans(), so the antialiased boundaries land in the same places
/// with the same colours. A pixel takes the colour its centre falls on. Only
/// that way is a comparison against a real capture a statement about the
/// renderer rather than about a second implementation.
QImage rasterize(const QVector<mixxx::BandSample>& samples,
        const mixxx::Rekordbox3BandCalibration& cal,
        float fullScale,
        int width,
        int height,
        double start,
        double span) {
    QImage image(width, height, QImage::Format_RGB32);
    image.fill(cal.background);

    const int entryCount = static_cast<int>(samples.size());
    const float half = static_cast<float>(height) / 2.0f;
    const float scales[3] = {cal.lowHeightScale, cal.midHeightScale, cal.highHeightScale};

    for (int x = 0; x < width; ++x) {
        const mixxx::Rekordbox3BandColumnRange range = mixxx::columnRangeForPixel(
                x, width, start, start + span, entryCount);
        if (range.end <= range.begin) {
            continue;
        }
        const mixxx::BandSample sample = reduceColumn(samples, range, cal.columnRule);
        const uint8_t values[3] = {sample.low, sample.mid, sample.high};

        float heights[3] = {0.0f, 0.0f, 0.0f};
        for (int band = 0; band < 3; ++band) {
            heights[band] = mixxx::scaleHeight(values[band],
                    fullScale,
                    cal.gamma,
                    scales[band],
                    half,
                    cal.minVisibleHeightPx);
        }

        mixxx::Rekordbox3BandSpan spans[mixxx::kRekordbox3BandMaxSpans];
        const int spanCount = mixxx::rekordbox3BandSpans(
                cal, heights[0], heights[1], heights[2], half, spans);

        for (int y = 0; y < height; ++y) {
            const float dy = std::fabs(static_cast<float>(y) + 0.5f - half);
            const QColor colour =
                    mixxx::rekordbox3BandColourAt(cal, spans, spanCount, dy);
            image.setPixel(x, y, colour.rgb());
        }
    }
    return image;
}

int envInt(const char* name, int fallback) {
    const QByteArray raw = qgetenv(name);
    if (raw.isEmpty()) {
        return fallback;
    }
    bool ok = false;
    const int value = QString::fromLocal8Bit(raw).toInt(&ok);
    return ok ? value : fallback;
}

double envDouble(const char* name, double fallback) {
    const QByteArray raw = qgetenv(name);
    if (raw.isEmpty()) {
        return fallback;
    }
    bool ok = false;
    const double value = QString::fromLocal8Bit(raw).toDouble(&ok);
    return ok ? value : fallback;
}

} // namespace

class Rekordbox3BandRenderTest : public MixxxTest {};

// Compares the CPU reference rasterization of a real .2EX file against a real
// rekordbox or CDJ-3000 screen capture.
//
// Both inputs come from the environment and neither is committed: a .2EX file
// and rekordbox's analysis of it are copyrighted, and a reference this code
// produced itself would measure nothing at all. With either variable unset the
// test skips, so the normal suite stays green with no fixtures present.
//
//   BITEDJ_2EX      a rekordbox ANLZ0000.2EX file
//   BITEDJ_REF_PNG  a capture of the same window of the same track
//
// The window has to be the same window, which is what these override:
//
//   BITEDJ_3BAND_TAG        PWV7 (default) or PWV6
//   BITEDJ_3BAND_WIDTH      default 1100
//   BITEDJ_3BAND_HEIGHT     default 200
//   BITEDJ_3BAND_START      default 0.30, normalized position of the left edge
//   BITEDJ_3BAND_SPAN       default 0.02, fraction of the track across the pane
//   BITEDJ_3BAND_MAX_DIFF   differing-pixel percentage to fail above; unset
//                           means report only, because no threshold has been
//                           agreed against a real capture yet.
TEST_F(Rekordbox3BandRenderTest, MatchesReferenceCapture) {
    const QString anlzPath = QString::fromLocal8Bit(qgetenv("BITEDJ_2EX"));
    const QString referencePath = QString::fromLocal8Bit(qgetenv("BITEDJ_REF_PNG"));
    if (anlzPath.isEmpty() || referencePath.isEmpty()) {
        GTEST_SKIP() << "set BITEDJ_2EX to a rekordbox ANLZ0000.2EX file and "
                        "BITEDJ_REF_PNG to a real rekordbox or CDJ-3000 capture "
                        "of the same window to run this comparison";
    }

    const auto pBands = mixxx::Rekordbox3BandWaveform::fromAnlz2Ex(anlzPath);
    ASSERT_FALSE(pBands.isNull()) << "cannot parse " << anlzPath.toStdString();

    const bool usePreview =
            QString::fromLocal8Bit(qgetenv("BITEDJ_3BAND_TAG")).compare(
                    QLatin1String("PWV6"), Qt::CaseInsensitive) == 0;
    const QVector<mixxx::BandSample>& samples =
            usePreview ? pBands->preview() : pBands->detail();
    ASSERT_FALSE(samples.isEmpty())
            << anlzPath.toStdString() << " holds no "
            << (usePreview ? "PWV6" : "PWV7") << " series";

    const mixxx::Rekordbox3BandCalibration cal;
    float fullScale = cal.pwv7FullScale;
    if (usePreview) {
        // PWV6 is stored on a smaller scale, so a zero pwv6FullScale means
        // normalise to this track's own maximum.
        fullScale = cal.pwv6FullScale;
        if (fullScale <= 0.0f) {
            uint8_t peak = 0;
            for (const mixxx::BandSample& entry : samples) {
                peak = std::max(peak, std::max(entry.low, std::max(entry.mid, entry.high)));
            }
            fullScale = peak > 0 ? static_cast<float>(peak) : 1.0f;
        }
    }

    const int width = envInt("BITEDJ_3BAND_WIDTH", 1100);
    const int height = envInt("BITEDJ_3BAND_HEIGHT", 200);
    const double start = envDouble("BITEDJ_3BAND_START", 0.30);
    const double span = envDouble("BITEDJ_3BAND_SPAN", 0.02);
    ASSERT_GT(width, 0);
    ASSERT_GT(height, 0);
    ASSERT_GT(span, 0.0);

    QImage reference;
    ASSERT_TRUE(reference.load(referencePath))
            << "cannot read the reference " << referencePath.toStdString();
    reference = reference.convertToFormat(QImage::Format_RGB32);
    ASSERT_EQ(width, reference.width())
            << "the reference is " << reference.width() << "x" << reference.height()
            << "; set BITEDJ_3BAND_WIDTH and BITEDJ_3BAND_HEIGHT to match it";
    ASSERT_EQ(height, reference.height())
            << "the reference is " << reference.width() << "x" << reference.height()
            << "; set BITEDJ_3BAND_WIDTH and BITEDJ_3BAND_HEIGHT to match it";

    const QImage rendered = rasterize(samples, cal, fullScale, width, height, start, span);

    QImage difference(width, height, QImage::Format_RGB32);
    qint64 differingPixels = 0;
    qint64 absoluteErrorSum = 0;
    int maximumError = 0;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const QRgb a = rendered.pixel(x, y);
            const QRgb b = reference.pixel(x, y);
            const int dr = std::abs(qRed(a) - qRed(b));
            const int dg = std::abs(qGreen(a) - qGreen(b));
            const int db = std::abs(qBlue(a) - qBlue(b));
            if (dr != 0 || dg != 0 || db != 0) {
                differingPixels++;
            }
            absoluteErrorSum += dr + dg + db;
            maximumError = std::max(maximumError, std::max(dr, std::max(dg, db)));
            difference.setPixel(x, y, qRgb(dr, dg, db));
        }
    }

    const double pixelCount = static_cast<double>(width) * height;
    const double differingPercent = 100.0 * differingPixels / pixelCount;
    const double meanAbsoluteError = absoluteErrorSum / (pixelCount * 3.0);

    const QFileInfo referenceInfo(referencePath);
    const QString diffPath = referenceInfo.dir().filePath(
            referenceInfo.completeBaseName() + QStringLiteral("-diff.png"));
    const bool wroteDiff = difference.save(diffPath, "PNG");

    std::cout << "3Band reference comparison\n"
              << "  .2EX                " << anlzPath.toStdString() << "\n"
              << "  tag                 " << (usePreview ? "PWV6" : "PWV7") << ", "
              << samples.size() << " entries\n"
              << "  window              " << width << "x" << height << " start "
              << start << " span " << span << " full scale " << fullScale << "\n"
              << "  reference           " << referencePath.toStdString() << "\n"
              << "  differing pixels    " << differingPercent << "%\n"
              << "  mean abs RGB error  " << meanAbsoluteError << "\n"
              << "  max RGB error       " << maximumError << "\n"
              << "  difference image    "
              << (wroteDiff ? diffPath.toStdString() : std::string("could not be written"))
              << std::endl;
    RecordProperty("differing_pixel_percent", QString::number(differingPercent).toStdString());
    RecordProperty("mean_abs_rgb_error", QString::number(meanAbsoluteError).toStdString());
    RecordProperty("max_rgb_error", maximumError);
    EXPECT_TRUE(wroteDiff) << "cannot write " << diffPath.toStdString();

    // Deliberately not a fixed threshold. No reference capture has been
    // measured against this renderer yet, so inventing a tolerance here would
    // be inventing a result. Set BITEDJ_3BAND_MAX_DIFF once one has.
    const QByteArray maxDiff = qgetenv("BITEDJ_3BAND_MAX_DIFF");
    if (!maxDiff.isEmpty()) {
        bool ok = false;
        const double threshold = QString::fromLocal8Bit(maxDiff).toDouble(&ok);
        ASSERT_TRUE(ok) << "BITEDJ_3BAND_MAX_DIFF is not a number: "
                        << maxDiff.toStdString();
        EXPECT_LE(differingPercent, threshold);
    }
}

} // namespace
