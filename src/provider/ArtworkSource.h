#pragma once

#include <QString>

namespace JellyfinNative {

// Turns an image the model names (an item, an image slot and the tag the
// source stamped on it) into a URL the artwork pipeline can fetch. Which
// slot, how large, and how hard to compress are decided by ArtworkService;
// only the URL shape belongs to the source. Plain abstract class, see
// Catalog.
class ArtworkSource {
public:
    struct ImageRequest {
        QString itemId;
        QString tag;
        // Primary, Backdrop, Logo, Banner or Thumb: the slot names the media
        // model's tag fields already use.
        QString imageType;
        // Either a width bound or an exact fill box; the fill box wins when
        // both dimensions are set.
        int maxWidth = 0;
        int fillWidth = 0;
        int fillHeight = 0;
        // webp, jpeg or png, and the encoder quality to ask for.
        QString format;
        int quality = 0;
    };

    virtual ~ArtworkSource() = default;

    // Empty when the request cannot be served (no item, no tag, no origin).
    virtual QString imageUrl(const ImageRequest& request) const = 0;
};

} // namespace JellyfinNative
