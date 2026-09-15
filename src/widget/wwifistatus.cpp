#include "widget/wwifistatus.h"

#include <QLabel>
#include <QVBoxLayout>

#include "control/controlproxy.h"
#include "moc_wwifistatus.cpp"
#include "preferences/wifisettings.h"
#include "skin/legacy/skincontext.h"

namespace {
const char* kTitleObjectName = "WifiStatusTitle";
const char* kDetailObjectName = "WifiStatusDetail";
} // namespace

WWifiStatus::WWifiStatus(QWidget* parent)
        : WWidget(parent),
          m_pTitleLabel(new QLabel(this)),
          m_pDetailLabel(new QLabel(this)),
          m_pPageControl(new ControlProxy(
                  QStringLiteral("[Wifi]"), QStringLiteral("page"), this)),
          m_page(WifiSettings::kPageList),
          m_visibleToBackend(false) {
    setAttribute(Qt::WA_StyledBackground, true);

    auto* pLayout = new QVBoxLayout(this);
    pLayout->setContentsMargins(0, 0, 0, 0);
    pLayout->setSpacing(0);

    m_pTitleLabel->setObjectName(kTitleObjectName);
    m_pTitleLabel->setWordWrap(true);
    m_pDetailLabel->setObjectName(kDetailObjectName);
    m_pDetailLabel->setWordWrap(true);
    pLayout->addWidget(m_pTitleLabel);
    pLayout->addWidget(m_pDetailLabel);
    // Without this, a QVBoxLayout with all-default (0) stretch factors gives
    // its two Preferred-policy QLabels a share of any extra vertical space
    // the skin gives this widget -- which on page 3, sized to the whole
    // page, spread the title and detail hundreds of pixels apart instead of
    // stacking them. The stretch item is Expanding by construction and
    // claims that leftover space instead, pinning both labels to their
    // natural (sizeHint) height at the top.
    pLayout->addStretch(1);

    m_pPageControl->connectValueChanged(this, &WWifiStatus::onPageChanged);
    if (m_pPageControl->valid()) {
        m_page = static_cast<int>(m_pPageControl->get());
    }

    if (WifiSettings* pSettings = WifiSettings::tryInstance()) {
        connect(pSettings, &WifiSettings::statusChanged, this, &WWifiStatus::onStatusChanged);
        connect(pSettings,
                &WifiSettings::joinTargetChanged,
                this,
                &WWifiStatus::onJoinTargetChanged);
        m_statusLine = pSettings->statusLine();
        m_joinTarget = pSettings->joinTarget();
    }
    refresh();
}

WWifiStatus::~WWifiStatus() {
    setVisibleToBackend(false);
}

void WWifiStatus::setup(const QDomNode& /*node*/, const SkinContext& /*context*/) {
    // No XML-side configuration; everything is driven by WifiSettings and
    // the [Wifi],page control.
}

void WWifiStatus::showEvent(QShowEvent* e) {
    WWidget::showEvent(e);
    setVisibleToBackend(true);
}

void WWifiStatus::hideEvent(QHideEvent* e) {
    WWidget::hideEvent(e);
    setVisibleToBackend(false);
}

void WWifiStatus::setVisibleToBackend(bool visible) {
    if (m_visibleToBackend == visible) {
        return;
    }
    m_visibleToBackend = visible;
    if (WifiSettings* pSettings = WifiSettings::tryInstance()) {
        pSettings->setClientVisible(this, visible);
    }
}

void WWifiStatus::onStatusChanged(const QString& statusLine, int /*state*/) {
    m_statusLine = statusLine;
    refresh();
}

void WWifiStatus::onJoinTargetChanged(const QString& ssid) {
    m_joinTarget = ssid;
    refresh();
}

void WWifiStatus::onPageChanged(double page) {
    m_page = static_cast<int>(page);
    refresh();
}

void WWifiStatus::refresh() {
    if (m_page == WifiSettings::kPageJoining) {
        m_pTitleLabel->setText(tr("Joining %1...").arg(m_joinTarget));
        m_pDetailLabel->setText(QString());
    } else if (m_page == WifiSettings::kPageManage) {
        m_pTitleLabel->setText(m_joinTarget);
        m_pDetailLabel->setText(m_statusLine);
    } else {
        m_pTitleLabel->setText(m_statusLine);
        m_pDetailLabel->setText(QString());
    }
}
