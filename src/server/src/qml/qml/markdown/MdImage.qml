import QtQuick
import QtQuick.Layouts
import Vicinae

Item {
    id: root

    property var blockData: ({})
    property var mdModel: null
    property int blockIndex: -1
    property var selectionController: null
    property bool selected: false
    property real maxImageHeight: 0

    readonly property int requestedWidth: blockData.width ?? 0
    readonly property int requestedHeight: blockData.height ?? 0
    readonly property string alt: blockData.alt ?? ""
    readonly property string link: blockData.link ?? ""
    readonly property string internalTarget: blockData.alt?.startsWith("account:") ? blockData.alt.slice(8) : ""

    width: parent?.width ?? 0
    implicitHeight: col.implicitHeight

    ColumnLayout {
        id: col
        anchors.horizontalCenter: parent.horizontalCenter
        spacing: 4

        Item {
            id: imgContainer
            Layout.alignment: Qt.AlignHCenter
            Layout.preferredWidth: imgW
            Layout.preferredHeight: imgH

            readonly property real fitScale: {
                if (root.requestedWidth <= 0 || root.width <= 0)
                    return 1;
                return Math.min(1, root.width / root.requestedWidth);
            }
            readonly property int imgW: {
                if (root.requestedWidth > 0)
                    return Math.round(root.requestedWidth * fitScale);
                if (root.requestedHeight > 0)
                    return root.requestedHeight;
                return root.width;
            }
            readonly property int imgH: {
                if (root.requestedWidth > 0 && root.requestedHeight > 0)
                    return Math.round(root.requestedHeight * fitScale);
                const rawH = root.requestedHeight > 0 ? root.requestedHeight : root.requestedWidth > 0 ? root.requestedWidth : 200;
                return root.maxImageHeight > 0 ? Math.min(rawH, 200, root.maxImageHeight) : Math.min(rawH, 200);
            }

            ViciImage {
                anchors.fill: parent
                fillMode: Image.PreserveAspectFit
                source: root.blockData.src ?? ""
            }

            Rectangle {
                anchors.fill: parent
                color: Theme.textSelectionBg
                opacity: 0.4
                visible: root.selected
            }

            MouseArea {
                anchors.fill: parent
                enabled: root.link.length > 0
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: root.mdModel?.openLink(root.link)
            }
        }

        Text {
            Layout.alignment: Qt.AlignHCenter
            visible: root.alt.length > 0 && root.internalTarget.length === 0
            text: root.alt
            color: Theme.textMuted
            font.pointSize: Theme.smallerFontSize
            font.italic: true
        }
    }

    onSelectionControllerChanged: if (selectionController)
        selectionController.registerSelectable(root, blockIndex * 10000, false)
    Component.onDestruction: if (selectionController)
        selectionController.unregisterSelectable(root)
}
