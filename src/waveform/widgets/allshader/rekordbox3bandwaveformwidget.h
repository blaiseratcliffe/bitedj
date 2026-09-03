#pragma once

#include "util/class.h"
#include "waveform/widgets/allshader/waveformwidget.h"
#include "waveform/widgets/waveformwidgettype.h"

class WaveformWidgetFactory;

namespace allshader {
class Rekordbox3BandWaveformWidget;
}

class allshader::Rekordbox3BandWaveformWidget final : public allshader::WaveformWidget {
    Q_OBJECT
  public:
    WaveformWidgetType::Type getType() const override {
        return WaveformWidgetType::AllShaderRekordbox3BandWaveform;
    }

    static inline QString getWaveformWidgetName() {
        return tr("3Band");
    }
    static constexpr bool useOpenGl() {
        return true;
    }
    static constexpr bool useOpenGles() {
        return true;
    }
    static constexpr bool useOpenGLShaders() {
        return true;
    }
    static constexpr bool useTextureForWaveform() {
        return false;
    }
    static constexpr WaveformWidgetCategory category() {
        return WaveformWidgetCategory::AllShader;
    }

  protected:
    void castToQWidget() override;
    void paintEvent(QPaintEvent* event) override;

  private:
    Rekordbox3BandWaveformWidget(const QString& group, QWidget* parent);
    friend class ::WaveformWidgetFactory;

    DISALLOW_COPY_AND_ASSIGN(Rekordbox3BandWaveformWidget);
};
