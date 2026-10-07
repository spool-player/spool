#include "player/MpvVideoItem.h"

#include "platform/PlatformDisplayOutput.h"
#include "player/MpvOptionProfile.h"
#include "player/RenderTargetProfile.h"

#include "TestMain.h"

#include <QDir>
#include <QEventLoop>
#include <QGuiApplication>
#include <QImage>
#include <QQuickWindow>
#include <QSGTexture>
#include <QSGTextureProvider>
#include <QSurfaceFormat>
#include <QTemporaryFile>
#include <QTimer>
#ifdef Q_OS_TVOS
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QPointer>
#include <QRunnable>
#endif
#include <algorithm>
#include <atomic>
#if SPOOL_MPV_ITEM_RHI
#include <rhi/qrhi.h>
#endif

#include <clocale>
#include <cstdio>

extern "C" {
#include <mpv/client.h>
}

namespace {

bool writeVideo(QTemporaryFile& file)
{
    // A red top half over a blue bottom half, so the grab below can tell which
    // way up the frame arrived. A uniform frame cannot: the scene graph samples
    // the item's texture with a different origin convention than an OpenGL
    // framebuffer writes it, and getting that wrong is invisible until the
    // picture is upside down.
    // This is 30 seconds at 10 fps, not a two-frame EOF/seek loop that resets
    // video output five times a second while native GPU targets initialize.
    // Store the finite FFV1 clip in qCompress format without a heap input copy.
    static constexpr unsigned char encoded[] =
        "\x00\x00\x8d\xc4\x78\xda\xed\x98\x7b\x54\x4d\xdb\x1e\xc7\xd7\x2e\x8f\x88\x1e\x8a\x92\x48\x4e\x0e"
        "\xe7\x70\x6b\x27\x79\x1b\xb2\xed\x9c\x50\x94\x9d\xd7\xe1\x9c\x4a\x54\x57\xca\x23\x8f\x74\x06\x59"
        "\xbb\xad\xd7\xc9\x38\x21\x8f\x48\x45\x1e\x17\x21\xa5\x6e\xa4\xe7\xad\x23\x79\x1c\xe1\xea\xa1\x54"
        "\x42\x8e\x47\x0f\x95\x52\x7b\xad\x79\xe7\xdc\x7b\xac\xa5\xf3\xcf\xfd\xeb\x6a\xdd\x3f\x7e\x6b\xd4"
        "\x1a\x63\xf5\xfd\xad\xf9\x99\x6b\xae\xdf\x9e\xbb\xf1\x31\xb1\xaf\x49\x4a\x92\x84\xd2\x22\x49\x07"
        "\xfe\xfd\x48\xf7\x91\xb4\xd2\x5a\x12\x79\xb8\x9f\xc7\xb6\xad\x9b\x02\x7c\x3d\x24\x61\xf8\x4f\xfb"
        "\x68\x0d\x63\xd9\x5e\x6f\x11\x45\x8e\xfd\xd1\xfa\x4e\x27\xb6\xe5\xe5\x2a\x66\xee\x71\xf3\x72\xca"
        "\xfa\x55\x76\x49\x31\x74\xc1\x05\x2f\x59\x32\x7d\x4a\x7d\x35\xcc\xf5\x8a\x2f\xbe\x6a\x72\xca\x8a"
        "\xc2\x57\x43\x5c\xff\xe5\x2d\x4b\x96\x6b\x0c\x55\x5f\x9a\xca\xb2\x70\x28\xdf\xbf\xed\xbd\x7a\x34"
        "\x4a\x46\xfd\xef\x0f\x32\x9f\x3b\xb9\x8a\x29\xeb\x17\x9a\x7d\x5f\x76\x2d\x44\x57\x62\xe7\xb4\x37"
        "\xca\xd1\x63\x87\xd7\x14\x1b\x4b\x6b\x4b\x6b\xb1\xf5\x8a\xb9\x7f\xb9\x0c\x38\x13\xed\x3e\x72\xc0"
        "\xa1\xce\x26\x93\xd0\x66\xf7\xf6\x01\xad\x1e\x2f\x8d\xa5\x11\xe1\x76\xd5\x8e\xaa\xd1\xc8\xf3\xcc"
        "\x1d\x9d\xab\x18\xba\x65\x63\xec\x15\xf5\xac\x45\xfa\x65\xb4\x28\xa0\x28\x7c\xb7\xe7\x87\xf2\xb5"
        "\xe2\x4f\xd3\xe2\x68\x6a\x4c\x7a\x5c\xc8\x76\xff\x75\xe1\x34\x15\x1a\xba\xdc\x6d\xfe\xfc\xe5\xd6"
        "\x21\xb4\xe8\x9b\xfa\x10\x45\xdf\xf6\x3a\xaa\x36\x3a\x85\xb6\xbb\x49\xdb\x1d\xa7\x35\x96\xa5\x28"
        "\x96\xdd\xa0\x45\xcb\x1a\x69\x8a\x5b\x02\x0d\x8a\xf2\x3c\x6d\x97\xb3\xbc\xdf\x28\x2b\x26\x73\x7e"
        "\xd9\xe6\xa3\x62\x51\xa8\xa8\xb1\xce\xf4\x75\xb6\xbd\x53\xfa\xb4\x71\xdb\xca\x2d\xf5\x76\x6f\x2f"
        "\xba\x58\x3f\xdb\x7d\x62\x79\x68\x08\xbb\xfa\x87\x6b\xba\x45\x3f\xb7\xfc\x82\xe8\xc0\xf4\xdd\xf9"
        "\xe6\x63\xcc\x73\x86\xff\xee\x52\x61\xad\xab\x95\x11\x1c\x94\x7a\xa7\xaa\x64\x55\xfc\xed\x6a\xa6"
        "\xde\xa6\xff\xed\xb0\xea\xc6\x6d\x61\x2f\xd6\x17\x76\x2b\x9f\xe4\x99\xde\x1f\xfb\xd2\x67\x61\x99"
        "\x4e\x8d\xc9\x52\xdb\x9c\xe5\xde\xac\x78\x53\xce\x91\x22\x77\x8d\x69\x8e\x96\x46\x05\x36\x4e\xb3"
        "\x3d\x13\x77\x95\x8a\x07\x3a\x3b\x9c\xbc\x79\xdd\xaf\x66\xb4\x63\xfd\xfa\xf3\xa7\xcd\x4f\x36\xc4"
        "\x4e\x1a\x51\x5d\xac\x3b\x21\xe2\x51\xe9\xda\x9c\xfd\xc1\xcf\xa4\x25\xfd\x76\x77\x1b\x04\xb5\x77"
        "\x5a\xec\xcb\x79\x39\xb6\x36\x70\xa7\xb9\xe7\xc7\x79\x41\x6b\x7e\xac\xd4\xb1\xb5\xfa\x44\xde\x33"
        "\x9b\xab\xb8\x2c\xbe\x5a\x1b\x10\x90\xe0\x99\xb7\xd7\xbb\x38\xd6\x3e\x29\xcc\x7e\xf1\xbc\x25\x52"
        "\xfb\xa5\xd2\xb0\xbf\x2e\x78\xc0\x63\xcf\xbc\x5f\x3d\xbf\x2c\xa0\x77\x71\x7c\xcf\xe2\x03\xb8\xd8"
        "\x93\x2b\x1e\xed\xe5\xb5\xc3\xda\xbb\xf8\x94\x7d\x52\xb8\x74\xd9\xd2\xb9\xae\x0b\x96\x2c\x96\x86"
        "\x1d\x12\x8b\x67\xe0\x1f\x1b\xb1\xa5\x98\x3b\x28\xb3\x79\x19\xdb\x57\xe0\xc6\x8c\x0c\xbd\x96\xda"
        "\x40\x53\x49\x76\x31\x34\x45\xed\x7d\x63\x24\xef\x8c\x45\x5f\x8e\xa7\xef\x22\xf0\xb9\xeb\xda\x3d"
        "\x63\x54\x46\x51\xa6\x94\xdf\xdf\xd2\x64\xd3\xa5\x41\x3a\xcd\x3d\x6a\x6e\xc7\xb4\xe3\x73\x67\xe6"
        "\x8b\x16\x54\x4c\x6a\x8c\xd2\xb6\xe8\x44\x4b\x26\x46\xb9\xf5\xa8\xb1\xf0\xc6\x27\xd6\x43\x91\x8f"
        "\xd0\x6f\xa4\x66\xae\x85\xb5\xb9\x11\x5d\xd7\x65\xf6\xa5\x84\x2d\xd3\xc6\x67\xe6\xe6\xca\xec\x60"
        "\xb4\x9e\xd4\x4c\xbe\x31\x63\x7c\x92\x92\xa6\xd6\x51\x3e\x47\xdd\x0d\x8d\xb9\x3a\xe5\x4d\xdf\x21"
        "\xac\xf3\xf8\xb0\x2e\xdc\x6c\xd4\xd0\x9a\x2b\x9f\xa7\x4b\x17\x9c\x4f\xe6\xd2\xee\x73\x87\x22\x99"
        "\xdf\x65\xfd\x3b\x49\x6a\xa2\x29\x79\x10\x2d\xa1\xd8\x8d\x5c\xfa\xc9\x86\x42\x1f\xc6\x59\xe8\xab"
        "\xee\x8d\x8f\xba\xfb\xa3\x11\x7d\x86\xf9\x07\x97\x7e\x1c\xf3\x14\xbd\xda\xa5\xe3\xd4\x41\x52\x7f"
        "\xa3\xcd\xcf\x93\x3a\x69\xaa\x18\xd3\xdd\x5e\xe8\x73\x35\x1f\xee\xfa\xa3\xdb\xd6\xc6\x2c\xfe\xdc"
        "\x50\x39\x6d\x2b\x56\x63\xf8\xda\x93\x5c\xf8\x3a\x30\x0a\xa5\x3e\xa3\x18\x12\x86\x5e\x8e\x9c\x82"
        "\xd9\x37\x52\xb9\xf0\xd1\xd8\x0f\x6c\xe1\xce\x8b\xaa\x3b\xf3\x56\x5f\xad\xc1\xe8\xf8\x01\x5c\x98"
        "\x6b\x5d\xc9\xce\x7d\xfd\xad\x2a\xac\xcf\xdf\x6b\x8c\xc9\xa2\x89\x84\x7c\x41\xc1\x95\x64\x06\x2a"
        "\x99\x8d\x2f\x50\x01\x29\xd1\x2d\x7c\x38\x05\x93\x0d\x3c\xb9\xf0\x44\x5e\xb0\x72\xd5\x22\x74\x49"
        "\x45\xf6\x0d\x2c\xc3\xe4\x40\x47\x2e\xf4\x5e\xe0\xdc\xa1\x6c\x44\xcf\x48\x38\xf9\x70\xbd\x03\x26"
        "\xcf\x75\xe1\xc2\xef\x3e\x07\xb4\x65\xe8\xa1\xfb\x24\x4c\xf0\xdb\x90\x49\xc8\xd1\x84\x1c\xe2\xcf"
        "\x95\xac\xbd\xd4\xdd\x56\xee\xc2\xa6\x91\x92\xf4\x03\xa6\xc3\xa6\x4b\x1d\x6a\x75\xb9\xd0\xea\xe0"
        "\xbd\x96\x6f\x1f\xb1\x76\x24\x9c\xa4\xeb\x1a\x89\xc9\xb2\xd5\xdc\x2b\xbd\x7f\xc7\xe7\x95\xfc\x00"
        "\x72\x26\xe1\xb8\x8c\xc1\x4e\x98\x3c\x78\x01\x17\x2e\x4a\x42\x55\x56\xf9\x68\x28\x09\xe7\x14\x6c"
        "\x97\x24\xb5\xd1\xa2\x36\x42\x76\x6f\xe3\x4a\x12\x33\x51\x99\xbb\x57\x37\x45\x19\x52\xfd\x7f\x96"
        "\x1d\xc4\xe0\xab\x9f\xb8\x4c\xbb\x12\xdd\x91\x5d\xfd\x4c\xb2\x21\xa9\x89\x69\x98\x3b\x9d\x9b\x13"
        "\x13\xa3\x89\xf2\xd3\x4f\xb1\x24\x63\x47\x34\x7e\x34\xa2\x93\xde\xf0\xed\x73\x74\x2a\x4a\xde\x1a"
        "\xce\x90\xcc\xdc\xbe\xd8\x04\x53\x35\x56\x12\xea\x0a\x23\xee\xee\x4b\xe7\xd1\xa5\x3f\xbc\x3f\x92"
        "\x8a\xa8\xae\x90\xb3\x98\x9a\xc0\xdf\xfd\xfa\x38\x3a\xb8\x20\xb8\x91\x64\xc3\x7d\x2a\x3c\x31\x75"
        "\x1a\x9f\x4d\xb4\x41\x31\x3b\xc2\x55\xb3\xed\xd8\x74\x75\x61\x4f\x6a\xd7\x9f\xcf\xd1\x56\xa3\x7b"
        "\xaa\xd9\x4a\xbe\xf1\xd8\x47\xa8\xb7\x08\x75\x13\xdf\xb8\x66\xf3\xd9\xf6\x7f\xa3\x3b\xa4\x42\xb6"
        "\x52\xbc\x0a\x53\x0b\xb8\xec\xf3\xd8\x75\x6c\x44\x3b\xca\x20\x99\x4b\xe8\x88\x64\x4c\xe5\x5b\xba"
        "\xd3\x6f\x32\xcb\x38\x84\xbc\x27\xd9\x6c\x9f\xec\x65\x78\x89\xad\xb9\xac\xbd\xee\x04\x7b\x8a\x75"
        "\x78\x49\xb2\x9f\xd6\x7a\x3d\xc2\x54\xcd\xd1\x84\xba\x9d\x9f\x57\x84\x88\xbd\xd5\x8c\xb2\x48\xc5"
        "\x11\xab\x41\x72\x4c\x2d\xe1\x47\xce\x5d\xce\x3a\x18\xa2\xd3\x24\x3b\xe0\xba\xc6\x0c\x53\xf9\xfb"
        "\x3a\x3e\xdb\xb3\xcf\x6d\xc7\xbe\x21\x99\xed\x1a\xdf\xbe\x98\x3a\x9b\xa7\xce\x98\xc0\xee\xf8\x30"
        "\xad\x86\x64\x8e\x5b\xae\x1d\x23\x54\x05\xa1\xee\xe3\x9f\xe7\x85\x26\xeb\xbb\x03\x9d\x53\xcd\xb9"
        "\x3a\xb3\x89\x34\x14\x3f\x72\xb9\x1b\xd3\xa0\x40\x11\x24\x5b\x59\x13\x92\x80\xa9\x91\x7c\xb6\x5d"
        "\x97\xcd\xf8\x5e\x51\x47\x32\x8b\x3d\x1e\xda\x98\xba\x8a\xcb\x5a\x7f\xf0\x62\x07\x97\x48\x1f\x93"
        "\x6c\x82\x55\xae\x2d\xa1\xbe\x21\xd4\xfd\x3c\x35\xcc\x80\x9d\x57\x80\xe2\x48\x85\xd1\xcc\x5d\x99"
        "\x98\xfa\x9e\x1f\x79\x95\x1f\x73\xf1\x13\xda\x4d\x32\xcb\xf5\x8b\x27\x60\x6a\x0c\xbf\x69\x34\x35"
        "\xb1\x09\x56\x01\xaa\xe7\x29\x79\x4b\x3d\xc5\x54\x1f\x7e\xcb\xf0\x7f\xc5\xbc\xca\x47\xf7\x48\x26"
        "\xd7\x6c\x37\xc7\xd4\x3e\x8e\x84\x7a\x8c\xa7\x8e\x33\x63\xcd\xf5\xd0\x21\x52\xf1\x24\x21\xfe\x1c"
        "\xa6\x7e\xe6\x47\x76\x19\xc9\x38\x67\x90\xdd\xce\x90\x0a\xeb\xeb\xdc\x8a\xa9\x89\xfc\x2a\x76\x5e"
        "\x66\x03\x26\xbe\xac\x54\xad\x83\xf4\xd5\x2e\x4c\x0d\xe2\xb2\x96\xb3\x52\xa6\xe8\xa1\xba\x57\xb4"
        "\xcf\x64\xce\x24\xd4\x14\x42\x4d\xe2\xdf\x5d\xfe\x54\xe6\x9d\x02\x29\x48\x45\xfa\x9c\xb5\x87\xf0"
        "\x66\xa1\xc5\x8f\x9c\x3e\x8d\xd1\x77\x46\x4b\x49\x16\x14\xb2\xba\x18\x53\xaf\xf0\x59\xc5\x7b\xd6"
        "\xd7\x76\x50\x19\xc9\xc4\x1d\xad\x3b\x31\x35\x8a\xcb\x9a\x9f\x9f\x66\xe2\xff\x40\x45\x24\xfb\xf4"
        "\x2e\x76\x3a\xa6\xf6\x35\x24\x54\x7e\xf3\xed\x74\xfc\x89\x29\x29\x45\x41\xa4\x22\x76\x56\xa7\x1f"
        "\xa6\x0e\xe7\x47\x1e\xb9\x45\x59\x51\x88\xec\x49\x46\x15\x99\x1b\x60\x6a\x36\x97\xb5\x25\x0d\x64"
        "\x2d\xc5\x0b\x1f\x91\xac\xda\x29\x9f\x50\x4f\x71\x59\x53\xa3\x1d\xb3\xa9\x0a\xe5\x92\xac\xce\xc4"
        "\x60\x3d\xa1\x06\x12\x6a\x16\xff\xee\x2a\x43\x99\xf0\x79\xc8\x8f\x54\x6c\x28\xcd\x1c\x8a\xa9\x13"
        "\xf8\x91\xc3\xd3\x94\x11\x6b\xd0\x24\x92\x9d\xd0\xf3\xef\x87\xa9\xf7\xf8\x8e\xe9\xfa\x8e\xd5\xb2"
        "\x46\x25\x24\x2b\xcd\x2e\xb4\xc0\xd4\x0c\x2e\x6b\x44\x35\xcc\xcc\x7a\x94\x49\xb2\xe0\xc8\xbe\x3a"
        "\x84\x5a\x45\xa8\x77\x78\xea\xea\x6c\x66\x45\x06\x5a\xa3\x9a\xd7\xc9\x53\xe5\x98\x3a\x87\x1f\xb9"
        "\xa8\x43\x39\x75\x14\x1a\x4e\xb2\x91\xce\xd2\xad\x98\x5a\xd5\xa3\x4f\x99\xb7\x12\xf2\x4d\x89\x47"
        "\x6e\x8f\xfb\x05\x53\x1f\xf0\x54\xcd\x93\x8c\x76\x2b\x4a\x25\x99\xfb\xfd\xc4\x0b\x98\xda\xcf\x8e"
        "\x50\x2b\xf8\x8e\xd9\xfc\x40\xd9\x14\x8f\x24\xa4\x62\x46\xea\xf0\x19\x98\x2a\xe3\x47\xd6\x77\xed"
        "\x7e\xf6\x9c\x55\xed\x7a\x71\x55\x2e\x5d\x98\xfa\xa1\x67\x9f\x16\x2d\x42\xf9\x24\x3b\x3b\x67\x25"
        "\x85\xa9\x0d\x5c\xf6\xbe\xab\x42\x59\xd7\x8f\x7c\x77\xe0\xd5\x9f\x62\x4b\x56\xb8\xdf\x19\x42\xfd"
        "\x93\x7f\x77\x99\xba\xca\xe3\x53\x91\x25\xa9\x58\x7a\xe4\xc5\x75\x4c\xdd\xc0\xf7\x62\xcc\x9e\x6e"
        "\xa9\x1e\x5b\xa1\xa2\x1e\x3d\x28\x8f\x96\x88\x34\x7a\xf6\x69\x9c\x9b\x7a\x77\xb9\xeb\xbd\x5c\x69"
        "\x44\x9f\xd5\xe4\xb2\x77\xb5\x2d\xca\xf3\xa3\xd5\xbb\x4b\xeb\xa8\x25\x2d\x98\xda\x5f\x8b\x50\x95"
        "\x3c\x75\xf4\x6e\xa5\x4b\xa9\x7a\x15\x07\xce\xda\x77\x17\x53\xf9\x1d\xa4\x39\x45\xbb\xab\xc4\x8c"
        "\x7c\x25\x19\x52\x85\x83\xe5\xee\x98\x3a\xac\x67\x9f\x6e\xde\xa1\xde\x2d\xfd\xe3\xd3\x6a\x31\x95"
        "\xff\x37\xe3\xed\x75\xa4\x5c\x37\x03\xc5\x92\xcc\x6d\x73\xdd\x64\x42\xdd\x28\x08\xf5\xa1\x10\x54"
        "\xad\xc9\x82\x50\x8f\x09\x42\x55\x0a\x41\x1d\xe0\x2e\x08\xb5\x50\x08\xea\xc0\xf1\x82\x50\xa3\x04"
        "\xa1\x36\x0b\x41\xd5\x76\x15\x84\x9a\x29\x04\x75\x90\xa9\x20\xd4\xbd\x82\x50\x5f\x0a\x41\x1d\xec"
        "\x20\x08\x35\x59\x08\xaa\x8e\x9e\x20\xd4\x6d\x82\x50\xcb\x85\xa0\xea\xce\x12\x84\x9a\x28\x04\x55"
        "\xaf\x8f\x20\x54\x1f\x41\xa8\xf7\x84\xa0\xea\x8b\x05\xa1\xc6\x08\x42\xed\x14\x82\x3a\x64\x8d\x20"
        "\xd4\x3c\x21\xa8\x06\x16\x82\x50\xc3\x7b\x9d\xaa\xb2\xf8\x51\xb9\x8a\xc3\x05\xf3\xab\x1b\xe4\x06"
        "\xef\xf1\x2c\x28\x4a\x88\x67\x57\x59\xfa\xde\xa7\x16\x0b\x41\x55\x9b\xf9\x5e\xa7\x46\x0b\x42\x6d"
        "\x13\x82\xaa\x36\xf2\xbd\x4e\xbd\x25\x04\x55\x6d\xe4\x7b\x9d\xaa\x10\x84\xfa\x46\x08\xaa\xda\xc8"
        "\xf7\x3a\x35\x45\x08\xaa\xda\xc8\xf7\x3a\x35\x50\x10\x6a\x95\x10\x54\xb5\x91\xef\x75\xea\x19\x41"
        "\x2c\x35\x18\x79\x30\xf2\x60\xe4\xc1\xc8\x83\x91\x07\x23\x0f\x46\x1e\x8c\x3c\x18\x79\x30\xf2\x60"
        "\xe4\xc1\xc8\x83\x91\x07\x23\x0f\x46\xfe\xbf\x19\xf9\x75\xc1\x5a\x95\x0d\xf2\x71\xe5\x60\xe4\xc1"
        "\xc8\x83\x91\x07\x23\x0f\x46\x1e\x8c\x3c\x18\x79\x30\xf2\x60\xe4\xc1\xc8\x83\x91\x07\x23\x0f\x46"
        "\x1e\x8c\x3c\x18\x79\x30\xf2\x60\xe4\xc1\xc8\x83\x91\x07\x23\x0f\x46\x1e\x8c\x3c\x18\xf9\xaf\x69"
        "\xe4\xff\x7e\xc9\xc4\xbc\x41\x3e\xb3\x10\x8c\x3c\x18\x79\x30\xf2\x60\xe4\xc1\xc8\x83\x91\x07\x23"
        "\x0f\x46\x1e\x8c\x3c\x18\x79\x30\xf2\x60\xe4\xc1\xc8\x83\x91\x07\x23\x0f\x46\x1e\x8c\x3c\x18\x79"
        "\x30\xf2\x60\xe4\xc1\xc8\x83\x91\x07\x23\xff\x35\x8d\x7c\x68\x9f\xc3\x4f\x1a\xe4\x4b\x52\xc0\xc8"
        "\x83\x91\x07\x23\x0f\x46\x1e\x8c\x3c\x18\x79\x30\xf2\x60\xe4\xc1\xc8\x83\x91\x07\x23\x0f\x46\x1e"
        "\x8c\x3c\x18\x79\x30\xf2\x60\xe4\xc1\xc8\x83\x91\x07\x23\x0f\x46\x1e\x8c\x3c\x18\x79\x30\xf2\x5f"
        "\xcb\xc8\xbb\x16\xe7\x2a\x6e\x24\x2f\x7e\xdc\x20\xf7\x8c\x03\x23\x0f\x46\x1e\x8c\x3c\x18\x79\x30"
        "\xf2\x60\xe4\xc1\xc8\x83\x91\x07\x23\x0f\x46\x1e\x8c\x3c\x18\x79\x30\xf2\x60\xe4\xc1\xc8\x83\x91"
        "\x07\x23\x0f\x46\x1e\x8c\x3c\x18\xf9\xff\x4f\x23\x6f\x2a\xcb\xf2\x3d\x9a\xab\xf0\x31\x2e\xec\x9f"
        "\xf5\x5b\x1a\x4d\xfd\x33\xb2\x83\x16\xb5\xc8\x35\x8e\x35\xd3\x03\xfe\x03\x0c\xda\xf1\x94";
    const QByteArray video = qUncompress(encoded, sizeof(encoded) - 1);
    return file.open() && file.write(video) == video.size() && file.flush();
}

bool isRed(const QColor& color)
{
    return color.red() > 80 && color.red() > color.green() * 2 && color.red() > color.blue() * 2;
}

bool isBlue(const QColor& color)
{
    return color.blue() > 80 && color.blue() > color.green() * 2 && color.blue() > color.red() * 2;
}

bool containsNeutralOsd(const QImage& image, const QImage& baseline)
{
    if (image.size() != baseline.size())
        return false;
    int neutral = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            // Check text over the fixture's black letterbox, not antialiased
            // text blended into the red/blue video or a particular font weight.
            if (baseline.pixelColor(x, y) != QColor(Qt::black))
                continue;
            const QColor color = image.pixelColor(x, y);
            const int high = std::max({ color.red(), color.green(), color.blue() });
            const int low = std::min({ color.red(), color.green(), color.blue() });
            if (high <= 16)
                continue;
            if (high - low > 8)
                return false;
            ++neutral;
        }
    }
    return neutral >= 8;
}

// The video is red over blue, so the frame is the right way up when the top of
// the window is red and the bottom is blue. Counting rather than sampling one
// pixel keeps letterboxing and the window's own background out of the answer.
bool isRightWayUp(const QImage& image)
{
    if (image.isNull())
        return false;
    int redAbove = 0;
    int blueAbove = 0;
    int redBelow = 0;
    int blueBelow = 0;
    const int middle = image.height() / 2;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); x += 4) {
            const QColor color = image.pixelColor(x, y);
            if (isRed(color))
                (y < middle ? redAbove : redBelow)++;
            else if (isBlue(color))
                (y < middle ? blueAbove : blueBelow)++;
        }
    }
    std::fprintf(stderr, "orientation: redAbove=%d blueAbove=%d redBelow=%d blueBelow=%d\n", redAbove, blueAbove,
        redBelow, blueBelow);
    return redAbove > blueAbove && blueBelow > redBelow
        && isRed(image.pixelColor(image.width() / 2, image.height() / 4))
        && isBlue(image.pixelColor(image.width() / 2, 3 * image.height() / 4));
}

} // namespace

SPOOL_TEST_MAIN("mpv-video-item")
{
    // The same end-to-end check is worth running against either backend, and
    // the Vulkan one is the whole reason the item moved to the RHI. OpenGL
    // stays the default so CI and a plain local run test what ships.
    const QByteArray api = qgetenv("SPOOL_TEST_RENDER_API").toLower();
#if QT_CONFIG(vulkan)
    if (api == "vulkan")
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Vulkan);
    else
#endif
        QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
    if (api == "vulkan")
        std::fprintf(stderr, "requested Vulkan scene graph\n");
    QSurfaceFormat format;
#ifdef Q_OS_TVOS
    format.setRenderableType(QSurfaceFormat::OpenGLES);
    format.setVersion(3, 0);
#else
    format.setRenderableType(QSurfaceFormat::OpenGL);
    format.setVersion(3, 3);
#endif
    format.setAlphaBufferSize(0);
    QGuiApplication app(argc, argv);

    QTemporaryFile video(QDir::tempPath() + QStringLiteral("/mpv-video-item-XXXXXX.mkv"));
    if (!writeVideo(video)) {
        std::fprintf(stderr, "failed to create test video\n");
        return 1;
    }

#if SPOOL_MPV_ITEM_RHI
    std::atomic_int textureFormat { -1 };
#endif
    QQuickWindow window;
    window.setColor(Qt::black);
    window.resize(320, 180);
    Spool::MpvVideoItem videoItem(window.contentItem());
    // Match production's anchors.fill: parent. UIKit can replace requested
    // window geometry asynchronously with the fullscreen television surface.
    qreal viewportFraction = 1.0;
    const auto fitSurface = [&] { videoItem.setSize(window.contentItem()->size() * viewportFraction); };
    QObject::connect(window.contentItem(), &QQuickItem::widthChanged, &videoItem, fitSurface);
    QObject::connect(window.contentItem(), &QQuickItem::heightChanged, &videoItem, fitSurface);
    fitSurface();
#if SPOOL_MPV_ITEM_RHI
    QObject::connect(
        &window, &QQuickWindow::afterRendering, &videoItem,
        [&] {
            const auto *provider = videoItem.textureProvider();
            const auto *texture = provider ? provider->texture() : nullptr;
            if (const auto *target = texture ? texture->rhiTexture() : nullptr)
                textureFormat.store(int(target->format()));
        },
        Qt::DirectConnection);
#endif
#ifdef Q_OS_TVOS
    window.showFullScreen();
#else
    window.show();
#endif
    app.processEvents();
    const auto captureItem = [&] {
        const QImage image = window.grabWindow();
#ifdef Q_OS_TVOS
        if (image.isNull())
            return image;
        const qreal xScale = qreal(image.width()) / window.contentItem()->width();
        const qreal yScale = qreal(image.height()) / window.contentItem()->height();
        return image.copy(0, 0, qRound(videoItem.width() * xScale), qRound(videoItem.height() * yScale));
#else
        return image;
#endif
    };
    const auto waitForPresentedFrame = [&](const auto& matches) {
        bool matched = false;
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        QObject::connect(
            &window, &QQuickWindow::frameSwapped, &loop,
            [&] {
                if (!matched && matches(captureItem())) {
                    matched = true;
                    loop.quit();
                }
            },
            Qt::QueuedConnection);
        timeout.start(5000);
        // UIKit returns to UIApplicationMain only for EventLoopExec, not a
        // manual processEvents pump. Read back after its real presentation.
        loop.exec();
        return matched;
    };

    // Reusing an item after detach must reset first-frame state and publish
    // the new context, including when Qt replaces the render target on resize.
    for (const QSize size : { QSize(320, 180), QSize(480, 270) }) {
#ifdef Q_OS_TVOS
        // UIKit owns the fullscreen native window; resize the real video
        // viewport instead of requesting an unsupported television window size.
        viewportFraction = size.width() == 320 ? 1.0 : 2.0 / 3.0;
#else
        window.resize(size);
#endif
        fitSurface();
        std::setlocale(LC_NUMERIC, "C");
        mpv_handle *handle = mpv_create();
        const bool verbose = !qgetenv("SPOOL_TEST_MPV_LOG").isEmpty();
        // mpv_create can fail, and the check for that is below: setting options on
        // its result first would crash instead of reporting it, but only for a run
        // that asked for logging.
        if (handle && verbose
            && (mpv_set_option_string(handle, "terminal", "yes") < 0
                || mpv_set_option_string(handle, "msg-level", "all=debug") < 0)) {
            std::fprintf(stderr, "failed to enable mpv logging\n");
            return 1;
        }
        if (!handle || mpv_set_option_string(handle, "terminal", verbose ? "yes" : "no") < 0
            || mpv_set_option_string(handle, "vo", "libmpv") < 0 || mpv_set_option_string(handle, "hwdec", "no") < 0
            || mpv_set_option_string(handle, "osd-color", "#FFFFFFFF") < 0
            || mpv_set_option_string(handle, "osd-font-size", "48") < 0 || mpv_initialize(handle) < 0) {
            std::fprintf(stderr, "failed to initialize mpv\n");
            if (handle)
                mpv_terminate_destroy(handle);
            return 1;
        }
        for (const auto& option : Spool::RenderTargetPolicy::targetOptions({})) {
            if (mpv_set_option_string(handle, option.name.constData(), option.value.constData()) < 0) {
                std::fprintf(stderr, "failed to set the managed SDR target\n");
                mpv_terminate_destroy(handle);
                return 1;
            }
        }

        videoItem.setRenderBackend(qgetenv("SPOOL_TEST_RENDER_BACKEND"));
        videoItem.setMpvHandle(handle);
        if (!videoItem.waitForRenderContext()) {
            std::fprintf(stderr, "render context was not ready before media load\n");
            mpv_terminate_destroy(handle);
            return 1;
        }

        const QByteArray path = QFile::encodeName(video.fileName());
        const char *command[] = { "loadfile", path.constData(), nullptr };
        if (mpv_command(handle, command) < 0) {
            std::fprintf(stderr, "failed to load test video\n");
            videoItem.releaseMpvHandle();
            mpv_terminate_destroy(handle);
            return 1;
        }

        const bool rendered = waitForPresentedFrame(isRightWayUp);
        if (!rendered) {
            int64_t decodedWidth = 0;
            int64_t decodedHeight = 0;
            double position = -1;
            int eof = -1;
            const int widthStatus = mpv_get_property(handle, "video-params/w", MPV_FORMAT_INT64, &decodedWidth);
            const int heightStatus = mpv_get_property(handle, "video-params/h", MPV_FORMAT_INT64, &decodedHeight);
            const int positionStatus = mpv_get_property(handle, "time-pos", MPV_FORMAT_DOUBLE, &position);
            const int eofStatus = mpv_get_property(handle, "eof-reached", MPV_FORMAT_FLAG, &eof);
            std::fprintf(stderr, "decoder: width=%lld(%d) height=%lld(%d) position=%.3f(%d) eof=%d(%d)\n",
                static_cast<long long>(decodedWidth), widthStatus, static_cast<long long>(decodedHeight), heightStatus,
                position, positionStatus, eof, eofStatus);
            captureItem().save(QDir::tempPath() + QStringLiteral("/mpv-video-item-failure.png"));
#ifdef Q_OS_TVOS
            if (verbose) {
                std::fprintf(stderr, "native presentation diagnostic: holding failed live viewport\n");
                window.scheduleRenderJob(
                    QRunnable::create([item = QPointer<Spool::MpvVideoItem>(&videoItem),
                                          surface = QPointer<QQuickWindow>(&window)] {
                        if (!item || !surface || !QOpenGLContext::currentContext())
                            return;
                        const auto *provider = item->textureProvider();
                        const auto *texture = provider ? provider->texture() : nullptr;
                        const auto *native = texture ? texture->nativeInterface<QNativeInterface::QSGOpenGLTexture>() : nullptr;
                        if (!native)
                            return;
                        const QSize size = texture->textureSize();
                        surface->beginExternalCommands();
                        auto *gl = QOpenGLContext::currentContext()->extraFunctions();
                        GLint readFbo = 0, drawFbo = 0, packBuffer = 0, rowLength = 0, skipRows = 0, skipPixels = 0;
                        gl->glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFbo);
                        gl->glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo);
                        gl->glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &packBuffer);
                        gl->glGetIntegerv(GL_PACK_ROW_LENGTH, &rowLength);
                        gl->glGetIntegerv(GL_PACK_SKIP_ROWS, &skipRows);
                        gl->glGetIntegerv(GL_PACK_SKIP_PIXELS, &skipPixels);
                        GLuint probe = 0;
                        gl->glGenFramebuffers(1, &probe);
                        gl->glBindFramebuffer(GL_FRAMEBUFFER, probe);
                        gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                            native->nativeTexture(), 0);
                        const GLenum status = gl->glCheckFramebufferStatus(GL_FRAMEBUFFER);
                        unsigned char quarter[4] {}, threeQuarter[4] {};
                        gl->glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
                        gl->glPixelStorei(GL_PACK_ROW_LENGTH, 0);
                        gl->glPixelStorei(GL_PACK_SKIP_ROWS, 0);
                        gl->glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
                        if (status == GL_FRAMEBUFFER_COMPLETE) {
                            gl->glReadPixels(size.width() / 2, size.height() / 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, quarter);
                            gl->glReadPixels(size.width() / 2, 3 * size.height() / 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, threeQuarter);
                        }
                        gl->glPixelStorei(GL_PACK_ROW_LENGTH, rowLength);
                        gl->glPixelStorei(GL_PACK_SKIP_ROWS, skipRows);
                        gl->glPixelStorei(GL_PACK_SKIP_PIXELS, skipPixels);
                        gl->glBindBuffer(GL_PIXEL_PACK_BUFFER, packBuffer);
                        gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, readFbo);
                        gl->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, drawFbo);
                        gl->glDeleteFramebuffers(1, &probe);
                        surface->endExternalCommands();
                        std::fprintf(stderr, "native target: size=%dx%d status=%u y25=%u,%u,%u,%u y75=%u,%u,%u,%u\n",
                            size.width(), size.height(), unsigned(status),
                            unsigned(quarter[0]), unsigned(quarter[1]), unsigned(quarter[2]), unsigned(quarter[3]),
                            unsigned(threeQuarter[0]), unsigned(threeQuarter[1]), unsigned(threeQuarter[2]), unsigned(threeQuarter[3]));
                    }),
                    QQuickWindow::AfterRenderingStage);
                window.update();
                QEventLoop hold;
                QTimer::singleShot(15000, &hold, &QEventLoop::quit);
                hold.exec();
            }
#endif
        }

        // Diagnostic, not an assertion: what the swapchain can present depends on
        // the driver, the compositor and whether the display is in HDR mode, none
        // of which a test can require.
        const Spool::DisplayOutputCapabilities display = Spool::PlatformDisplayOutput::probe(&window);
        std::fprintf(stderr, "display: hdrAvailable=%d format=%d sdrWhite=%.0f min=%.4f max=%.0f\n",
            int(display.hdrAvailable), int(display.preferredFormat), double(display.sdrWhiteNits),
            double(display.minLuminanceNits), double(display.maxLuminanceNits));

        const bool upright = rendered && isRightWayUp(captureItem());
        char *pixelFormat = mpv_get_property_string(handle, "video-target-params/pixelformat");
        const QByteArray actualFormat = pixelFormat ? QByteArray(pixelFormat) : QByteArray();
        mpv_free(pixelFormat);
        // The legacy OpenGL renderer does not expose video-target-params.
        // Inspect the actual RHI texture on both APIs, and additionally the
        // mpv handover descriptor on Vulkan.
#if SPOOL_MPV_ITEM_RHI
        const bool sdrTarget
            = textureFormat.load() == int(QRhiTexture::RGBA8) && (api != "vulkan" || actualFormat == "rgba8");
        std::fprintf(stderr, "rendered SDR target: RHI=%d mpv=%s\n", textureFormat.load(),
            actualFormat.isEmpty() ? "(legacy renderer)" : actualFormat.constData());
#endif
        const QImage beforeOsd = captureItem();
        const char *osdCommand[] = { "show-text", "SDR white", "10000", nullptr };
        bool neutralOsd = false;
        if (mpv_command(handle, osdCommand) >= 0) {
            neutralOsd = waitForPresentedFrame([&](const QImage& image) { return containsNeutralOsd(image, beforeOsd); });
        }
        const bool released = videoItem.releaseMpvHandle();
        mpv_terminate_destroy(handle);
        if (!rendered || !upright || !released || !neutralOsd
#if SPOOL_MPV_ITEM_RHI
            || !sdrTarget
#endif
        ) {
            std::fprintf(stderr, "video result: rendered=%d upright=%d released=%d neutralOSD=%d\n",
                rendered, upright, released, neutralOsd);
            const QImage failedFrame = captureItem();
            const QColor upper = failedFrame.pixelColor(failedFrame.width() / 2, failedFrame.height() / 4);
            const QColor lower = failedFrame.pixelColor(failedFrame.width() / 2, 3 * failedFrame.height() / 4);
            std::fprintf(stderr, "viewport: window=%dx%d content=%.0fx%.0f item=%.0fx%.0f capture=%dx%d upper=%s lower=%s\n",
                window.width(), window.height(), window.contentItem()->width(), window.contentItem()->height(),
                videoItem.width(), videoItem.height(), failedFrame.width(), failedFrame.height(),
                qPrintable(upper.name()), qPrintable(lower.name()));
            return 1;
        }
    }
    std::fprintf(stderr, "mpv video smoke: upright frames and OSD rendered across detach and resize\n");
    return 0;
}

namespace {

int vulkanEntry(int argc, char **argv)
{
    qputenv("SPOOL_TEST_RENDER_API", "vulkan");
    return spoolTestBody(argc, argv);
}

// Registered by hand rather than with a second SPOOL_TEST_MAIN, which names
// its body the same thing every time and so can only appear once per file.
[[maybe_unused]] const bool vulkanRegistered = ::SpoolTests::registerTest("mpv-video-item-vulkan", &vulkanEntry);

} // namespace
