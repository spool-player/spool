// Keysym and modifier translation for the webOS text_model keysym event.
// Adapted from qtwayland-webos keysymhelper.h (Copyright (c) 2013-2023 LG
// Electronics, Inc., Apache-2.0). The LG Qt fork mapped XKB_KEY_XF86Back to
// its private Qt::Key_webOS_Back extension; stock Qt has no such key, so we
// map it to Qt::Key_Back which the QML input helpers already treat as Back.
#pragma once

#include <QKeyEvent>
#include <QMap>

#include <cstring>

#include <wayland-client-core.h>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace Spool {

struct XkbQtKey {
    uint32_t xkbkey;
    int qtkey;
};

static const struct XkbQtKey g_xkbQtKeyMap[] = {
    { XKB_KEY_Shift_L, Qt::Key_Shift },
    { XKB_KEY_Control_L, Qt::Key_Control },
    { XKB_KEY_Super_L, Qt::Key_Super_L },
    { XKB_KEY_Alt_L, Qt::Key_Alt },

    { XKB_KEY_Shift_R, Qt::Key_Shift },
    { XKB_KEY_Control_R, Qt::Key_Control },
    { XKB_KEY_Super_R, Qt::Key_Super_R },
    { XKB_KEY_Alt_R, Qt::Key_Alt },

    { XKB_KEY_Menu, Qt::Key_Menu },
    { XKB_KEY_Escape, Qt::Key_Escape },

    { XKB_KEY_BackSpace, Qt::Key_Backspace },
    { XKB_KEY_Return, Qt::Key_Return },
    { XKB_KEY_Tab, Qt::Key_Tab },
    { XKB_KEY_Caps_Lock, Qt::Key_CapsLock },

    { XKB_KEY_F1, Qt::Key_F1 },
    { XKB_KEY_F2, Qt::Key_F2 },
    { XKB_KEY_F3, Qt::Key_F3 },
    { XKB_KEY_F4, Qt::Key_F4 },
    { XKB_KEY_F5, Qt::Key_F5 },
    { XKB_KEY_F6, Qt::Key_F6 },
    { XKB_KEY_F7, Qt::Key_F7 },
    { XKB_KEY_F8, Qt::Key_F8 },
    { XKB_KEY_F9, Qt::Key_F9 },
    { XKB_KEY_F10, Qt::Key_F10 },
    { XKB_KEY_F11, Qt::Key_F11 },
    { XKB_KEY_F12, Qt::Key_F12 },

    { XKB_KEY_Print, Qt::Key_Print },
    { XKB_KEY_Pause, Qt::Key_Pause },
    { XKB_KEY_Scroll_Lock, Qt::Key_ScrollLock },

    { XKB_KEY_Insert, Qt::Key_Insert },
    { XKB_KEY_Delete, Qt::Key_Delete },
    { XKB_KEY_Home, Qt::Key_Home },
    { XKB_KEY_End, Qt::Key_End },
    { XKB_KEY_Prior, Qt::Key_PageUp },
    { XKB_KEY_Next, Qt::Key_PageDown },

    { XKB_KEY_Up, Qt::Key_Up },
    { XKB_KEY_Left, Qt::Key_Left },
    { XKB_KEY_Down, Qt::Key_Down },
    { XKB_KEY_Right, Qt::Key_Right },

    { XKB_KEY_Num_Lock, Qt::Key_NumLock },

    { XKB_KEY_XF86Back, Qt::Key_Back },
};

static const struct XkbQtKey g_xkbQtKeypadMap[] = {
    { XKB_KEY_KP_Divide, Qt::Key_Slash },
    { XKB_KEY_KP_Multiply, Qt::Key_Asterisk },
    { XKB_KEY_KP_Subtract, Qt::Key_Minus },
    { XKB_KEY_KP_Add, Qt::Key_Plus },

    { XKB_KEY_KP_Home, Qt::Key_Home },
    { XKB_KEY_KP_Up, Qt::Key_Up },
    { XKB_KEY_KP_Prior, Qt::Key_PageUp },
    { XKB_KEY_KP_Left, Qt::Key_Left },
    { XKB_KEY_KP_Begin, Qt::Key_Clear },
    { XKB_KEY_KP_Right, Qt::Key_Right },
    { XKB_KEY_KP_End, Qt::Key_End },
    { XKB_KEY_KP_Down, Qt::Key_Down },
    { XKB_KEY_KP_Next, Qt::Key_PageDown },

    { XKB_KEY_KP_Insert, Qt::Key_Insert },
    { XKB_KEY_KP_Delete, Qt::Key_Delete },
    { XKB_KEY_KP_Enter, Qt::Key_Enter },
    { XKB_KEY_KP_Decimal, Qt::Key_Period },

    { XKB_KEY_KP_0, Qt::Key_0 },
    { XKB_KEY_KP_1, Qt::Key_1 },
    { XKB_KEY_KP_2, Qt::Key_2 },
    { XKB_KEY_KP_3, Qt::Key_3 },
    { XKB_KEY_KP_4, Qt::Key_4 },
    { XKB_KEY_KP_5, Qt::Key_5 },
    { XKB_KEY_KP_6, Qt::Key_6 },
    { XKB_KEY_KP_7, Qt::Key_7 },
    { XKB_KEY_KP_8, Qt::Key_8 },
    { XKB_KEY_KP_9, Qt::Key_9 },
};

static const struct XkbQtKey g_xkbQtMediaMap[] = {
    { XKB_KEY_Cancel, Qt::Key_MediaStop }, // keymap for NRCU Stop button

    { XKB_KEY_XF86AudioPlay, Qt::Key_MediaPlay },
    { XKB_KEY_XF86AudioStop, Qt::Key_MediaStop },
    { XKB_KEY_XF86AudioPrev, Qt::Key_MediaPrevious },
    { XKB_KEY_XF86AudioNext, Qt::Key_MediaNext },

    { XKB_KEY_XF86AudioRecord, Qt::Key_MediaRecord },
    { XKB_KEY_XF86AudioRewind, Qt::Key_AudioRewind },
    { XKB_KEY_XF86AudioForward, Qt::Key_AudioForward },
};

inline Qt::Key xkbKeyToQtKey(uint32_t xkbkey)
{
    for (const auto& entry : g_xkbQtKeyMap) {
        if (xkbkey == entry.xkbkey)
            return static_cast<Qt::Key>(entry.qtkey);
    }
    for (const auto& entry : g_xkbQtKeypadMap) {
        if (xkbkey == entry.xkbkey)
            return static_cast<Qt::Key>(entry.qtkey);
    }
    for (const auto& entry : g_xkbQtMediaMap) {
        if (xkbkey == entry.xkbkey)
            return static_cast<Qt::Key>(entry.qtkey);
    }
    return Qt::Key_unknown;
}

inline bool isKeypadKey(uint32_t xkbkey)
{
    for (const auto& entry : g_xkbQtKeypadMap) {
        if (xkbkey == entry.xkbkey)
            return true;
    }
    return false;
}

inline Qt::KeyboardModifier qtModifierByXkbName(const char *name)
{
    // XKB_MOD_NAME_* from xkbcommon-names.h, inlined to keep includes small.
    if (std::strcmp(name, "Shift") == 0)
        return Qt::ShiftModifier;
    if (std::strcmp(name, "Control") == 0)
        return Qt::ControlModifier;
    if (std::strcmp(name, "Mod1") == 0)
        return Qt::AltModifier;
    return Qt::NoModifier;
}

// Maps the IME-provided modifier-name array (modifiers_map event) onto Qt
// modifier flags for decoding the bitmask in subsequent keysym events.
class XkbQtModifiersMap {
public:
    void applyWaylandModifiersMap(struct wl_array *map)
    {
        m_map.clear();

        int index = 0;
        const char *p = static_cast<const char *>(map->data);
        const char *end = p + map->size;
        while (p < end) {
            Qt::KeyboardModifier qtModifier = qtModifierByXkbName(p);
            if (qtModifier != Qt::NoModifier)
                m_map.insert(index, qtModifier);

            ++index;
            p += std::strlen(p) + 1;
        }
    }

    Qt::KeyboardModifiers convertNativeModifiersToQt(uint32_t nativeModifiers) const
    {
        Qt::KeyboardModifiers qtModifiers = Qt::NoModifier;
        for (auto it = m_map.cbegin(); it != m_map.cend(); ++it) {
            if (nativeModifiers & (1u << it.key()))
                qtModifiers |= it.value();
        }
        return qtModifiers;
    }

private:
    QMap<int, Qt::KeyboardModifier> m_map;
};

} // namespace Spool
