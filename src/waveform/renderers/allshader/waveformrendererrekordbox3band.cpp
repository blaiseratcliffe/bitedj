#include "waveform/renderers/allshader/waveformrendererrekordbox3band.h"

#include <algorithm>

#include "track/track.h"
#include "util/assert.h"
#include "waveform/rekordbox3bandwaveform.h"
#include "waveform/renderers/allshader/matrixforwidgetgeometry.h"
#include "waveform/renderers/waveformwidgetrenderer.h"

namespace allshader {

namespace {

mixxx::BandSample reduceColumn(const QVector<mixxx::BandSample>& samples,
        const mixxx::Rekordbox3BandColumnRange& range,
        mixxx::Rekordbox3BandColumnRule rule) {
    switch (rule) {
    case mixxx::Rekordbox3BandColumnRule::Nearest:
        return samples[range.begin + (range.end - range.begin - 1) / 2];
    case mixxx::Rekordbox3BandColumnRule::MeanOverRange: {
        unsigned int low = 0, mid = 0, high = 0;
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

} // namespace

WaveformRendererRekordbox3Band::WaveformRendererRekordbox3Band(
        WaveformWidgetRenderer* waveformWidget)
        : WaveformRendererSignalBase(waveformWidget) {
}

void WaveformRendererRekordbox3Band::onSetup(const QDomNode& node) {
    // Nothing to read: the calibration is fixed in code so that a skin cannot
    // repaint rekordbox's analysis in its own palette.
    Q_UNUSED(node);
}

void WaveformRendererRekordbox3Band::initializeGL() {
    WaveformRendererSignalBase::initializeGL();
    m_shader.init();
}

void WaveformRendererRekordbox3Band::paintGL() {
    TrackPointer pTrack = m_waveformRenderer->getTrackInfo();
    if (!pTrack) {
        return;
    }

    // Native data when the importer attached some, a Mixxx derived
    // approximation otherwise. Building that fallback is the resolver's job,
    // not ours.
    const mixxx::ConstRekordbox3BandWaveformPointer pBands =
            mixxx::resolveRekordbox3BandWaveform(pTrack);
    if (pBands.isNull() || !pBands->hasDetail()) {
        return;
    }

    const QVector<mixxx::BandSample>& detail = pBands->detail();
    const int entryCount = static_cast<int>(detail.size());
    if (entryCount < 2) {
        return;
    }

    const float devicePixelRatio = m_waveformRenderer->getDevicePixelRatio();
    const int length = static_cast<int>(m_waveformRenderer->getLength() * devicePixelRatio);
    if (length <= 0) {
        return;
    }

    const double firstPosition = m_waveformRenderer->getFirstDisplayedPosition();
    const double lastPosition = m_waveformRenderer->getLastDisplayedPosition();
    if (firstPosition == lastPosition) {
        // Nothing is on screen yet; a zero wide range would collapse every
        // column onto one entry.
        return;
    }

    // Only the master gain. The three band pointers are deliberately null: the
    // EQ knobs must not change the height of rekordbox's stored analysis, which
    // is a recording of the track rather than of the mixer.
    float allGain(1.0f);
    getGains(&allGain, false, nullptr, nullptr, nullptr);

    const float breadth = static_cast<float>(m_waveformRenderer->getBreadth()) * devicePixelRatio;
    const float halfBreadth = breadth / 2.0f;
    const float minVisibleHeight = m_calibration.minVisibleHeightPx * devicePixelRatio;
    const float opacity = m_calibration.opacity;

    // The bands are concentric about the centre line, not stacked, so a half
    // column is at most kRekordbox3BandMaxSpans runs and transition strips.
    // Each is mirrored into two rectangles except the innermost, which is drawn
    // as one spanning the centre.
    const int numVerticesPerRectangle = 6; // 2 triangles
    const int rectanglesPerColumn = 2 * mixxx::kRekordbox3BandMaxSpans - 1;
    const int reserved = numVerticesPerRectangle * rectanglesPerColumn * (length + 1);

    m_vertices.clear();
    m_vertices.reserve(reserved);
    m_colors.clear();
    m_colors.reserve(reserved);

    for (int pos = 0; pos < length; ++pos) {
        const mixxx::Rekordbox3BandColumnRange range = mixxx::columnRangeForPixel(
                pos, length, firstPosition, lastPosition, entryCount);
        const mixxx::BandSample sample =
                reduceColumn(detail, range, m_calibration.columnRule);

        const float heights[3] = {
                mixxx::scaleHeight(sample.low,
                        m_calibration.pwv7FullScale,
                        m_calibration.gamma,
                        m_calibration.lowHeightScale * allGain,
                        halfBreadth,
                        minVisibleHeight),
                mixxx::scaleHeight(sample.mid,
                        m_calibration.pwv7FullScale,
                        m_calibration.gamma,
                        m_calibration.midHeightScale * allGain,
                        halfBreadth,
                        minVisibleHeight),
                mixxx::scaleHeight(sample.high,
                        m_calibration.pwv7FullScale,
                        m_calibration.gamma,
                        m_calibration.highHeightScale * allGain,
                        halfBreadth,
                        minVisibleHeight)};

        // The span list is the picture. It is built by the GL-free helper that
        // the reference rasterizers also call, so the three implementations
        // cannot drift apart in what they think a column looks like.
        mixxx::Rekordbox3BandSpan spans[mixxx::kRekordbox3BandMaxSpans];
        const int spanCount = mixxx::rekordbox3BandSpans(m_calibration,
                heights[0],
                heights[1],
                heights[2],
                halfBreadth,
                spans);

        const float fpos = static_cast<float>(pos);

        for (int i = 0; i < spanCount; ++i) {
            const mixxx::Rekordbox3BandSpan& span = spans[i];
            if (span.outer <= span.inner) {
                // Silence, or two bands at the same height. Nothing to draw.
                continue;
            }

            const float innerR = static_cast<float>(span.innerColour.redF());
            const float innerG = static_cast<float>(span.innerColour.greenF());
            const float innerB = static_cast<float>(span.innerColour.blueF());

            if (!span.gradient && span.inner == 0.0f) {
                // The innermost flat run is one rectangle spanning the centre
                // rather than two meeting on it, so there is no seam down the
                // middle of a quiet passage.
                m_vertices.addRectangle(fpos,
                        halfBreadth - span.outer,
                        fpos + 1.0f,
                        halfBreadth + span.outer);
                m_colors.addForRectangle(innerR, innerG, innerB, opacity);
                continue;
            }

            const float outerR = static_cast<float>(span.outerColour.redF());
            const float outerG = static_cast<float>(span.outerColour.greenF());
            const float outerB = static_cast<float>(span.outerColour.blueF());

            // addForRectangleGradient puts colour A on the y1 edge of the
            // rectangle addRectangle just pushed and colour B on its y2 edge.
            // Above the centre line y1 is the far edge, below it y1 is the near
            // one, so the pair is mirrored. A flat span passes the same colour
            // twice, which pushes exactly what addForRectangle would.
            m_vertices.addRectangle(fpos,
                    halfBreadth - span.outer,
                    fpos + 1.0f,
                    halfBreadth - span.inner);
            m_colors.addForRectangleGradient(outerR,
                    outerG,
                    outerB,
                    opacity,
                    innerR,
                    innerG,
                    innerB,
                    opacity);
            m_vertices.addRectangle(fpos,
                    halfBreadth + span.inner,
                    fpos + 1.0f,
                    halfBreadth + span.outer);
            m_colors.addForRectangleGradient(innerR,
                    innerG,
                    innerB,
                    opacity,
                    outerR,
                    outerG,
                    outerB,
                    opacity);
        }
    }

    if (m_vertices.size() == 0) {
        return;
    }

    DEBUG_ASSERT(m_vertices.size() == m_colors.size());

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    const QMatrix4x4 matrix = matrixForWidgetGeometry(m_waveformRenderer, true);

    const int matrixLocation = m_shader.matrixLocation();
    const int positionLocation = m_shader.positionLocation();
    const int colorLocation = m_shader.colorLocation();

    m_shader.bind();
    m_shader.enableAttributeArray(positionLocation);
    m_shader.enableAttributeArray(colorLocation);

    m_shader.setUniformValue(matrixLocation, matrix);

    m_shader.setAttributeArray(
            positionLocation, GL_FLOAT, m_vertices.constData(), 2);
    m_shader.setAttributeArray(
            colorLocation, GL_FLOAT, m_colors.constData(), 4);

    glDrawArrays(GL_TRIANGLES, 0, m_vertices.size());

    m_shader.disableAttributeArray(positionLocation);
    m_shader.disableAttributeArray(colorLocation);
    m_shader.release();
}

} // namespace allshader
