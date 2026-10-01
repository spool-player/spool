import QtQuick
import Spool

AppText {
    property var provider
    visible: !!provider && !!provider.missingHostExtensions && provider.missingHostExtensions.length > 0
    text: "Update Spool to use all features of this provider."
    wrapMode: Text.WordWrap
}
