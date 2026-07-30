import QtQuick
import QtQuick.Layouts
import Vicinae

Item {
    id: root

    property var blockData: ({})
    property var mdModel: null
    property int blockIndex: -1
    property var selectionController: null
    property real maxImageHeight: 0

    readonly property var images: blockData.images ?? []

    width: parent?.width ?? 0
    implicitHeight: flow.implicitHeight

    Flow {
        id: flow
        width: root.width
        spacing: 8

        Repeater {
            model: root.images

            Item {
                id: chip
                required property var modelData
                required property int index

                readonly property int requestedWidth: modelData.width ?? 34
                readonly property int requestedHeight: modelData.height ?? 34
                readonly property string link: modelData.link ?? ""

                width: requestedWidth
                height: requestedHeight

                ViciImage {
                    anchors.fill: parent
                    fillMode: Image.PreserveAspectFit
                    source: chip.modelData.src ?? ""
                }

                MouseArea {
                    anchors.fill: parent
                    enabled: chip.link.length > 0
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.mdModel?.openLink(chip.link)
                }
            }
        }
    }
}
