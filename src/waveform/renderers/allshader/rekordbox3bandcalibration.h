#pragma once

#include <QColor>
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace mixxx {

/// How the entries falling under one pixel column are reduced to one value.
///
/// `MaxOverRange` is the default and the only one that matches a CDJ zoomed
/// out: a transient that occupies a single entry stays a full height spike
/// instead of being averaged into the floor around it.
enum class Rekordbox3BandColumnRule {
    Nearest,
    MaxOverRange,
    MeanOverRange,
};

/// Everything the three band renderer needs that is not the sample data.
///
/// The colours live here rather than in the skin's `WaveformSignalColors` on
/// purpose. This renderer draws rekordbox's own analysis, and it has to look
/// like rekordbox drew it, so a skin swapping its RGB palette must not change
/// it. The eight coverage colours below come from the validated reference
/// rendering of real `.2EX` data; they are not free choices.
struct Rekordbox3BandCalibration {
    // Base colours, defined here and never read from the skin, so this
    // renderer cannot pick up the skin's RGB palette.
    QColor low{0x00, 0x55, 0xE1};
    QColor mid{0xFF, 0xA6, 0x00};
    QColor high{0xFF, 0xFF, 0xFF};
    QColor background{0x00, 0x00, 0x00};
    // Overlap colours. Measured targets, consistent with the painter model below.
    QColor lowMid{0xB4, 0x69, 0x0A};
    QColor midHigh{0xFF, 0xF0, 0xD7};
    QColor lowHigh{0xD2, 0xDC, 0xFA};
    QColor lowMidHigh{0xF5, 0xEB, 0xD7};
    // The painter's-algorithm parameters the measured palette was validated
    // against: mid over low is `midAlpha * mid + lowBleed * low`, and high
    // over whatever is underneath is `highAlpha * high + (1 - highAlpha) *
    // base`. Those two rules reproduce the four measured overlap colours
    // above to 1.77% RMS, which is the evidence that the eight colours are a
    // consistent set and not eight unrelated samples. They are kept for that
    // reason only. Nothing computes with them: `blendPartialCoverage()`
    // interpolates the measured colours themselves, because a model that is
    // 1.77% off would put a visible seam exactly where antialiasing is meant
    // to remove one.
    float highAlpha = 0.833f;
    float midAlpha = 0.682f;
    float lowBleed = 0.0285f;
    // Height scaling.
    //
    // The three band scales are equal on purpose: they are the documented
    // place to fold in a master gain (see scaleHeight below), and this
    // renderer wants one number rather than a per band balance. 0.55 is
    // headroom, judged on the panel against a CDJ-3000 capture. At 1.0 a
    // loud master reaches the pane edge and a drop draws as a slab with no
    // black in it; rekordbox leaves room above its peaks.
    //
    // This is not a substitute for the zoom. Section 20.1 of
    // docs/M4-SKIN-NOTES.md records that most of what reads as "too dense"
    // is entries per pixel, not height: at DefaultZoom 12 the full width
    // band showed 34.5s against the ten a CDJ gives, and MaxOverRange turns
    // that into a wall whatever this number says. Fix the zoom first.
    float gamma = 1.0f;
    float lowHeightScale = 0.55f;
    float midHeightScale = 0.55f;
    float highHeightScale = 0.55f;
    float pwv7FullScale = 127.0f;
    /// PWV6 is stored on a different, smaller scale than PWV7. Normalising it
    /// by 127 renders the overview as a thin line: a typical track's PWV6
    /// peaks around 39, and the largest value seen across 40 measured files
    /// was 91. So 0.0 means "normalise to this track's own PWV6 maximum",
    /// which fills the strip and is what looks correct; a positive value
    /// overrides that with a fixed scale, the way `pwv7FullScale` does.
    float pwv6FullScale = 0.0f;
    float minVisibleHeightPx = 1.0f;
    /// Width in pixels of the transition strip drawn across a band boundary.
    /// 1.0 is not a taste setting: at exactly one pixel the blend a boundary
    /// produces is the fraction of that pixel's height lying inside the band,
    /// so the softening is the pixel's own area coverage and no second row is
    /// touched. It matters here because PWV7 stores 150 columns per second
    /// against the 441 Mixxx analyses at, so 3Band has about a third of the
    /// temporal resolution of every other option and steps visibly at deep
    /// zoom. Oversampling cannot invent samples rekordbox never stored;
    /// softening the edge is the honest improvement. 0.0 disables it and
    /// restores hard edges exactly.
    float antialiasWidthPx = 1.0f;
    float opacity = 1.0f;
    Rekordbox3BandColumnRule columnRule = Rekordbox3BandColumnRule::MaxOverRange;
};

/// A half open range `[begin, end)` of entry indices. Empty only when the
/// series itself is.
struct Rekordbox3BandColumnRange {
    int begin;
    int end;
};

/// The entries that fall under pixel column `pixel` of `pixelCount`.
///
/// `firstPosition` and `lastPosition` are normalized [0,1] track positions, and
/// they are mapped with the same `normalized * (count - 1)` convention that
/// `Rekordbox3BandWaveform::detailIndexForPosition()` uses, so the two agree.
/// The returned range is clamped into the series and is always at least one
/// entry wide, so a zoomed in column still has something to reduce. The end
/// is the entry the right edge lands on, plus one, rather than the ceiling of
/// it: with a half open range and a ceiling, an edge landing exactly on an
/// index drops that entry, which silently loses the very last one whenever the
/// window reaches the end of the track. Adjacent columns therefore share a
/// single entry at exact boundaries, which a max reduction does not mind.
inline Rekordbox3BandColumnRange columnRangeForPixel(
        int pixel,
        int pixelCount,
        double firstPosition,
        double lastPosition,
        int entryCount) {
    if (entryCount <= 0 || pixelCount <= 0) {
        return {0, 0};
    }

    const double lastIndex = static_cast<double>(entryCount - 1);
    const double firstEntry = firstPosition * lastIndex;
    const double lastEntry = lastPosition * lastIndex;

    const double a = firstEntry +
            (lastEntry - firstEntry) * static_cast<double>(pixel) /
                    static_cast<double>(pixelCount);
    const double b = firstEntry +
            (lastEntry - firstEntry) * static_cast<double>(pixel + 1) /
                    static_cast<double>(pixelCount);

    int begin = static_cast<int>(std::floor(a));
    int end = std::max(static_cast<int>(std::floor(b)) + 1, begin + 1);

    begin = std::clamp(begin, 0, entryCount - 1);
    end = std::clamp(end, begin + 1, entryCount);

    return {begin, end};
}

/// One band's half height in pixels, measured from the centre line.
///
/// A zero sample stays zero and draws nothing, so silence is background rather
/// than a hairline. Any non zero sample is raised to `minVisibleHeightPx` so a
/// quiet passage stays legible, and the result is clamped to `halfHeightPx` so
/// a boosted `bandScale` cannot draw outside the pane. `bandScale` is the place
/// to fold in the master gain; the per band EQ gains deliberately do not belong
/// there, because this is a picture of the track, not of the mixer.
inline float scaleHeight(uint8_t value,
        float fullScale,
        float gamma,
        float bandScale,
        float halfHeightPx,
        float minVisibleHeightPx) {
    if (value == 0 || fullScale <= 0.0f || halfHeightPx <= 0.0f) {
        return 0.0f;
    }

    float normalized = std::clamp(static_cast<float>(value) / fullScale, 0.0f, 1.0f);
    if (gamma != 1.0f) {
        normalized = std::pow(normalized, gamma);
    }

    const float height = normalized * bandScale * halfHeightPx;
    if (height <= 0.0f) {
        return 0.0f;
    }

    return std::clamp(height, std::min(minVisibleHeightPx, halfHeightPx), halfHeightPx);
}

/// The seven entry coverage table. `background` for no coverage at all, which
/// the renderer does not draw.
inline QColor colourForCoverage(const Rekordbox3BandCalibration& calibration,
        bool low,
        bool mid,
        bool high) {
    if (low && mid && high) {
        return calibration.lowMidHigh;
    }
    if (low && mid) {
        return calibration.lowMid;
    }
    if (mid && high) {
        return calibration.midHigh;
    }
    if (low && high) {
        return calibration.lowHigh;
    }
    if (low) {
        return calibration.low;
    }
    if (mid) {
        return calibration.mid;
    }
    if (high) {
        return calibration.high;
    }
    return calibration.background;
}

namespace rekordbox3band {

struct Rgb {
    float r;
    float g;
    float b;
};

inline Rgb toRgb(const QColor& colour) {
    return {static_cast<float>(colour.redF()),
            static_cast<float>(colour.greenF()),
            static_cast<float>(colour.blueF())};
}

} // namespace rekordbox3band

/// The colour of a pixel the bands cover only partly.
///
/// Trilinear interpolation of the eight `colourForCoverage()` colours over the
/// unit cube of coverages. At every corner exactly one of the eight weights is
/// 1 and the rest are 0, so a whole coverage returns that entry of the table
/// bit for bit and an antialiased edge meets the flat interior it borders with
/// no seam. In between it is smooth, and it needs no special casing.
///
/// The bands are concentric, so in practice only one band's boundary falls
/// inside a given pixel and the other two coverages are 0 or 1; the cube's
/// interior is there for generality. Note that this is deliberately not the
/// painter's-algorithm model that `highAlpha`, `midAlpha` and `lowBleed`
/// describe: that model agrees with the measured overlaps only to 1.77% RMS,
/// which is 11 levels of 255 at worst and would show as an edge that does not
/// match its own interior.
///
/// The renderer draws whole coverage only and calls `colourForCoverage()`
/// directly, so this function is for a CPU reference rasterizer.
inline QColor blendPartialCoverage(const Rekordbox3BandCalibration& calibration,
        float lowCoverage,
        float midCoverage,
        float highCoverage) {
    lowCoverage = std::clamp(lowCoverage, 0.0f, 1.0f);
    midCoverage = std::clamp(midCoverage, 0.0f, 1.0f);
    highCoverage = std::clamp(highCoverage, 0.0f, 1.0f);

    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;

    for (int low = 0; low < 2; ++low) {
        const float lowWeight = low ? lowCoverage : 1.0f - lowCoverage;
        for (int mid = 0; mid < 2; ++mid) {
            const float midWeight = mid ? midCoverage : 1.0f - midCoverage;
            for (int high = 0; high < 2; ++high) {
                const float highWeight = high ? highCoverage : 1.0f - highCoverage;
                const float weight = lowWeight * midWeight * highWeight;
                if (weight == 0.0f) {
                    // Skipped so that a corner is a single assignment of the
                    // table entry rather than a sum of eight terms.
                    continue;
                }
                const rekordbox3band::Rgb corner = rekordbox3band::toRgb(
                        colourForCoverage(calibration, low != 0, mid != 0, high != 0));
                r += weight * corner.r;
                g += weight * corner.g;
                b += weight * corner.b;
            }
        }
    }

    return QColor::fromRgbF(std::clamp(r, 0.0f, 1.0f),
            std::clamp(g, 0.0f, 1.0f),
            std::clamp(b, 0.0f, 1.0f));
}

/// Linear interpolation between two of the table colours, in 8-bit channels.
///
/// Deliberately integer and deliberately explicit about its rounding. The
/// Python reference rasterizer has to reproduce this bit for bit, and 8-bit
/// channels with a written-out round leave nothing to a language's floating
/// point conversion rules. Exact at both endpoints, so a truncated gradient
/// still starts and ends on the colour the table gives.
inline QColor lerpColour(const QColor& a, const QColor& b, float t) {
    if (t <= 0.0f) {
        return a;
    }
    if (t >= 1.0f) {
        return b;
    }
    const auto channel = [t](int from, int to) {
        return static_cast<int>(std::floor((1.0f - t) * static_cast<float>(from) +
                t * static_cast<float>(to) + 0.5f));
    };
    return QColor(channel(a.red(), b.red()),
            channel(a.green(), b.green()),
            channel(a.blue(), b.blue()));
}

/// One band of one half column, as a distance range out from the centre line.
///
/// A flat span carries one colour. A gradient span is a boundary's transition
/// strip and ramps linearly from `innerColour` at `inner` to `outerColour` at
/// `outer`, which is what a GPU gradient quad draws and what
/// `blendPartialCoverage()` computes, because that function is trilinear and
/// only one band's coverage varies across a strip.
struct Rekordbox3BandSpan {
    float inner;
    float outer;
    QColor innerColour;
    QColor outerColour;
    bool gradient;
};

/// Three transition strips and the three flat runs between them.
constexpr int kRekordbox3BandMaxSpans = 6;

/// The spans of one half column, ordered outwards from the centre line.
///
/// This is the single definition of the picture: the GPU renderer turns each
/// span into a rectangle or a gradient quad, and the reference rasterizers
/// evaluate it per pixel through `rekordbox3BandColourAt()`. Writes at most
/// `kRekordbox3BandMaxSpans` entries and returns how many. Spans are
/// contiguous, start at 0, never overlap and never run backwards; beyond the
/// last one is background.
///
/// The rules that are not obvious:
///
/// - `antialiasWidthPx <= 0` produces no strips at all, so the result is the
///   flat runs the renderer drew before antialiasing existed, unchanged.
/// - Bands at the same height share one boundary, so a column sitting on the
///   `minVisibleHeightPx` floor or clamped to the `halfHeightPx` ceiling fades
///   straight from its own colour to the background instead of through
///   combinations that are not on screen.
/// - A strip never passes the midpoint to a neighbouring boundary, so two of
///   them cannot overlap or invert however close two heights are. Truncating
///   one does not re-slope it: the endpoint colours are the untruncated ramp
///   sampled where it was cut, which keeps the colour at a given distance from
///   depending on how near the next band happens to be.
/// - A silent band gets no strip, so softening an edge can never bleed a band
///   that is not there into view.
inline int rekordbox3BandSpans(const Rekordbox3BandCalibration& calibration,
        float lowHeightPx,
        float midHeightPx,
        float highHeightPx,
        float halfHeightPx,
        Rekordbox3BandSpan* pSpans) {
    struct Boundary {
        float height;
        int band;
    };
    Boundary sorted[3] = {{std::max(lowHeightPx, 0.0f), 0},
            {std::max(midHeightPx, 0.0f), 1},
            {std::max(highHeightPx, 0.0f), 2}};
    // Insertion sort. Stable, so equal heights keep low, mid, high order and
    // the colours below do not depend on which sort a standard library uses.
    for (int i = 1; i < 3; ++i) {
        const Boundary key = sorted[i];
        int j = i - 1;
        while (j >= 0 && sorted[j].height > key.height) {
            sorted[j + 1] = sorted[j];
            --j;
        }
        sorted[j + 1] = key;
    }

    // The colour just inside boundary k is the coverage of every band that
    // reaches at least that far, which after sorting is sorted[k..2]. Outside
    // the outermost boundary there is only the background.
    QColor colours[4];
    for (int k = 0; k < 3; ++k) {
        bool covers[3] = {false, false, false};
        for (int j = k; j < 3; ++j) {
            covers[sorted[j].band] = true;
        }
        colours[k] = colourForCoverage(calibration, covers[0], covers[1], covers[2]);
    }
    colours[3] = calibration.background;

    const float halfWidth = std::max(calibration.antialiasWidthPx, 0.0f) * 0.5f;

    int count = 0;
    float cursor = 0.0f;
    int k = 0;
    while (k < 3) {
        // Bands at exactly this height share this boundary.
        int last = k;
        while (last + 1 < 3 && sorted[last + 1].height == sorted[k].height) {
            ++last;
        }

        const float height = sorted[k].height;
        const float bandHalfWidth = height > 0.0f ? halfWidth : 0.0f;

        float lo = height - bandHalfWidth;
        float hi = height + bandHalfWidth;
        if (k > 0) {
            lo = std::max(lo, (sorted[k - 1].height + height) * 0.5f);
        }
        if (last + 1 < 3) {
            hi = std::min(hi, (height + sorted[last + 1].height) * 0.5f);
        }
        lo = std::max(lo, cursor);
        hi = std::min(hi, halfHeightPx);
        hi = std::max(hi, lo);

        // The flat run from wherever the last span ended up to this strip. The
        // `count == 0` case keeps the degenerate span at the centre that a
        // silent innermost band leaves behind, so a pixel centred exactly on
        // the centre line still resolves the way it did before antialiasing.
        if (lo > cursor || count == 0) {
            pSpans[count].inner = cursor;
            pSpans[count].outer = lo;
            pSpans[count].innerColour = colours[k];
            pSpans[count].outerColour = colours[k];
            pSpans[count].gradient = false;
            ++count;
            cursor = lo;
        }

        if (hi > lo) {
            const float rampStart = height - bandHalfWidth;
            const float rampWidth = bandHalfWidth * 2.0f;
            pSpans[count].inner = lo;
            pSpans[count].outer = hi;
            pSpans[count].innerColour = lerpColour(
                    colours[k], colours[last + 1], (lo - rampStart) / rampWidth);
            pSpans[count].outerColour = lerpColour(
                    colours[k], colours[last + 1], (hi - rampStart) / rampWidth);
            pSpans[count].gradient = true;
            ++count;
            cursor = hi;
        }

        k = last + 1;
    }
    return count;
}

/// The colour at distance `dy` from the centre line, background beyond the
/// last span.
///
/// The first span whose outer edge reaches `dy` wins, which is the rule that
/// makes a pixel centred exactly on a boundary take the colour from inside it,
/// the same way the flat rectangles resolved that pixel before.
inline QColor rekordbox3BandColourAt(const Rekordbox3BandCalibration& calibration,
        const Rekordbox3BandSpan* pSpans,
        int spanCount,
        float dy) {
    for (int i = 0; i < spanCount; ++i) {
        const Rekordbox3BandSpan& span = pSpans[i];
        if (dy > span.outer) {
            continue;
        }
        if (!span.gradient || span.outer <= span.inner) {
            return span.innerColour;
        }
        const float t = (dy - span.inner) / (span.outer - span.inner);
        return lerpColour(span.innerColour, span.outerColour, t);
    }
    return calibration.background;
}

} // namespace mixxx
