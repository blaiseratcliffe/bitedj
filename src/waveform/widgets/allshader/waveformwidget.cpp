#include "waveform/widgets/allshader/waveformwidget.h"

#include <QApplication>
#include <QWheelEvent>

#include "moc_waveformwidget.cpp"
#include "waveform/renderers/allshader/waveformrendererabstract.h"
#include "waveform/renderers/waveformrendererabstract.h"

namespace allshader {

WaveformWidget::WaveformWidget(const QString& group, QWidget* parent)
        : WGLWidget(parent), WaveformWidgetAbstract(group) {
}

WaveformWidget::~WaveformWidget() {
    makeCurrentIfNeeded();
    for (auto* pRenderer : std::as_const(m_rendererStack)) {
        delete pRenderer;
    }
    m_rendererStack.clear();
    // No doneCurrent() here. The renderers above are not the only GL
    // resources this object owns: base destructors run after this body and
    // free textures of their own, and releasing the context here left them
    // with none. Qt then logs "QOpenGLTexturePrivate::destroy() called
    // without a current context / Texture has not been destroyed" and the
    // texture leaks. Measured on a waveform type change, which deletes
    // every deck's widget: two leaked textures per loaded deck, so eight
    // with four decks up, and none of them from the loop above (each of
    // its seven deletes was verified to run with a context current).
    //
    // ~WGLWidget releases it instead. It is the last base to destruct and
    // it owns the window the context belongs to, so the context stays
    // current for the whole chain and is dropped once, at the end.
}

mixxx::Duration WaveformWidget::render() {
    makeCurrentIfNeeded();
    paintGL();
    doneCurrent();
    // In the legacy widgets, this is used to "return timer for painter setup"
    // which is not relevant here. Also note that the return value is not used
    // at all, so it might be better to remove it everywhere. In the meantime.
    // we need to return something for API compatibility.
    return mixxx::Duration();
}

void WaveformWidget::paintGL() {
    if (shouldOnlyDrawBackground()) {
        if (!m_rendererStack.empty()) {
            m_rendererStack[0]->allshaderWaveformRenderer()->paintGL();
        }
    } else {
        for (auto* pRenderer : std::as_const(m_rendererStack)) {
            pRenderer->allshaderWaveformRenderer()->paintGL();
        }
    }
}

void WaveformWidget::initializeGL() {
    for (auto* pRenderer : std::as_const(m_rendererStack)) {
        pRenderer->allshaderWaveformRenderer()->initializeGL();
    }
}

void WaveformWidget::resizeGL(int w, int h) {
    for (auto* pRenderer : std::as_const(m_rendererStack)) {
        pRenderer->allshaderWaveformRenderer()->resizeGL(w, h);
    }
}

void WaveformWidget::wheelEvent(QWheelEvent* pEvent) {
    QApplication::sendEvent(parentWidget(), pEvent);
    pEvent->accept();
}

void WaveformWidget::leaveEvent(QEvent* pEvent) {
    QApplication::sendEvent(parentWidget(), pEvent);
    pEvent->accept();
}

} // namespace allshader
