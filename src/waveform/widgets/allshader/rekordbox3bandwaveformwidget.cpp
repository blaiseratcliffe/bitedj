#include "waveform/widgets/allshader/rekordbox3bandwaveformwidget.h"

#include "waveform/renderers/allshader/waveformrenderbackground.h"
#include "waveform/renderers/allshader/waveformrenderbeat.h"
#include "waveform/renderers/allshader/waveformrendererendoftrack.h"
#include "waveform/renderers/allshader/waveformrendererpreroll.h"
#include "waveform/renderers/allshader/waveformrendererrekordbox3band.h"
#include "waveform/renderers/allshader/waveformrendermark.h"
#include "waveform/renderers/allshader/waveformrendermarkrange.h"
#include "waveform/widgets/allshader/moc_rekordbox3bandwaveformwidget.cpp"

namespace allshader {

Rekordbox3BandWaveformWidget::Rekordbox3BandWaveformWidget(const QString& group, QWidget* parent)
        : WaveformWidget(group, parent) {
    addRenderer<WaveformRenderBackground>();
    addRenderer<WaveformRendererEndOfTrack>();
    addRenderer<WaveformRendererPreroll>();
    addRenderer<WaveformRenderMarkRange>();
    addRenderer<WaveformRendererRekordbox3Band>();
    addRenderer<WaveformRenderBeat>();
    addRenderer<WaveformRenderMark>();

    m_initSuccess = init();
}

void Rekordbox3BandWaveformWidget::castToQWidget() {
    m_widget = this;
}

void Rekordbox3BandWaveformWidget::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
}

} // namespace allshader
