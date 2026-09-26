#include "widget/wpinkeypad.h"

#include <QEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QStyle>

#include "control/controlproxy.h"
#include "moc_wpinkeypad.cpp"
#include "skin/legacy/skincontext.h"

namespace {
const char* kDownProperty = "down";
constexpr int kPinLength = 4;
constexpr int kKeySize = 60;
constexpr int kBodyMaxWidth = 480;
constexpr int kColumns = 6;

void restyle(QStyle* pStyle, QWidget* pWidget) {
    pStyle->unpolish(pWidget);
    pStyle->polish(pWidget);
}
} // namespace

WPinKeypad::WPinKeypad(QWidget* parent)
        : WWidget(parent),
          m_pTitleLabel(new QLabel(tr("New PIN"), this)),
          m_pFieldLabel(new QLabel(this)),
          m_pBackspaceButton(new QPushButton(tr("Del"), this)),
          m_pCancelButton(new QPushButton(tr("Cancel"), this)),
          m_pSaveButton(new QPushButton(tr("Save"), this)),
          m_pPinControl(new ControlProxy(QStringLiteral("[BiteDJ]"),
                  QStringLiteral("visuals_admin_pin"),
                  this,
                  ControlFlag::AllowMissingOrInvalid | ControlFlag::NoWarnIfMissing)),
          m_pPageControl(new ControlProxy(QStringLiteral("[BiteDJ]"),
                  QStringLiteral("visuals_page"),
                  this,
                  ControlFlag::AllowMissingOrInvalid | ControlFlag::NoWarnIfMissing)),
          m_returnPage(0),
          m_pressedKeyIndex(-1) {
    setAttribute(Qt::WA_StyledBackground, true);
    // See WWifiList: a button-less synthesized move is dropped unless the
    // widget tracks the mouse.
    setMouseTracking(true);

    auto* pBody = new QWidget(this);
    pBody->setObjectName(QStringLiteral("PinKeypadBody"));
    pBody->setMaximumWidth(kBodyMaxWidth);
    auto* pOuter = new QHBoxLayout(this);
    pOuter->setContentsMargins(0, 0, 0, 0);
    pOuter->addStretch(1);
    pOuter->addWidget(pBody, 0, Qt::AlignTop);
    pOuter->addStretch(1);

    // Adding a widget to this grid reparents it to pBody.
    auto* pGrid = new QGridLayout(pBody);
    pGrid->setContentsMargins(8, 8, 8, 8);
    pGrid->setHorizontalSpacing(6);
    pGrid->setVerticalSpacing(6);
    for (int col = 0; col < kColumns; ++col) {
        pGrid->setColumnStretch(col, 1);
        pGrid->setColumnMinimumWidth(col, kKeySize);
    }

    m_pTitleLabel->setObjectName(QStringLiteral("PinKeypadTitle"));
    m_pFieldLabel->setObjectName(QStringLiteral("PinKeypadField"));
    m_pFieldLabel->setTextFormat(Qt::PlainText);
    m_pFieldLabel->setMinimumHeight(44);
    pGrid->addWidget(m_pTitleLabel, 0, 0, 1, 3);
    pGrid->addWidget(m_pFieldLabel, 0, 3, 1, 3);

    for (int d = 0; d <= 9; ++d) {
        auto* pKey = new QPushButton(QString::number(d), pBody);
        pKey->setObjectName(QStringLiteral("PinKey"));
        m_digitButtons.append(pKey);
    }
    // 1 2 3 4 5 Del / 6 7 8 9 0.
    for (int i = 0; i < 5; ++i) {
        pGrid->addWidget(m_digitButtons.at(i + 1), 1, i);
    }
    pGrid->addWidget(m_pBackspaceButton, 1, 5);
    for (int i = 0; i < 4; ++i) {
        pGrid->addWidget(m_digitButtons.at(i + 6), 2, i);
    }
    pGrid->addWidget(m_digitButtons.at(0), 2, 4);
    m_pBackspaceButton->setObjectName(QStringLiteral("PinKeyBackspace"));
    m_pCancelButton->setObjectName(QStringLiteral("PinKeypadCancel"));
    m_pSaveButton->setObjectName(QStringLiteral("PinKeypadSave"));
    pGrid->addWidget(m_pCancelButton, 3, 0, 1, 3);
    pGrid->addWidget(m_pSaveButton, 3, 3, 1, 3);

    m_allButtons = m_digitButtons;
    m_allButtons.append(m_pBackspaceButton);
    m_allButtons.append(m_pCancelButton);
    m_allButtons.append(m_pSaveButton);
    for (QPushButton* pButton : std::as_const(m_allButtons)) {
        pButton->setFocusPolicy(Qt::NoFocus);
        pButton->setMinimumWidth(kKeySize);
        pButton->setFixedHeight(kKeySize);
        connect(pButton, &QPushButton::clicked, this, &WPinKeypad::onKeyClicked);
    }
    refresh();
}

WPinKeypad::~WPinKeypad() = default;

void WPinKeypad::setup(const QDomNode& node, const SkinContext& context) {
    bool ok = false;
    const int page = context.selectInt(node, QStringLiteral("ReturnPage"), &ok);
    setReturnPage(ok ? page : 0);
}

void WPinKeypad::setReturnPage(int page) {
    m_returnPage = page;
}

void WPinKeypad::mousePressEvent(QMouseEvent* e) {
    // Reset first, so a press with another button leaves no stale index.
    m_pressedKeyIndex = -1;
    if (e->button() != Qt::LeftButton) {
        WWidget::mousePressEvent(e);
        return;
    }
    m_pressedKeyIndex = keyIndexAt(e->globalPosition().toPoint());
    setKeyDown(m_pressedKeyIndex, true);
    e->accept();
}

void WPinKeypad::mouseMoveEvent(QMouseEvent* e) {
    // "down" follows the finger off the pressed key and back on.
    if (m_pressedKeyIndex >= 0) {
        setKeyDown(m_pressedKeyIndex,
                keyIndexAt(e->globalPosition().toPoint()) == m_pressedKeyIndex);
    }
    e->accept();
}

void WPinKeypad::mouseReleaseEvent(QMouseEvent* e) {
    const int pressedIndex = m_pressedKeyIndex;
    m_pressedKeyIndex = -1;
    setKeyDown(pressedIndex, false);
    if (pressedIndex >= 0 && e->button() == Qt::LeftButton &&
            keyIndexAt(e->globalPosition().toPoint()) == pressedIndex) {
        dispatchButton(m_allButtons.at(pressedIndex));
        e->accept();
        return;
    }
    WWidget::mouseReleaseEvent(e);
}

bool WPinKeypad::event(QEvent* e) {
    if (e->type() == QEvent::TouchCancel) {
        setKeyDown(m_pressedKeyIndex, false);
        m_pressedKeyIndex = -1;
    }
    return WWidget::event(e);
}

void WPinKeypad::hideEvent(QHideEvent* e) {
    WWidget::hideEvent(e);
    setKeyDown(m_pressedKeyIndex, false);
    m_pressedKeyIndex = -1;
    m_digits.clear();
    refresh();
}

int WPinKeypad::keyIndexAt(const QPoint& globalPos) const {
    for (int i = 0; i < m_allButtons.size(); ++i) {
        const QPushButton* pButton = m_allButtons.at(i);
        if (!pButton->isVisible() || !pButton->isEnabled()) {
            continue;
        }
        if (pButton->rect().contains(pButton->mapFromGlobal(globalPos))) {
            return i;
        }
    }
    return -1;
}

void WPinKeypad::setKeyDown(int index, bool down) {
    if (index < 0 || index >= m_allButtons.size()) {
        return;
    }
    QPushButton* pButton = m_allButtons.at(index);
    if (pButton->property(kDownProperty).toBool() == down) {
        return;
    }
    pButton->setProperty(kDownProperty, down);
    restyle(style(), pButton);
}

void WPinKeypad::onKeyClicked() {
    // Desktop path: a real mouse click reaches the child key directly.
    dispatchButton(qobject_cast<QPushButton*>(sender()));
}

void WPinKeypad::dispatchButton(QPushButton* pButton) {
    if (!pButton || !pButton->isEnabled()) {
        return;
    }
    const int digit = m_digitButtons.indexOf(pButton);
    if (digit >= 0) {
        typeDigit(digit);
    } else if (pButton == m_pBackspaceButton) {
        backspace();
    } else if (pButton == m_pCancelButton) {
        cancel();
    } else if (pButton == m_pSaveButton) {
        save();
    }
}

void WPinKeypad::typeDigit(int digit) {
    if (m_digits.size() >= kPinLength) {
        return;
    }
    m_digits += QString::number(digit);
    refresh();
}

void WPinKeypad::backspace() {
    m_digits.chop(1);
    refresh();
}

void WPinKeypad::save() {
    if (m_digits.size() != kPinLength) {
        return;
    }
    const int pin = m_digits.toInt();
    m_digits.clear();
    refresh();
    emit pinSaved(pin);
    m_pPinControl->set(pin);
    m_pPageControl->set(m_returnPage);
}

void WPinKeypad::cancel() {
    m_digits.clear();
    refresh();
    emit cancelled();
    m_pPageControl->set(m_returnPage);
}

void WPinKeypad::refresh() {
    m_pFieldLabel->setText(m_digits + QString(kPinLength - m_digits.size(), QChar(0x00B7)));
    const bool complete = m_digits.size() == kPinLength;
    if (m_pSaveButton->isEnabled() != complete) {
        m_pSaveButton->setEnabled(complete);
        restyle(style(), m_pSaveButton);
    }
}
