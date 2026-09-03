#pragma once

#include "shaders/rgbashader.h"
#include "util/class.h"
#include "waveform/renderers/allshader/rekordbox3bandcalibration.h"
#include "waveform/renderers/allshader/rgbadata.h"
#include "waveform/renderers/allshader/vertexdata.h"
#include "waveform/renderers/allshader/waveformrenderersignalbase.h"

namespace allshader {
class WaveformRendererRekordbox3Band;
}

/// Draws the CDJ-3000's own three band analysis (PWV7) instead of Mixxx's.
///
/// Like the other renderers here this builds a CPU side triangle list and draws
/// it straight to the widget's framebuffer. It allocates no framebuffer object,
/// no offscreen surface and no texture, deliberately: the fixed oversampling
/// FBO that WaveformRendererTextured allocates comes back invalid on the Pi's
/// V3D GPU and paints the pane solid white (docs/M4-SKIN-NOTES.md section 14).
class allshader::WaveformRendererRekordbox3Band final
        : public allshader::WaveformRendererSignalBase {
  public:
    explicit WaveformRendererRekordbox3Band(WaveformWidgetRenderer* waveformWidget);

    // override ::WaveformRendererSignalBase
    void onSetup(const QDomNode& node) override;

    void initializeGL() override;
    void paintGL() override;

  private:
    mixxx::RGBAShader m_shader;
    VertexData m_vertices;
    RGBAData m_colors;

    mixxx::Rekordbox3BandCalibration m_calibration;

    DISALLOW_COPY_AND_ASSIGN(WaveformRendererRekordbox3Band);
};
