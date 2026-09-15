#include "widget/wwifikeypad.h"

#include <QGridLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QStyle>

#include "moc_wwifikeypad.cpp"
#include "preferences/wifisettings.h"
#include "skin/legacy/skincontext.h"

namespace {
const char* kTitleObjectName = "WifiKeypadTitle";
const char* kFieldObjectName = "WifiKeypadField";
const char* kKeyObjectName = "WifiKey";
const char* kShiftObjectName = "WifiKeyShift";
const char* kLayoutObjectName = "WifiKeyLayout";
const char* kBackspaceObjectName = "WifiKeyBackspace";
const char* kSpaceObjectName = "WifiKeySpace";
const char* kCancelObjectName = "WifiKeypadCancel";
const char* kJoinObjectName = "WifiKeypadJoin";
const char* kShiftActiveProperty = "shiftActive";

constexpr int kGridColumns = 10;
constexpr int kMinKeyWidth = 44;
constexpr int kMinKeyHeight = 44;

constexpr int kCharSlotCount = 26;
constexpr int kLayoutLetters = 0;
constexpr int kLayout123 = 1;
constexpr int kLayoutSymbols = 2;
constexpr int kLayoutCount = 3;

// Row-major: Q-row (10 slots), A-row (9 slots), Z-row (7 slots). See
// wwifikeypad.h for the derivation and the disjointness/coverage argument;
// WifiKeypadAsciiCoverageTest in wwifikeypad_test.cpp checks it directly
// rather than trusting either comment.
constexpr char kLettersLower[kCharSlotCount] = {
        'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p',
        'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l',
        'z', 'x', 'c', 'v', 'b', 'n', 'm'};

// '\0' marks a slot this layout leaves blank/disabled.
constexpr char kLayout123Chars[kCharSlotCount] = {
        '1', '2', '3', '4', '5', '6', '7', '8', '9', '0',
        '!', '#', '$', '%', '&', '(', ')', '*', '+',
        '-', '=', '@', '^', '_', '\0', '\0'};

constexpr char kSymbolsChars[kCharSlotCount] = {
        '"', '\'', ',', '.', '/', ':', ';', '<', '>', '?',
        '[', '\\', ']', '`', '{', '|', '}', '~', '\0',
        '\0', '\0', '\0', '\0', '\0', '\0', '\0'};

const char* layoutChars(int layout) {
    switch (layout) {
    case kLayout123:
        return kLayout123Chars;
    case kLayoutSymbols:
        return kSymbolsChars;
    case kLayoutLetters:
    default:
        return kLettersLower;
    }
}

// Names the layout a tap on the switch key will move TO, not the one it's
// showing now (0 -> "123" -> "#+=" -> "ABC" -> back to 0's own "123").
QString layoutSwitchLabel(int layout) {
    switch (layout) {
    case kLayout123:
        return QStringLiteral("#+=");
    case kLayoutSymbols:
        return QStringLiteral("ABC");
    case kLayoutLetters:
    default:
        return QStringLiteral("123");
    }
}

// Repolishes a widget so a property change picked up by the stylesheet
// ([shiftActive]) is actually repainted. Same small helper WWifiList and
// WUsbList each keep locally.
void restyle(QStyle* pStyle, QWidget* pWidget) {
    pStyle->unpolish(pWidget);
    pStyle->polish(pWidget);
}
} // namespace

WWifiKeypad::WWifiKeypad(QWidget* parent)
        : WWidget(parent),
          m_pTitleLabel(new QLabel(this)),
          m_pFieldLabel(new QLabel(this)),
          m_pLayout(new QGridLayout(this)),
          m_pShiftButton(new QPushButton(this)),
          m_pBackspaceButton(new QPushButton(tr("Backspace"), this)),
          m_pLayoutButton(new QPushButton(this)),
          m_pSpaceButton(new QPushButton(tr("Space"), this)),
          m_pCancelButton(new QPushButton(tr("Cancel"), this)),
          m_pJoinButton(new QPushButton(tr("Join"), this)),
          m_currentLayout(kLayoutLetters),
          m_shiftActive(false),
          m_pressedKeyIndex(-1) {
    setAttribute(Qt::WA_StyledBackground, true);
    // See WWifiList/WUsbList: a button-less synthesized move is dropped
    // unless the widget tracks the mouse.
    setMouseTracking(true);

    m_pTitleLabel->setObjectName(kTitleObjectName);
    m_pFieldLabel->setObjectName(kFieldObjectName);
    m_pShiftButton->setObjectName(kShiftObjectName);
    m_pShiftButton->setText(tr("Shift"));
    m_pBackspaceButton->setObjectName(kBackspaceObjectName);
    m_pLayoutButton->setObjectName(kLayoutObjectName);
    m_pSpaceButton->setObjectName(kSpaceObjectName);
    m_pCancelButton->setObjectName(kCancelObjectName);
    m_pJoinButton->setObjectName(kJoinObjectName);

    const QList<QPushButton*> specialButtons = {m_pShiftButton,
            m_pBackspaceButton,
            m_pLayoutButton,
            m_pSpaceButton,
            m_pCancelButton,
            m_pJoinButton};
    for (QPushButton* pButton : specialButtons) {
        pButton->setFocusPolicy(Qt::NoFocus);
        pButton->setMinimumSize(kMinKeyWidth, kMinKeyHeight);
        connect(pButton, &QPushButton::clicked, this, &WWifiKeypad::onKeyClicked);
    }

    m_charButtons.reserve(kCharSlotCount);
    m_currentChars.reserve(kCharSlotCount);
    for (int i = 0; i < kCharSlotCount; ++i) {
        auto* pButton = new QPushButton(this);
        pButton->setObjectName(kKeyObjectName);
        pButton->setFocusPolicy(Qt::NoFocus);
        pButton->setMinimumSize(kMinKeyWidth, kMinKeyHeight);
        connect(pButton, &QPushButton::clicked, this, &WWifiKeypad::onKeyClicked);
        m_charButtons.append(pButton);
        m_currentChars.append(QChar());
    }

    m_pLayout->setContentsMargins(8, 8, 8, 8);
    m_pLayout->setHorizontalSpacing(6);
    m_pLayout->setVerticalSpacing(6);
    for (int col = 0; col < kGridColumns; ++col) {
        m_pLayout->setColumnStretch(col, 1);
    }
    // The four key rows share out any extra vertical space the panel gives
    // this widget; title, field and the Cancel/Join row stay at their
    // content size.
    m_pLayout->setRowStretch(2, 1);
    m_pLayout->setRowStretch(3, 1);
    m_pLayout->setRowStretch(4, 1);
    m_pLayout->setRowStretch(5, 1);

    m_pLayout->addWidget(m_pTitleLabel, 0, 0, 1, kGridColumns);
    m_pLayout->addWidget(m_pFieldLabel, 1, 0, 1, kGridColumns);

    // Q-row: all 10 columns.
    for (int col = 0; col < 10; ++col) {
        m_pLayout->addWidget(m_charButtons.at(col), 2, col);
    }
    // A-row: 9 keys; column 9 is left empty.
    for (int col = 0; col < 9; ++col) {
        m_pLayout->addWidget(m_charButtons.at(10 + col), 3, col);
    }
    // Shift, Z-row (7 keys), Backspace (2 columns wide -- a bigger target
    // for the key that undoes a mistake).
    m_pLayout->addWidget(m_pShiftButton, 4, 0);
    for (int col = 0; col < 7; ++col) {
        m_pLayout->addWidget(m_charButtons.at(19 + col), 4, 1 + col);
    }
    m_pLayout->addWidget(m_pBackspaceButton, 4, 8, 1, 2);

    // Layout switch, Space (wide, the rest of the row).
    m_pLayout->addWidget(m_pLayoutButton, 5, 0, 1, 2);
    m_pLayout->addWidget(m_pSpaceButton, 5, 2, 1, 8);

    // Cancel / Join, half the width each.
    m_pLayout->addWidget(m_pCancelButton, 6, 0, 1, 5);
    m_pLayout->addWidget(m_pJoinButton, 6, 5, 1, 5);

    // Fixed hit-test order for the outer widget's press/release dispatch.
    // Never rebuilt: see the header comment on why that matters mid-gesture.
    m_allButtons = m_charButtons;
    m_allButtons.append(m_pShiftButton);
    m_allButtons.append(m_pBackspaceButton);
    m_allButtons.append(m_pLayoutButton);
    m_allButtons.append(m_pSpaceButton);
    m_allButtons.append(m_pCancelButton);
    m_allButtons.append(m_pJoinButton);

    refreshLayout();

    if (WifiSettings* pSettings = WifiSettings::tryInstance()) {
        connect(pSettings,
                &WifiSettings::joinTargetChanged,
                this,
                &WWifiKeypad::onJoinTargetChanged);
        connect(pSettings,
                &WifiSettings::passwordChanged,
                this,
                &WWifiKeypad::onPasswordChanged);
        onJoinTargetChanged(pSettings->joinTarget());
        onPasswordChanged(pSettings->passwordLength());
    } else {
        onJoinTargetChanged(QString());
        onPasswordChanged(0);
    }
}

void WWifiKeypad::setup(const QDomNode& /*node*/, const SkinContext& /*context*/) {
    // No XML-side configuration; everything is driven by WifiSettings.
}

void WWifiKeypad::onJoinTargetChanged(const QString& ssid) {
    m_pTitleLabel->setText(
            ssid.isEmpty() ? tr("Wi-Fi password") : tr("Password for %1").arg(ssid));
}

void WWifiKeypad::onPasswordChanged(int length) {
    m_pFieldLabel->setText(QString(length, QChar(0x2022)));
}

void WWifiKeypad::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) {
        WWidget::mousePressEvent(e);
        return;
    }
    m_pressedKeyIndex = keyIndexAt(e->globalPosition().toPoint());
    e->accept();
}

void WWifiKeypad::mouseMoveEvent(QMouseEvent* e) {
    // Nothing scrolls here and the commit decision is made on release by
    // comparing hit-tests, not by tracking the drag -- so there is nothing
    // to do but keep the synthesized event from leaking anywhere else.
    e->accept();
}

void WWifiKeypad::mouseReleaseEvent(QMouseEvent* e) {
    const int pressedIndex = m_pressedKeyIndex;
    m_pressedKeyIndex = -1;

    if (pressedIndex >= 0) {
        const int releasedIndex = keyIndexAt(e->globalPosition().toPoint());
        if (releasedIndex == pressedIndex) {
            dispatchButton(m_allButtons.at(pressedIndex));
            e->accept();
            return;
        }
    }
    // No press was pending, or the release landed off the pressed key (or on
    // a different one): nothing to commit. Falls through to the base
    // handler rather than swallowing the event, the same as
    // WWifiList/WUsbList do on a miss.
    WWidget::mouseReleaseEvent(e);
}

int WWifiKeypad::keyIndexAt(const QPoint& globalPos) const {
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

void WWifiKeypad::onKeyClicked() {
    dispatchButton(qobject_cast<QPushButton*>(sender()));
}

void WWifiKeypad::dispatchButton(QPushButton* pButton) {
    if (pButton == nullptr) {
        return;
    }
    if (pButton == m_pShiftButton) {
        toggleShift();
        return;
    }
    if (pButton == m_pBackspaceButton) {
        commitBackspace();
        return;
    }
    if (pButton == m_pLayoutButton) {
        cycleLayout();
        return;
    }
    if (pButton == m_pSpaceButton) {
        commitChar(QChar(' '));
        return;
    }
    if (pButton == m_pCancelButton) {
        commitCancel();
        return;
    }
    if (pButton == m_pJoinButton) {
        commitJoin();
        return;
    }

    const int index = m_charButtons.indexOf(pButton);
    if (index < 0 || !pButton->isEnabled()) {
        return;
    }
    const QChar c = m_currentChars.at(index);
    commitChar(c);
    // One-shot Shift: consumed by the letter it capitalized. A digit or
    // symbol key can't reach here with Shift active -- refreshLayout()
    // disables Shift outside layout 0, and cycling layout always clears it
    // first -- so this check only ever fires for a letter.
    if (m_currentLayout == kLayoutLetters && m_shiftActive) {
        m_shiftActive = false;
        refreshLayout();
    }
}

void WWifiKeypad::refreshLayout() {
    const char* chars = layoutChars(m_currentLayout);
    for (int i = 0; i < kCharSlotCount; ++i) {
        QPushButton* pButton = m_charButtons.at(i);
        const char c = chars[i];
        if (c == '\0') {
            pButton->setText(QString());
            pButton->setEnabled(false);
            m_currentChars[i] = QChar();
            continue;
        }
        QChar ch = QLatin1Char(c);
        if (m_currentLayout == kLayoutLetters && m_shiftActive) {
            ch = ch.toUpper();
        }
        pButton->setText(QString(ch));
        pButton->setEnabled(true);
        m_currentChars[i] = ch;
    }

    const bool lettersActive = m_currentLayout == kLayoutLetters;
    m_pShiftButton->setEnabled(lettersActive);
    m_pShiftButton->setProperty(kShiftActiveProperty, lettersActive && m_shiftActive);
    restyle(style(), m_pShiftButton);

    m_pLayoutButton->setText(layoutSwitchLabel(m_currentLayout));
}

void WWifiKeypad::toggleShift() {
    if (m_currentLayout != kLayoutLetters) {
        return;
    }
    m_shiftActive = !m_shiftActive;
    refreshLayout();
}

void WWifiKeypad::cycleLayout() {
    m_currentLayout = (m_currentLayout + 1) % kLayoutCount;
    m_shiftActive = false;
    refreshLayout();
}

void WWifiKeypad::commitChar(QChar c) {
    emit characterTyped(c);
    if (WifiSettings* pSettings = WifiSettings::tryInstance()) {
        pSettings->appendPasswordChar(c);
    }
}

void WWifiKeypad::commitBackspace() {
    emit backspaceTyped();
    if (WifiSettings* pSettings = WifiSettings::tryInstance()) {
        pSettings->backspacePassword();
    }
}

void WWifiKeypad::commitCancel() {
    emit cancelRequested();
    if (WifiSettings* pSettings = WifiSettings::tryInstance()) {
        pSettings->cancelJoin();
    }
}

void WWifiKeypad::commitJoin() {
    emit joinRequested();
    if (WifiSettings* pSettings = WifiSettings::tryInstance()) {
        pSettings->submitJoin();
    }
}
