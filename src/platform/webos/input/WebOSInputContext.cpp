// Ported to Qt 6 from qtwayland-webos waylandinputcontext.cpp
// (Copyright (c) 2013-2021 LG Electronics, Inc., Apache-2.0).
#include "WebOSInputContext.h"

#include <qpa/qplatformnativeinterface.h>
#include <qpa/qwindowsysteminterface.h>

#include <QBrush>
#include <QColor>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QLoggingCategory>
#include <QTextCharFormat>
#include <QVariant>

#include <climits>

namespace Spool {

Q_STATIC_LOGGING_CATEGORY(lcOsk, "spool.osk")

const struct wl_registry_listener WebOSInputContext::registryListener = {
    WebOSInputContext::registryGlobalAdded,
    WebOSInputContext::registryGlobalRemoved,
};

const struct text_model_listener WebOSInputContext::textModelListener = {
    WebOSInputContext::textModelCommitString,
    WebOSInputContext::textModelPreEditString,
    WebOSInputContext::textModelDeleteSurroundingText,
    WebOSInputContext::textModelCursorPosition,
    WebOSInputContext::textModelPreEditStyling,
    WebOSInputContext::textModelPreEditCursor,
    WebOSInputContext::textModelModifiersMap,
    WebOSInputContext::textModelKeySym,
    WebOSInputContext::textModelEnter,
    WebOSInputContext::textModelLeave,
    WebOSInputContext::textModelInputPanelState,
    WebOSInputContext::textModelInputPanelRect,
};

static uint32_t contentHintFromQtHints(Qt::InputMethodHints hints)
{
    // Qt assumes these are always desired; masked out below when the field
    // opts out explicitly.
    uint32_t wlHint = TEXT_MODEL_CONTENT_HINT_AUTO_COMPLETION | TEXT_MODEL_CONTENT_HINT_AUTO_CAPITALIZATION;

    if (hints & Qt::ImhHiddenText)
        wlHint |= TEXT_MODEL_CONTENT_HINT_PASSWORD;
    if (hints & Qt::ImhSensitiveData)
        wlHint |= TEXT_MODEL_CONTENT_HINT_SENSITIVE_DATA;
    if (hints & Qt::ImhNoAutoUppercase)
        wlHint &= ~TEXT_MODEL_CONTENT_HINT_AUTO_CAPITALIZATION;
    if (hints & Qt::ImhPreferUppercase)
        wlHint |= TEXT_MODEL_CONTENT_HINT_UPPERCASE;
    if (hints & Qt::ImhPreferLowercase)
        wlHint |= TEXT_MODEL_CONTENT_HINT_LOWERCASE;
    if (hints & Qt::ImhNoPredictiveText)
        wlHint &= ~TEXT_MODEL_CONTENT_HINT_AUTO_COMPLETION;
    if (hints & Qt::ImhPreferLatin)
        wlHint |= TEXT_MODEL_CONTENT_HINT_LATIN;
    if (hints & Qt::ImhMultiLine)
        wlHint |= TEXT_MODEL_CONTENT_HINT_MULTILINE;

    return wlHint;
}

static uint32_t contentPurposeFromQtHints(Qt::InputMethodHints hints)
{
    Qt::InputMethodHints exclusiveHints = hints & Qt::ImhExclusiveInputMask;
    uint32_t wlPurpose = TEXT_MODEL_CONTENT_PURPOSE_NORMAL;
    switch (exclusiveHints) {
    case Qt::ImhDigitsOnly:
        wlPurpose |= TEXT_MODEL_CONTENT_PURPOSE_DIGITS;
        break;
    case Qt::ImhFormattedNumbersOnly:
        wlPurpose |= TEXT_MODEL_CONTENT_PURPOSE_NUMBER;
        break;
    case Qt::ImhDialableCharactersOnly:
        wlPurpose |= TEXT_MODEL_CONTENT_PURPOSE_PHONE;
        break;
    case Qt::ImhEmailCharactersOnly:
        wlPurpose |= TEXT_MODEL_CONTENT_PURPOSE_EMAIL;
        break;
    case Qt::ImhUrlCharactersOnly:
        wlPurpose |= TEXT_MODEL_CONTENT_PURPOSE_URL;
        break;
    default:
        break;
    }

    if (hints & Qt::ImhPreferNumbers)
        wlPurpose |= TEXT_MODEL_CONTENT_PURPOSE_NUMBER;
    if (hints & Qt::ImhDate)
        wlPurpose |= TEXT_MODEL_CONTENT_PURPOSE_DATE;
    if (hints & Qt::ImhTime)
        wlPurpose |= TEXT_MODEL_CONTENT_PURPOSE_TIME;
    if (hints & Qt::ImhHiddenText)
        wlPurpose |= TEXT_MODEL_CONTENT_PURPOSE_PASSWORD;

    return wlPurpose;
}

static uint32_t enterKeyTypeFromQtEnterKeyType(Qt::EnterKeyType enterType)
{
    switch (enterType) {
    case Qt::EnterKeyDefault:
        return TEXT_MODEL_ENTER_KEY_TYPE_DEFAULT;
    case Qt::EnterKeyReturn:
        return TEXT_MODEL_ENTER_KEY_TYPE_RETURN;
    case Qt::EnterKeyDone:
        return TEXT_MODEL_ENTER_KEY_TYPE_DONE;
    case Qt::EnterKeyGo:
        return TEXT_MODEL_ENTER_KEY_TYPE_GO;
    case Qt::EnterKeySend:
        return TEXT_MODEL_ENTER_KEY_TYPE_SEND;
    case Qt::EnterKeySearch:
        return TEXT_MODEL_ENTER_KEY_TYPE_SEARCH;
    case Qt::EnterKeyNext:
        return TEXT_MODEL_ENTER_KEY_TYPE_NEXT;
    case Qt::EnterKeyPrevious:
        return TEXT_MODEL_ENTER_KEY_TYPE_PREVIOUS;
    }
    return TEXT_MODEL_ENTER_KEY_TYPE_DEFAULT;
}

static QTextCharFormat qtStylingFrom(uint32_t style)
{
    QTextCharFormat format;
    if (style == TEXT_MODEL_PREEDIT_STYLE_HIGHLIGHT)
        format.setBackground(QBrush(QColor(198, 176, 186)));
    return format;
}

static uint32_t s_serial = 0;

WebOSInputContext::WebOSInputContext()
{
    ensureWaylandConnection();
}

WebOSInputContext::~WebOSInputContext()
{
    cleanup();

    if (m_textModelFactory)
        text_model_factory_destroy(m_textModelFactory);
    if (m_seat)
        wl_seat_destroy(m_seat);
    if (m_registry)
        wl_registry_destroy(m_registry);
}

bool WebOSInputContext::isValid() const
{
    return true;
}

void WebOSInputContext::reset()
{
    commitAndReset();
}

void WebOSInputContext::commit()
{
    if (!m_currentTextModel)
        return;

    QInputMethodEvent event;
    event.setCommitString(m_preEditData.preEdit);
    resetPreEditData();
    if (m_focusObject)
        QGuiApplication::sendEvent(m_focusObject, &event);
    text_model_commit(m_currentTextModel);
    text_model_reset(m_currentTextModel, s_serial);
}

void WebOSInputContext::update(Qt::InputMethodQueries queries)
{
    if (!m_focusObject)
        return;

    m_pendingQueries |= queries;
    if (!m_modelActivated || !m_currentTextModel) {
        m_isQueryPending = true;
        return;
    }

    queries |= m_pendingQueries;
    QInputMethodQueryEvent query(queries);
    QCoreApplication::sendEvent(m_focusObject, &query);

    if (queries & Qt::ImHints) {
        Qt::InputMethodHints hints = Qt::InputMethodHints(query.value(Qt::ImHints).toUInt());
        text_model_set_content_type(
            m_currentTextModel, contentHintFromQtHints(hints), contentPurposeFromQtHints(hints));
    }

    if (queries & Qt::ImEnterKeyType) {
        Qt::EnterKeyType enterKeyType = Qt::EnterKeyType(query.value(Qt::ImEnterKeyType).toUInt());
        text_model_set_enter_key_type(m_currentTextModel, enterKeyTypeFromQtEnterKeyType(enterKeyType));
    }

    if (queries & Qt::ImMaximumTextLength) {
        const uint32_t maxLength = query.value(Qt::ImMaximumTextLength).toUInt();
        if (maxLength > 0)
            text_model_set_max_text_length(m_currentTextModel, maxLength);
    }

    if (queries & Qt::ImPlatformData)
        text_model_set_platform_data(
            m_currentTextModel, query.value(Qt::ImPlatformData).toString().toUtf8().constData());

    if (queries & (Qt::ImSurroundingText | Qt::ImAnchorPosition | Qt::ImCursorPosition))
        updateSurroundingText(
            query.value(Qt::ImSurroundingText), query.value(Qt::ImCursorPosition), query.value(Qt::ImAnchorPosition));

    m_pendingQueries = Qt::ImEnabled;
    m_isQueryPending = false;
}

void WebOSInputContext::resetPreEditData()
{
    m_preEditData.formats.clear();
    m_preEditData.preEdit.clear();
    m_preEditData.cursor = 0;
}

void WebOSInputContext::commitAndReset(bool keepCursorPosition)
{
    const bool inPreEdit = !m_preEditData.preEdit.isEmpty();
    if (!inPreEdit || !inputMethodAccepted() || !m_focusObject)
        return;

    QList<QInputMethodEvent::Attribute> attrs;
    if (keepCursorPosition) {
        // Move the cursor back to the original position after committing the
        // preedit data.
        QInputMethodQueryEvent query(Qt::ImCursorPosition);
        QCoreApplication::sendEvent(m_focusObject, &query);
        attrs.append(QInputMethodEvent::Attribute(
            QInputMethodEvent::Selection, query.value(Qt::ImCursorPosition).toInt(), 0, QVariant()));
    }
    QInputMethodEvent event(QString(), attrs);
    event.setCommitString(m_preEditData.preEdit);

    QGuiApplication::sendEvent(m_focusObject, &event);

    resetPreEditData();
    if (m_currentTextModel)
        text_model_reset(m_currentTextModel, s_serial);
}

void WebOSInputContext::updateSurroundingText(const QVariant& text, const QVariant& cursor, const QVariant& anchor)
{
    if (!m_currentTextModel)
        return;

    // Wayland messages are limited to a few KB; keep the surrounding text the
    // IME sees to 200 characters around the cursor, like the LG client does.
    static const int kSurroundingTextMax = 200;
    QString surroundingText;
    int cursorPosition = cursor.toInt();
    int anchorPosition = anchor.toInt();

    if (kSurroundingTextMax < cursorPosition) {
        const int shifted = cursorPosition - kSurroundingTextMax;
        surroundingText = text.toString().left(cursorPosition).right(kSurroundingTextMax);

        if (anchorPosition < shifted)
            anchorPosition = 0;
        else if (anchorPosition < cursorPosition)
            anchorPosition -= shifted;
        else
            anchorPosition = kSurroundingTextMax;

        cursorPosition = kSurroundingTextMax;
    } else {
        surroundingText = text.toString().left(kSurroundingTextMax);
        if (anchorPosition > kSurroundingTextMax)
            anchorPosition = kSurroundingTextMax;
    }

    text_model_set_surrounding_text(m_currentTextModel, surroundingText.toUtf8().constData(),
        static_cast<uint32_t>(cursorPosition), static_cast<uint32_t>(anchorPosition));
}

void WebOSInputContext::invokeAction(QInputMethod::Action action, int cursorPosition)
{
    if (!inputMethodAccepted() || !m_currentTextModel)
        return;

    if (action == QInputMethod::Click) {
        if (cursorPosition <= 0 || cursorPosition >= m_preEditData.preEdit.length()) {
            commitAndReset(cursorPosition == 0);
            return;
        }
        text_model_invoke_action(m_currentTextModel, 0, static_cast<uint32_t>(cursorPosition));
    } else {
        QPlatformInputContext::invokeAction(action, cursorPosition);
    }
}

QRectF WebOSInputContext::keyboardRect() const
{
    return m_keyboardRect;
}

bool WebOSInputContext::isAnimating() const
{
    return false;
}

void WebOSInputContext::showInputPanel()
{
    if (!inputMethodAccepted())
        return;

    if (!m_seat || !m_textModelFactory) {
        qCWarning(lcOsk) << "cannot show input panel: seat" << m_seat << "factory" << m_textModelFactory;
        return;
    }

    if (m_isCleanupPending)
        cleanup();

    if (m_modelActivated) {
        // Executes pending queries as well.
        update(Qt::ImHints | Qt::ImSurroundingText | Qt::ImAnchorPosition | Qt::ImCursorPosition | Qt::ImEnterKeyType
            | Qt::ImMaximumTextLength | Qt::ImPlatformData);
        return;
    }

    if (m_isActivationPending) {
        qCDebug(lcOsk) << "text model activation already requested";
        return;
    }

    QPlatformNativeInterface *nativeInterface = QGuiApplication::platformNativeInterface();
    if (!nativeInterface) {
        qCWarning(lcOsk) << "no native interface available";
        return;
    }
    auto *surface = static_cast<wl_surface *>(
        nativeInterface->nativeResourceForWindow("surface", QGuiApplication::focusWindow()));
    if (!surface) {
        qCWarning(lcOsk) << "no wl_surface for focus window" << QGuiApplication::focusWindow();
        return;
    }

    m_currentTextModel = text_model_factory_create_text_model(m_textModelFactory);
    text_model_add_listener(m_currentTextModel, &textModelListener, this);
    text_model_activate(m_currentTextModel, s_serial++, m_seat, surface);
    m_isActivationPending = true;
    qCDebug(lcOsk) << "requested text model activation";
}

void WebOSInputContext::hideInputPanel()
{
    if (!m_currentTextModel)
        return;

    // Finish editing surrounding text before the input panel hides.
    commitAndReset();

    cleanup();
    m_inputPanelState = InputPanelUnknownState;
    emitInputPanelVisibleChanged();
}

bool WebOSInputContext::isInputPanelVisible() const
{
    return m_currentTextModel && (m_inputPanelState == InputPanelShown || m_inputPanelState == InputPanelShowing);
}

QLocale WebOSInputContext::locale() const
{
    return QLocale::system();
}

Qt::LayoutDirection WebOSInputContext::inputDirection() const
{
    return Qt::LeftToRight;
}

void WebOSInputContext::setFocusObject(QObject *object)
{
    if (m_focusObject == object)
        return;

    if (m_focusObject)
        disconnect(m_focusObject, &QObject::destroyed, this, &WebOSInputContext::focusObjectDestroyed);

    m_focusObject = object;

    if (m_focusObject)
        connect(m_focusObject, &QObject::destroyed, this, &WebOSInputContext::focusObjectDestroyed);

    if (!m_textModelFactory || !m_display)
        return;

    if (inputMethodAccepted() && qGuiApp->focusWindow())
        showInputPanel();
    else
        hideInputPanel();
}

void WebOSInputContext::focusObjectDestroyed(QObject *object)
{
    if (m_focusObject == object)
        m_focusObject = nullptr;
}

void WebOSInputContext::ensureWaylandConnection()
{
    if (m_display)
        return;

    QPlatformNativeInterface *nativeInterface = QGuiApplication::platformNativeInterface();
    if (nativeInterface)
        m_display = static_cast<wl_display *>(nativeInterface->nativeResourceForIntegration("display"));
    if (!m_display) {
        qCWarning(lcOsk) << "no wayland display available";
        return;
    }

    m_registry = wl_display_get_registry(m_display);
    wl_registry_add_listener(m_registry, &registryListener, this);
}

void WebOSInputContext::registryGlobalAdded(
    void *data, struct wl_registry *registry, uint32_t id, const char *interface, uint32_t version)
{
    Q_UNUSED(registry);
    Q_UNUSED(version);

    auto *that = static_cast<WebOSInputContext *>(data);
    const QByteArray interfaceName(interface);
    if (interfaceName == "text_model_factory") {
        that->m_textModelFactory = static_cast<text_model_factory *>(
            wl_registry_bind(that->m_registry, id, &text_model_factory_interface, 1));
        qCDebug(lcOsk) << "bound text_model_factory";
    } else if (interfaceName == "wl_seat" && !that->m_seat) {
        that->m_seat = static_cast<wl_seat *>(wl_registry_bind(that->m_registry, id, &wl_seat_interface, 1));
    }
}

void WebOSInputContext::registryGlobalRemoved(void *data, struct wl_registry *registry, uint32_t name)
{
    Q_UNUSED(data);
    Q_UNUSED(registry);
    Q_UNUSED(name);
}

void WebOSInputContext::textModelCommitString(
    void *data, struct text_model *textModel, uint32_t serial, const char *text)
{
    Q_UNUSED(textModel);
    Q_UNUSED(serial);

    auto *that = static_cast<WebOSInputContext *>(data);
    if (!that->m_focusObject)
        return;
    auto *event = new QInputMethodEvent();
    event->setCommitString(QString::fromUtf8(text), 0, 0);
    QCoreApplication::postEvent(that->m_focusObject, event);
    that->resetPreEditData();
}

void WebOSInputContext::textModelPreEditString(
    void *data, struct text_model *textModel, uint32_t serial, const char *text, const char *commit)
{
    Q_UNUSED(textModel);
    Q_UNUSED(serial);
    Q_UNUSED(commit);

    auto *that = static_cast<WebOSInputContext *>(data);
    if (!that->m_focusObject)
        return;
    that->m_preEditData.preEdit = QString::fromUtf8(text);
    that->m_preEditData.formats << QInputMethodEvent::Attribute(
        QInputMethodEvent::Cursor, that->m_preEditData.preEdit.length(), 1, QVariant());
    auto *event = new QInputMethodEvent(that->m_preEditData.preEdit, that->m_preEditData.formats);
    QCoreApplication::postEvent(that->m_focusObject, event);
}

void WebOSInputContext::textModelDeleteSurroundingText(
    void *data, struct text_model *textModel, uint32_t serial, int32_t index, uint32_t length)
{
    Q_UNUSED(textModel);
    Q_UNUSED(serial);

    auto *that = static_cast<WebOSInputContext *>(data);
    if (!that->m_focusObject)
        return;
    auto *event = new QInputMethodEvent(QString(), {});

    if (length == UINT_MAX) {
        // The IME requests the whole field to be cleared.
        QInputMethodQueryEvent query(Qt::ImSurroundingText | Qt::ImCursorPosition);
        QCoreApplication::sendEvent(that->m_focusObject, &query);
        event->setCommitString(QString(), -query.value(Qt::ImCursorPosition).toInt(),
            query.value(Qt::ImSurroundingText).toString().length());
    } else {
        event->setCommitString(QString(), index, static_cast<int>(length));
    }

    QCoreApplication::postEvent(that->m_focusObject, event);
}

void WebOSInputContext::textModelCursorPosition(
    void *data, struct text_model *textModel, uint32_t serial, int32_t index, int32_t anchor)
{
    Q_UNUSED(data);
    Q_UNUSED(textModel);
    Q_UNUSED(serial);
    Q_UNUSED(index);
    Q_UNUSED(anchor);
}

void WebOSInputContext::textModelPreEditStyling(
    void *data, struct text_model *textModel, uint32_t serial, uint32_t index, uint32_t length, uint32_t style)
{
    Q_UNUSED(textModel);
    Q_UNUSED(serial);

    auto *that = static_cast<WebOSInputContext *>(data);
    that->m_preEditData.formats << QInputMethodEvent::Attribute(
        QInputMethodEvent::TextFormat, static_cast<int>(index), static_cast<int>(length), qtStylingFrom(style));
}

void WebOSInputContext::textModelPreEditCursor(void *data, struct text_model *textModel, uint32_t serial, int32_t index)
{
    Q_UNUSED(textModel);
    Q_UNUSED(serial);

    auto *that = static_cast<WebOSInputContext *>(data);
    that->m_preEditData.cursor = index;
}

void WebOSInputContext::textModelModifiersMap(void *data, struct text_model *textModel, struct wl_array *map)
{
    Q_UNUSED(textModel);

    auto *that = static_cast<WebOSInputContext *>(data);
    that->m_modifiersMap.applyWaylandModifiersMap(map);
}

void WebOSInputContext::textModelKeySym(void *data, struct text_model *textModel, uint32_t serial, uint32_t time,
    uint32_t sym, uint32_t state, uint32_t modifiers)
{
    Q_UNUSED(textModel);
    Q_UNUSED(serial);

    auto *that = static_cast<WebOSInputContext *>(data);

    const QEvent::Type type = (state != 0) ? QEvent::KeyPress : QEvent::KeyRelease;
    int qtKey = xkbKeyToQtKey(sym);
    // The LG keymap system reports webOS remote keys with the Qt key code in
    // the keysym field; keys unknown to the xkb table pass through verbatim.
    if (qtKey == Qt::Key_unknown)
        qtKey = static_cast<int>(sym);

    Qt::KeyboardModifiers qtModifiers = that->m_modifiersMap.convertNativeModifiersToQt(modifiers);
    if (isKeypadKey(sym))
        qtModifiers |= Qt::KeypadModifier;

    QString text;
    switch (qtKey) {
    case Qt::Key_Enter:
    case Qt::Key_Return:
        text = QStringLiteral("\r");
        break;
    case Qt::Key_Left:
    case Qt::Key_Right:
        // Make sure the preedit string is committed before moving the cursor.
        that->commitAndReset(qtKey == Qt::Key_Right);
        break;
    default:
        break;
    }

    QWindowSystemInterface::handleExtendedKeyEvent(
        qGuiApp->focusWindow(), time, type, qtKey, qtModifiers, static_cast<quint32>(qtKey), 0, 0, text);
}

void WebOSInputContext::textModelEnter(void *data, struct text_model *textModel, struct wl_surface *surface)
{
    Q_UNUSED(textModel);
    Q_UNUSED(surface);

    auto *that = static_cast<WebOSInputContext *>(data);
    that->m_modelActivated = true;
    that->m_isActivationPending = false;

    that->m_inputPanelState = InputPanelShowing;
    that->emitInputPanelVisibleChanged();
    qCDebug(lcOsk) << "text model activated";

    // Executes pending queries as well.
    that->update(Qt::ImHints | Qt::ImSurroundingText | Qt::ImAnchorPosition | Qt::ImCursorPosition | Qt::ImEnterKeyType
        | Qt::ImMaximumTextLength | Qt::ImPlatformData);
}

void WebOSInputContext::textModelLeave(void *data, struct text_model *textModel)
{
    Q_UNUSED(textModel);

    auto *that = static_cast<WebOSInputContext *>(data);
    qCDebug(lcOsk) << "text model left";
    that->hideInputPanel();
}

void WebOSInputContext::textModelInputPanelState(void *data, struct text_model *textModel, uint32_t state)
{
    Q_UNUSED(textModel);

    auto *that = static_cast<WebOSInputContext *>(data);
    qCDebug(lcOsk) << "input panel state" << state;

    if (that->m_inputPanelState == static_cast<InputPanelState>(state))
        return;

    // A model hidden by the compositor cannot be re-shown; destroy it so the
    // next show request activates a fresh one.
    if (state == InputPanelHidden && that->m_inputPanelState != InputPanelShowing)
        that->m_isCleanupPending = true;
    that->m_inputPanelState = static_cast<InputPanelState>(state);
    that->emitInputPanelVisibleChanged();
}

void WebOSInputContext::textModelInputPanelRect(
    void *data, struct text_model *textModel, int32_t x, int32_t y, uint32_t width, uint32_t height)
{
    Q_UNUSED(textModel);

    auto *that = static_cast<WebOSInputContext *>(data);
    const QRectF newRect(x, y, width, height);
    if (that->m_keyboardRect != newRect) {
        that->m_keyboardRect = newRect;
        qCDebug(lcOsk) << "keyboard rect" << newRect;
        that->emitKeyboardRectChanged();
    }
}

void WebOSInputContext::cleanup()
{
    if (m_currentTextModel) {
        text_model_deactivate(m_currentTextModel, m_seat);
        text_model_destroy(m_currentTextModel);
        m_currentTextModel = nullptr;
    }

    m_pendingQueries = Qt::ImEnabled;
    m_isQueryPending = false;
    m_isCleanupPending = false;
    m_isActivationPending = false;
    m_modelActivated = false;
}

} // namespace Spool
