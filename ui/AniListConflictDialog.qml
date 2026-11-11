import QtQuick

// Asks the user which side wins when AniList and the device both changed an entry.
Rectangle {
    id: cd
    color: "#99000000"
    visible: active && conflicts.length > 0

    property bool active: false
    property var conflicts: []

    signal resolve(int mediaId, string choice)
    signal later()

    readonly property var c: conflicts.length > 0 ? conflicts[0] : ({})

    function remoteDesc() {
        if (!c.remote_present) return "not on your list";
        return "Ch. " + (c.remote_progress || 0) + ", " + (c.remote_status_label || "");
    }
    function localDesc() {
        return "Ch. " + (c.local_progress || 0) + (c.local_status_label ? ", " + c.local_status_label : "");
    }

    readonly property string explanation: {
        switch (c.reason) {
        case "went_back":
            return "On AniList the progress went back to Ch. " + (c.remote_progress || 0) +
                   (c.remote_status === "REPEATING" ? " (you started a reread)" : "") +
                   ", but this reMarkable read up to Ch. " + (c.local_progress || 0) + " while offline.";
        case "removed":
            return "It was removed from your AniList list, but this reMarkable read up to Ch. " +
                   (c.local_progress || 0) + " while offline.";
        case "status":
            return "On AniList it's marked " + (c.remote_status_label || "?") + " (Ch. " + (c.remote_progress || 0) +
                   "), but this reMarkable has read up to Ch. " + (c.local_progress || 0) + ".";
        case "explicit":
            return "You changed it on this reMarkable (" + localDesc() + "), but it was also changed on AniList (" +
                   remoteDesc() + ").";
        case "error":
            return "AniList refused the update (" + (c.message || "unknown error") + ").";
        }
        return "AniList and this reMarkable disagree.";
    }

    MouseArea { anchors.fill: parent }  // modal

    Rectangle {
        anchors.centerIn: parent
        width: parent.width - 120
        height: col.height + 64
        color: "#FFFFFF"
        border.width: 4
        border.color: "#000000"

        Column {
            id: col
            x: 32
            y: 32
            width: parent.width - 64
            spacing: 22

            Text {
                text: "Which progress is right?" + (cd.conflicts.length > 1 ? "  (1 of " + cd.conflicts.length + ")" : "")
                font.pixelSize: 30
                font.bold: true
            }
            Text {
                width: parent.width
                text: cd.c.title || ""
                font.pixelSize: 27
                font.bold: true
                wrapMode: Text.WordWrap
            }
            Text {
                width: parent.width
                text: cd.explanation
                font.pixelSize: 23
                wrapMode: Text.WordWrap
                color: "#222222"
            }

            Rectangle {
                width: parent.width
                height: 92
                color: localArea.pressed ? "#FFFFFF" : "#000000"
                border.width: 3
                border.color: "#000000"
                Column {
                    anchors.centerIn: parent
                    spacing: 2
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: cd.c.reason === "removed" ? "Add it back with this device's progress" : "Use this reMarkable's progress"
                        font.pixelSize: 24
                        font.bold: true
                        color: localArea.pressed ? "#000000" : "#FFFFFF"
                    }
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: cd.localDesc()
                        font.pixelSize: 20
                        color: localArea.pressed ? "#000000" : "#FFFFFF"
                    }
                }
                MouseArea {
                    id: localArea
                    anchors.fill: parent
                    onClicked: (mouse) => cd.resolve(cd.c.media_id, "local")
                }
            }

            Rectangle {
                width: parent.width
                height: 92
                color: remoteArea.pressed ? "#000000" : "#FFFFFF"
                border.width: 3
                border.color: "#000000"
                Column {
                    anchors.centerIn: parent
                    spacing: 2
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: "Keep what AniList has"
                        font.pixelSize: 24
                        font.bold: true
                        color: remoteArea.pressed ? "#FFFFFF" : "#000000"
                    }
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: cd.remoteDesc()
                        font.pixelSize: 20
                        color: remoteArea.pressed ? "#FFFFFF" : "#000000"
                    }
                }
                MouseArea {
                    id: remoteArea
                    anchors.fill: parent
                    onClicked: (mouse) => cd.resolve(cd.c.media_id, "remote")
                }
            }

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "Decide later"
                font.pixelSize: 22
                font.underline: true
                MouseArea {
                    anchors.fill: parent
                    anchors.margins: -16
                    onClicked: (mouse) => cd.later()
                }
            }
        }
    }
}
