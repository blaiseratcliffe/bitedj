#include <gtest/gtest.h>

#include <QUrl>

#include "sources/audiosource.h"
#include "util/samplebuffer.h"

namespace mixxx {

namespace {

constexpr SINT kFrameLength = 8192;
constexpr audio::ChannelCount::value_t kChannelCount = 2;

/// An AudioSource whose reads stop short at a configurable frame. It stands in
/// both for a decoder on a USB drive that failed mid-transfer and for a
/// genuinely truncated file: from readSampleFrames()' point of view the two are
/// indistinguishable, which is exactly why it must not decide between them.
class ShortReadingAudioSource : public AudioSource {
  public:
    explicit ShortReadingAudioSource(SINT readableUntilFrame)
            : AudioSource(QUrl(QStringLiteral("file:///short-read-test.aiff"))),
              m_readableUntilFrame(readableUntilFrame) {
        initChannelCountOnce(audio::ChannelCount(kChannelCount));
        initSampleRateOnce(audio::SampleRate(44100));
        initFrameIndexRangeOnce(IndexRange::forward(0, kFrameLength));
    }

    void close() override {
    }

    void setReadableUntilFrame(SINT frame) {
        m_readableUntilFrame = frame;
    }

  protected:
    OpenResult tryOpen(OpenMode /*mode*/, const OpenParams& /*params*/) override {
        return OpenResult::Succeeded;
    }

    ReadableSampleFrames readSampleFramesClamped(
            const WritableSampleFrames& sampleFrames) override {
        const auto requested = sampleFrames.frameIndexRange();
        const auto readable = intersect(requested,
                IndexRange::forward(0, m_readableUntilFrame));
        // Only a prefix of the request can be satisfied; anything else is
        // reported as nothing read at all, like a decoder that cannot seek
        // past the damage.
        if (readable.empty() || readable.start() != requested.start()) {
            return ReadableSampleFrames(
                    IndexRange::forward(requested.start(), 0));
        }
        const SINT readableSamples =
                getSignalInfo().frames2samples(readable.length());
        return ReadableSampleFrames(readable,
                SampleBuffer::ReadableSlice(
                        sampleFrames.writableData(), readableSamples));
    }

  private:
    SINT m_readableUntilFrame;
};

class AudioSourceShortReadTest : public testing::Test {
  protected:
    AudioSourceShortReadTest()
            : m_buffer(kFrameLength * kChannelCount) {
    }

    ReadableSampleFrames read(AudioSource& source, IndexRange frameIndexRange) {
        return source.readSampleFrames(WritableSampleFrames(frameIndexRange,
                SampleBuffer::WritableSlice(m_buffer)));
    }

    SampleBuffer m_buffer;
};

// The regression: a partial read used to shrink the readable range on the spot,
// irreversibly, so a deck that hit one USB hiccup played silence to the end of
// the track.
TEST_F(AudioSourceShortReadTest, PartialReadDoesNotShrinkTheReadableRange) {
    ShortReadingAudioSource source(1010);

    const auto readable = read(source, IndexRange::forward(0, kFrameLength));

    EXPECT_EQ(IndexRange::forward(0, 1010), readable.frameIndexRange());
    EXPECT_EQ(IndexRange::forward(0, kFrameLength), source.frameIndexRange());
}

TEST_F(AudioSourceShortReadTest, EmptyReadDoesNotShrinkTheReadableRange) {
    ShortReadingAudioSource source(0);

    const auto readable = read(source, IndexRange::forward(0, kFrameLength));

    EXPECT_TRUE(readable.frameIndexRange().empty());
    EXPECT_EQ(IndexRange::forward(0, kFrameLength), source.frameIndexRange());
}

// A transient failure must leave nothing behind: once the storage is back, the
// very next read of the same range returns everything.
TEST_F(AudioSourceShortReadTest, ReadsRecoverInFullAfterAShortRead) {
    ShortReadingAudioSource source(1010);

    read(source, IndexRange::forward(0, kFrameLength));
    source.setReadableUntilFrame(kFrameLength);
    const auto readable = read(source, IndexRange::forward(0, kFrameLength));

    EXPECT_EQ(IndexRange::forward(0, kFrameLength), readable.frameIndexRange());
    EXPECT_EQ(IndexRange::forward(0, kFrameLength), source.frameIndexRange());
}

// Shrinking is still available, but only to a caller that has confirmed the
// shortfall is about the file's content. That is CachingReaderWorker, after
// re-opening the file reproduces the same stopping point.
TEST_F(AudioSourceShortReadTest, ConfirmedShortfallShrinksTheReadableRange) {
    ShortReadingAudioSource source(1010);

    EXPECT_TRUE(source.shrinkReadableFrameIndexRange(IndexRange::forward(0, 1010)));

    EXPECT_EQ(IndexRange::forward(0, 1010), source.frameIndexRange());
}

TEST_F(AudioSourceShortReadTest, ShrinkingToTheSameRangeIsANoOp) {
    ShortReadingAudioSource source(kFrameLength);

    EXPECT_FALSE(source.shrinkReadableFrameIndexRange(
            IndexRange::forward(0, kFrameLength)));

    EXPECT_EQ(IndexRange::forward(0, kFrameLength), source.frameIndexRange());
}

} // anonymous namespace

} // namespace mixxx
