#include "widget/wvisualslabel.h"

#include <QLabel>
#include <QVBoxLayout>

#include "moc_wvisualslabel.cpp"
#include "preferences/visualssets.h"
#include "skin/legacy/skincontext.h"

WVisualsLabel::WVisualsLabel(QWidget* parent)
        : WWidget(parent),
          m_pText(new QLabel(this)),
          m_field(Field::Set),
          m_connected(false),
          m_visibleToBackend(false) {
    setAttribute(Qt::WA_StyledBackground, true);
    auto* pLayout = new QVBoxLayout(this);
    pLayout->setContentsMargins(0, 0, 0, 0);
    pLayout->setSpacing(0);
    m_pText->setObjectName(QStringLiteral("VisualsLabelText"));
    // Set names arrive in a file written over the LAN (as wwifilist.cpp
    // treats SSIDs): never let one render as markup.
    m_pText->setTextFormat(Qt::PlainText);
    m_pText->setWordWrap(true);
    pLayout->addWidget(m_pText);
    m_pText->setText(tr("Visuals sets unavailable"));
}

WVisualsLabel::~WVisualsLabel() {
    setVisibleToBackend(false);
}

void WVisualsLabel::setup(const QDomNode& node, const SkinContext& context) {
    const QString field = context.selectString(node, QStringLiteral("Field")).trimmed();
    setField(field == QStringLiteral("admin") ? Field::Admin : Field::Set);
}

void WVisualsLabel::setField(Field field) {
    m_field = field;
    VisualsSets* pSets = VisualsSets::tryInstance();
    if (!pSets) {
        m_pText->setText(tr("Visuals sets unavailable"));
        return;
    }
    if (!m_connected) {
        m_connected = true;
        if (m_field == Field::Admin) {
            connect(pSets, &VisualsSets::adminInfoChanged, this, &WVisualsLabel::onText);
        } else {
            connect(pSets, &VisualsSets::activeNameChanged, this, &WVisualsLabel::onText);
        }
    }
    onText(m_field == Field::Admin ? pSets->adminInfo() : pSets->activeName());
    if (isVisible()) {
        setVisibleToBackend(true);
    }
}

void WVisualsLabel::onText(const QString& text) {
    m_pText->setText(text);
}

void WVisualsLabel::showEvent(QShowEvent* e) {
    WWidget::showEvent(e);
    setVisibleToBackend(true);
}

void WVisualsLabel::hideEvent(QHideEvent* e) {
    WWidget::hideEvent(e);
    setVisibleToBackend(false);
}

void WVisualsLabel::setVisibleToBackend(bool visible) {
    // Only the Admin line needs the IP polled.
    if (m_field != Field::Admin && visible) {
        return;
    }
    if (m_visibleToBackend == visible) {
        return;
    }
    m_visibleToBackend = visible;
    if (VisualsSets* pSets = VisualsSets::tryInstance()) {
        pSets->setClientVisible(this, visible);
    }
}
