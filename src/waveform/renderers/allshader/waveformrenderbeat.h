#pragma once

#include <QColor>

#include "shaders/unicolorshader.h"
#include "util/class.h"
#include "waveform/renderers/allshader/vertexdata.h"
#include "waveform/renderers/allshader/waveformrenderer.h"

class QDomNode;
class SkinContext;

namespace allshader {
class WaveformRenderBeat;
}

class allshader::WaveformRenderBeat final : public allshader::WaveformRenderer {
  public:
    explicit WaveformRenderBeat(WaveformWidgetRenderer* waveformWidget,
            ::WaveformRendererAbstract::PositionSource type =
                    ::WaveformRendererAbstract::Play);

    void setup(const QDomNode& node, const SkinContext& context) override;
    void paintGL() override;
    void initializeGL() override;

  private:
    mixxx::UnicolorShader m_shader;
    QColor m_color;
    QColor m_downbeatColor;
    // CDJ style tick geometry: each beat is a short mark at the top and bottom
    // edge rather than a line across the whole waveform. Length is a fraction
    // of the pane's breadth, clamped, so the short deck 3/4 cells and the tall
    // deck 1/2 cells stay comparable. See setup() for the skin tags.
    float m_beatTickFraction;
    float m_beatTickMinPx;
    float m_beatTickMaxPx;
    VertexData m_vertices;
    VertexData m_fadingVertices;
    VertexData m_downbeatVertices;
    VertexData m_fadingDownbeatVertices;

    bool m_isSlipRenderer;

    DISALLOW_COPY_AND_ASSIGN(WaveformRenderBeat);
};
