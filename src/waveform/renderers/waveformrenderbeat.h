#pragma once

#include <QColor>

#include "skin/legacy/skincontext.h"
#include "util/class.h"
#include "waveform/renderers/waveformrendererabstract.h"

class WaveformRenderBeat : public WaveformRendererAbstract {
  public:
    explicit WaveformRenderBeat(WaveformWidgetRenderer* waveformWidgetRenderer);
    virtual ~WaveformRenderBeat();

    virtual void setup(const QDomNode& node, const SkinContext& context);
    virtual void draw(QPainter* painter, QPaintEvent* event);

  private:
    QColor m_beatColor;
    QColor m_downbeatColor;
    // CDJ style tick geometry, kept in step with allshader::WaveformRenderBeat.
    // See setup() for the skin tags.
    float m_beatTickFraction;
    float m_beatTickMinPx;
    float m_beatTickMaxPx;
    QVector<QLineF> m_beats;
    QVector<QLineF> m_fadingBeats;
    QVector<QLineF> m_downbeats;
    QVector<QLineF> m_fadingDownbeats;

    DISALLOW_COPY_AND_ASSIGN(WaveformRenderBeat);
};
