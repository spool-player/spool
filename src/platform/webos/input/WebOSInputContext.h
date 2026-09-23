// Platform input context that drives the stock webOS on-screen keyboard
// (MaliitServer) through the compositor's text_model wayland protocol.
// Ported to Qt 6 from qtwayland-webos waylandinputcontext.cpp
// (Copyright (c) 2013-2021 LG Electronics, Inc., Apache-2.0).
#pragma once

#include <qpa/qplatforminputcontext.h>

#include <QInputMethodEvent>
#include <QRectF>

#include <wayland-client.h>

#include "../protocol/wayland-text-client-protocol.h"
#include "WebOSKeysymMap.h"

namespace Spool {

// Preedit state accumulated between preedit_styling/cursor and the final
// preedit_string event.
struct PreEditData {
    QList<QInputMethodEvent::Attribute> formats;
    QString preEdit;
    int32_t cursor = 0;
};

class WebOSInputContext : public QPlatformInputContext {
    Q_OBJECT

public:
    // input_panel_state event values sent by the LSM compositor.
    enum InputPanelState {
        InputPanelUnknownState = 0xffffffff,
        InputPanelHidden = 0,
        InputPanelShown = 1,
        InputPanelShowing = 2,
    };

    WebOSInputContext();
    ~WebOSInputContext() override;

    bool isValid() const override;

    void reset() override;
    void commit() override;
    void update(Qt::InputMethodQueries queries) override;
    void invokeAction(QInputMethod::Action action, int cursorPosition) override;
    QRectF keyboardRect() const override;

    bool isAnimating() const override;

    void showInputPanel() override;
    void hideInputPanel() override;
    bool isInputPanelVisible() const override;

    QLocale locale() const override;
    Qt::LayoutDirection inputDirection() const override;

    void setFocusObject(QObject *object) override;

    // Wayland listener trampolines
    static void registryGlobalAdded(
        void *data, struct wl_registry *registry, uint32_t id, const char *interface, uint32_t version);
    static void registryGlobalRemoved(void *data, struct wl_registry *registry, uint32_t name);

    static void textModelCommitString(void *data, struct text_model *textModel, uint32_t serial, const char *text);
    static void textModelPreEditString(
        void *data, struct text_model *textModel, uint32_t serial, const char *text, const char *commit);
    static void textModelDeleteSurroundingText(
        void *data, struct text_model *textModel, uint32_t serial, int32_t index, uint32_t length);
    static void textModelCursorPosition(
        void *data, struct text_model *textModel, uint32_t serial, int32_t index, int32_t anchor);
    static void textModelPreEditStyling(
        void *data, struct text_model *textModel, uint32_t serial, uint32_t index, uint32_t length, uint32_t style);
    static void textModelPreEditCursor(void *data, struct text_model *textModel, uint32_t serial, int32_t index);
    static void textModelModifiersMap(void *data, struct text_model *textModel, struct wl_array *map);
    static void textModelKeySym(void *data, struct text_model *textModel, uint32_t serial, uint32_t time, uint32_t sym,
        uint32_t state, uint32_t modifiers);
    static void textModelEnter(void *data, struct text_model *textModel, struct wl_surface *surface);
    static void textModelLeave(void *data, struct text_model *textModel);
    static void textModelInputPanelState(void *data, struct text_model *textModel, uint32_t state);
    static void textModelInputPanelRect(
        void *data, struct text_model *textModel, int32_t x, int32_t y, uint32_t width, uint32_t height);

private Q_SLOTS:
    void focusObjectDestroyed(QObject *object);

private:
    void cleanup();
    void ensureWaylandConnection();
    void updateSurroundingText(const QVariant& text, const QVariant& cursor, const QVariant& anchor);
    void resetPreEditData();
    void commitAndReset(bool keepCursorPosition = false);

    static const struct wl_registry_listener registryListener;
    static const struct text_model_listener textModelListener;

    QObject *m_focusObject = nullptr;
    wl_display *m_display = nullptr;
    wl_registry *m_registry = nullptr;
    wl_seat *m_seat = nullptr;
    text_model_factory *m_textModelFactory = nullptr;
    text_model *m_currentTextModel = nullptr;

    Qt::InputMethodQueries m_pendingQueries = Qt::ImEnabled;
    bool m_isQueryPending = false;
    bool m_isCleanupPending = false;
    bool m_isActivationPending = false;
    bool m_modelActivated = false;
    PreEditData m_preEditData;
    XkbQtModifiersMap m_modifiersMap;
    QRectF m_keyboardRect;

    InputPanelState m_inputPanelState = InputPanelUnknownState;
};

} // namespace Spool
