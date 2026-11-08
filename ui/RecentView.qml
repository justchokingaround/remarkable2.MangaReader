import QtQuick

Item {
    id: recentRoot
    property var recentItems: []

    signal resumeItem(var itemObj)
    signal openManga(var itemObj)

    function formatRelativeTime(epochSec) {
        if (!epochSec || epochSec <= 0) return "";
        let nowSec = Math.floor(Date.now() / 1000);
        let diff = Math.max(0, nowSec - epochSec);
        if (diff < 60) return "Just now";
        if (diff < 3600) return Math.floor(diff / 60) + "m ago";
        if (diff < 86400) return Math.floor(diff / 3600) + "h ago";
        return Math.floor(diff / 86400) + "d ago";
    }

    Column {
        anchors.fill: parent
        anchors.margins: 28
        spacing: 20

        Text {
            text: "Recently Read"
            font.pixelSize: 30
            font.bold: true
            color: "#000000"
        }

        Rectangle {
            visible: !recentRoot.recentItems || recentRoot.recentItems.length === 0
            width: parent.width
            height: 280
            color: "#F8F8F8"
            border.width: 2
            border.color: "#000000"

            Text {
                anchors.centerIn: parent
                text: "Your reading history will appear here once you open a chapter."
                font.pixelSize: 23
                color: "#444444"
            }
        }

        ListView {
            id: recentList
            visible: recentRoot.recentItems && recentRoot.recentItems.length > 0
            width: parent.width
            height: parent.height - 72
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            spacing: 14
            model: recentRoot.recentItems

            delegate: Rectangle {
                width: recentList.width
                height: 144
                color: "#FFFFFF"
                border.width: 2
                border.color: "#000000"

                Rectangle {
                    id: thumbFrame
                    anchors.left: parent.left
                    anchors.leftMargin: 18
                    anchors.verticalCenter: parent.verticalCenter
                    width: 82
                    height: 112
                    color: "#F0F0F0"
                    border.width: 2
                    border.color: "#000000"
                    clip: true

                    Text {
                        anchors.centerIn: parent
                        text: modelData.title ? modelData.title.slice(0, 1).toUpperCase() : "M"
                        font.pixelSize: 32
                        font.bold: true
                        color: "#000000"
                        visible: thumbImg.status !== Image.Ready
                    }

                    Image {
                        id: thumbImg
                        anchors.fill: parent
                        source: modelData.cover_path ? modelData.cover_path : ""
                        fillMode: Image.PreserveAspectCrop
                        asynchronous: true
                        cache: true
                    }
                }

                Column {
                    anchors.left: thumbFrame.right
                    anchors.leftMargin: 22
                    anchors.right: actionRow.left
                    anchors.rightMargin: 20
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 6

                    Text {
                        width: parent.width
                        text: modelData.title || ""
                        font.pixelSize: 26
                        font.bold: true
                        color: "#000000"
                        elide: Text.ElideRight
                    }

                    Text {
                        width: parent.width
                        text: {
                            let ch = (modelData.last_chapter_num !== undefined && modelData.last_chapter_num !== null)
                                     ? ("Chapter " + modelData.last_chapter_num)
                                     : (modelData.last_chapter_title || "Chapter");
                            if (modelData.last_page_count > 0) {
                                ch += "  ·  Page " + ((modelData.last_page_index || 0) + 1) + " / " + modelData.last_page_count;
                            }
                            return ch;
                        }
                        font.pixelSize: 22
                        color: "#222222"
                        elide: Text.ElideRight
                    }

                    Text {
                        width: parent.width
                        text: (modelData.source_name || modelData.source_id || "") +
                              "  ·  " + recentRoot.formatRelativeTime(modelData.last_read_at)
                        font.pixelSize: 19
                        color: "#555555"
                        elide: Text.ElideRight
                    }
                }

                MouseArea {
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    anchors.right: actionRow.left
                    onClicked: (mouse) => recentRoot.openManga(modelData)
                }

                Row {
                    id: actionRow
                    anchors.right: parent.right
                    anchors.rightMargin: 18
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 12

                    Rectangle {
                        width: 150
                        height: 68
                        color: chBtnArea.pressed ? "#000000" : "#FFFFFF"
                        border.width: 2
                        border.color: "#000000"

                        Text {
                            anchors.centerIn: parent
                            text: "Chapters"
                            font.pixelSize: 21
                            font.bold: true
                            color: chBtnArea.pressed ? "#FFFFFF" : "#000000"
                        }
                        MouseArea {
                            id: chBtnArea
                            anchors.fill: parent
                            onClicked: (mouse) => recentRoot.openManga(modelData)
                        }
                    }

                    Rectangle {
                        width: 182
                        height: 68
                        color: resArea.pressed ? "#FFFFFF" : "#000000"
                        border.width: 2
                        border.color: "#000000"

                        Text {
                            anchors.centerIn: parent
                            text: "Resume →"
                            font.pixelSize: 22
                            font.bold: true
                            color: resArea.pressed ? "#000000" : "#FFFFFF"
                        }
                        MouseArea {
                            id: resArea
                            anchors.fill: parent
                            onClicked: (mouse) => recentRoot.resumeItem(modelData)
                        }
                    }
                }
            }
        }
    }
}
