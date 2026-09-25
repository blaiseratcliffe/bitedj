#pragma once

#include <QString>

#include "widget/wwidget.h"

class QDomNode;
class QLabel;
class SkinContext;

// Bite DJ: one line of text from VisualsSets (src/preferences/visualssets.h)
// on the Visuals settings page: the active set's name for the Set row, or
// the admin address, IP and PIN for the Admin row. Chosen in the skin with
// <Field>set</Field> or <Field>admin</Field>.
//
// A WWidget holding a QLabel child (objectName VisualsLabelText) rather than
// a WLabel, for the same reason WWifiStatus is one: the text comes from a
// singleton's signal, not from a control or the skin. PlainText always: a
// set name is typed on a phone and must never render as rich text. Word
// wrap on, so a long Admin line breaks at a space instead of clipping. The
// admin field registers with VisualsSets::setClientVisible while shown, so
// the IP is only polled while someone can read it.
//
// Without the singleton (stock Mixxx, or a skin parsed in a test) it shows
// "Visuals sets unavailable" and does nothing else.
class WVisualsLabel : public WWidget {
    Q_OBJECT
  public:
    enum class Field {
        Set,
        Admin,
    };

    explicit WVisualsLabel(QWidget* parent = nullptr);
    ~WVisualsLabel() override;

    void setup(const QDomNode& node, const SkinContext& context);
    // What setup() calls with the skin's <Field>; public for tests.
    void setField(Field field);

  protected:
    void showEvent(QShowEvent* e) override;
    void hideEvent(QHideEvent* e) override;

  private slots:
    void onText(const QString& text);

  private:
    void setVisibleToBackend(bool visible);

    QLabel* m_pText;
    Field m_field;
    bool m_connected;
    bool m_visibleToBackend;
};
