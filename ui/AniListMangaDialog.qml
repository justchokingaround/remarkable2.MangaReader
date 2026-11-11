import QtQuick

// Per-manga AniList sheet: link / change match, edit status & progress, unlink.
Rectangle {
    id: dlg
    color: "#80000000"
    visible: active

    property bool active: false
    property var info: ({})            // detail anilist info (see Tracker::detailInfo)
    property string mangaTitle: ""
    property var results: []
    property bool searching: false
    property string searchError: ""
    property bool showSearch: false
    property bool keyboardOpen: false

    // Editor state
    property string editStatus: ""
    property int editProgress: 0

    signal searchRequested(string query)
    signal linkRequested(var media)
    signal unlinkRequested()
    signal updateRequested(int progress, string status)
    signal closed()

    readonly property bool linked: Boolean(info && info.linked)
    readonly property bool dirty: linked && (editStatus !== (info.status || "") || editProgress !== (info.progress || 0))

    readonly property var statuses: [
        { id: "CURRENT", label: "Reading" },
        { id: "PLANNING", label: "Planning" },
        { id: "COMPLETED", label: "Completed" },
        { id: "PAUSED", label: "Paused" },
        { id: "DROPPED", label: "Dropped" },
        { id: "REPEATING", label: "Rereading" }
    ]

    function open() {
        resetEditor();
        showSearch = !linked;
        keyboardOpen = false;
        queryInput.text = "";
        active = true;
        if (showSearch) searchRequested("");  // backend searches with every known title
    }

    function resetEditor() {
        editStatus = (info && info.status) ? info.status : "";
        editProgress = (info && info.progress) ? info.progress : 0;
    }

    onInfoChanged: if (active && !dirty) resetEditor()

    function close() {
        active = false;
        keyboardOpen = false;
        closed();
    }

    function runSearch() {
        keyboardOpen = false;
        searchRequested(queryInput.text.trim());
    }

    MouseArea { anchors.fill: parent; onClicked: (mouse) => dlg.close() }

    Rectangle {
        id: card
        anchors.top: parent.top
        anchors.topMargin: 40
        anchors.horizontalCenter: parent.horizontalCenter
        width: parent.width - 60
        height: parent.height - 80
        color: "#FFFFFF"
        border.width: 3
        border.color: "#000000"

        MouseArea { anchors.fill: parent }  // swallow taps

        Column {
            id: topCol
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.margins: 24
            spacing: 18

            Item {
                width: parent.width
                height: 60
                Text {
                    anchors.left: parent.left
                    anchors.right: closeBtn.left
                    anchors.verticalCenter: parent.verticalCenter
                    text: "AniList · " + dlg.mangaTitle
                    font.pixelSize: 28
                    font.bold: true
                    elide: Text.ElideRight
                }
                Rectangle {
                    id: closeBtn
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    width: 130
                    height: 56
                    color: closeArea.pressed ? "#000000" : "#FFFFFF"
                    border.width: 2
                    border.color: "#000000"
                    Text {
                        anchors.centerIn: parent
                        text: "✕ Close"
                        font.pixelSize: 21
                        font.bold: true
                        color: closeArea.pressed ? "#FFFFFF" : "#000000"
                    }
                    MouseArea { id: closeArea; anchors.fill: parent; onClicked: (mouse) => dlg.close() }
                }
            }

            // ---------------- linked: editor
            Column {
                visible: dlg.linked && !dlg.showSearch
                width: parent.width
                spacing: 18

                Text {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    font.pixelSize: 23
                    text: "Linked to <b>" + (dlg.info.title || "") + "</b>" +
                          (dlg.info.chapters > 0 ? "  (" + dlg.info.chapters + " chapters)" : "") +
                          (dlg.info.on_list ? "" : "<br/>Not on your list yet. It gets added when you read a chapter or press Save.")
                    textFormat: Text.StyledText
                }
                Text {
                    visible: Boolean(dlg.info.conflict)
                    width: parent.width
                    wrapMode: Text.WordWrap
                    font.pixelSize: 21
                    font.bold: true
                    text: "⚠ This entry changed on AniList and on this device. Open the AniList tab to choose which one to keep."
                }
                Text {
                    visible: Boolean(dlg.info.pending) && !dlg.info.conflict
                    width: parent.width
                    wrapMode: Text.WordWrap
                    font.pixelSize: 20
                    color: "#444444"
                    text: "⟳ Waiting to sync. AniList currently has Ch. " + (dlg.info.remote_progress || 0) + "."
                }

                Text { text: "Status"; font.pixelSize: 21; font.bold: true; color: "#444444" }
                Flow {
                    width: parent.width
                    spacing: 10
                    Repeater {
                        model: dlg.statuses
                        delegate: Rectangle {
                            id: sPill
                            readonly property bool sel: dlg.editStatus === modelData.id
                            width: sTxt.implicitWidth + 36
                            height: 60
                            color: (sel || sArea.pressed) ? "#000000" : "#FFFFFF"
                            border.width: 2
                            border.color: "#000000"
                            Text {
                                id: sTxt
                                anchors.centerIn: parent
                                text: modelData.label
                                font.pixelSize: 22
                                font.bold: sPill.sel
                                color: (sPill.sel || sArea.pressed) ? "#FFFFFF" : "#000000"
                            }
                            MouseArea {
                                id: sArea
                                anchors.fill: parent
                                onClicked: (mouse) => {
                                    dlg.editStatus = modelData.id;
                                    if (modelData.id === "COMPLETED" && dlg.info.chapters > 0) dlg.editProgress = dlg.info.chapters;
                                }
                            }
                        }
                    }
                }

                Text { text: "Chapters read"; font.pixelSize: 21; font.bold: true; color: "#444444" }
                Row {
                    spacing: 14
                    Repeater {
                        model: [-10, -1]
                        delegate: Rectangle {
                            width: 96
                            height: 72
                            color: decArea.pressed ? "#000000" : "#FFFFFF"
                            border.width: 2
                            border.color: "#000000"
                            Text {
                                anchors.centerIn: parent
                                text: String(modelData)
                                font.pixelSize: 26
                                font.bold: true
                                color: decArea.pressed ? "#FFFFFF" : "#000000"
                            }
                            MouseArea {
                                id: decArea
                                anchors.fill: parent
                                onClicked: (mouse) => { dlg.editProgress = Math.max(0, dlg.editProgress + modelData); }
                            }
                        }
                    }
                    Rectangle {
                        width: 220
                        height: 72
                        border.width: 2
                        border.color: "#000000"
                        Text {
                            anchors.centerIn: parent
                            text: dlg.editProgress + (dlg.info.chapters > 0 ? " / " + dlg.info.chapters : "")
                            font.pixelSize: 30
                            font.bold: true
                        }
                    }
                    Repeater {
                        model: [1, 10]
                        delegate: Rectangle {
                            width: 96
                            height: 72
                            color: incArea.pressed ? "#000000" : "#FFFFFF"
                            border.width: 2
                            border.color: "#000000"
                            Text {
                                anchors.centerIn: parent
                                text: "+" + modelData
                                font.pixelSize: 26
                                font.bold: true
                                color: incArea.pressed ? "#FFFFFF" : "#000000"
                            }
                            MouseArea {
                                id: incArea
                                anchors.fill: parent
                                onClicked: (mouse) => {
                                    let v = dlg.editProgress + modelData;
                                    if (dlg.info.chapters > 0) v = Math.min(v, dlg.info.chapters);
                                    dlg.editProgress = v;
                                }
                            }
                        }
                    }
                }

                Row {
                    spacing: 14
                    Rectangle {
                        width: 300
                        height: 76
                        opacity: (dlg.dirty || !dlg.info.on_list) ? 1.0 : 0.4
                        color: saveArea.pressed ? "#FFFFFF" : "#000000"
                        border.width: 2
                        border.color: "#000000"
                        Text {
                            anchors.centerIn: parent
                            text: "Save to AniList"
                            font.pixelSize: 23
                            font.bold: true
                            color: saveArea.pressed ? "#000000" : "#FFFFFF"
                        }
                        MouseArea {
                            id: saveArea
                            anchors.fill: parent
                            onClicked: (mouse) => {
                                if (!dlg.dirty && dlg.info.on_list) return;
                                dlg.updateRequested(dlg.editProgress, dlg.editStatus || "CURRENT");
                            }
                        }
                    }
                    Rectangle {
                        width: 230
                        height: 76
                        color: changeArea.pressed ? "#000000" : "#FFFFFF"
                        border.width: 2
                        border.color: "#000000"
                        Text {
                            anchors.centerIn: parent
                            text: "Change match"
                            font.pixelSize: 22
                            font.bold: true
                            color: changeArea.pressed ? "#FFFFFF" : "#000000"
                        }
                        MouseArea {
                            id: changeArea
                            anchors.fill: parent
                            onClicked: (mouse) => {
                                dlg.showSearch = true;
                                dlg.searchRequested("");
                            }
                        }
                    }
                    Rectangle {
                        width: 160
                        height: 76
                        color: unlinkArea.pressed ? "#000000" : "#FFFFFF"
                        border.width: 2
                        border.color: "#000000"
                        Text {
                            anchors.centerIn: parent
                            text: "Unlink"
                            font.pixelSize: 22
                            font.bold: true
                            color: unlinkArea.pressed ? "#FFFFFF" : "#000000"
                        }
                        MouseArea {
                            id: unlinkArea
                            anchors.fill: parent
                            onClicked: (mouse) => dlg.unlinkRequested()
                        }
                    }
                }
                Text {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    font.pixelSize: 19
                    color: "#555555"
                    text: "Progress is updated automatically when you finish a chapter. Saved changes sync now, or as soon as Wi-Fi is back."
                }
            }

            // ---------------- search
            Column {
                visible: dlg.showSearch
                width: parent.width
                spacing: 12

                Text {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    font.pixelSize: 21
                    color: "#333333"
                    text: dlg.linked ? "Pick the right AniList entry:" : "Which AniList entry is this? Best matches first:"
                }
                Row {
                    width: parent.width
                    spacing: 12
                    Rectangle {
                        width: parent.width - 200 - 12
                        height: 64
                        border.width: 2
                        border.color: "#000000"
                        TextInput {
                            id: queryInput
                            anchors.fill: parent
                            anchors.margins: 14
                            font.pixelSize: 24
                            verticalAlignment: TextInput.AlignVCenter
                            clip: true
                            onAccepted: dlg.runSearch()
                        }
                        Text {
                            anchors.left: parent.left
                            anchors.leftMargin: 14
                            anchors.verticalCenter: parent.verticalCenter
                            visible: queryInput.text.length === 0
                            text: "Search AniList by another title…"
                            font.pixelSize: 22
                            color: "#888888"
                        }
                        MouseArea {
                            anchors.fill: parent
                            onClicked: (mouse) => {
                                queryInput.forceActiveFocus();
                                dlg.keyboardOpen = true;
                            }
                        }
                    }
                    Rectangle {
                        width: 200
                        height: 64
                        color: goArea.pressed ? "#FFFFFF" : "#000000"
                        border.width: 2
                        border.color: "#000000"
                        Text {
                            anchors.centerIn: parent
                            text: "Search"
                            font.pixelSize: 22
                            font.bold: true
                            color: goArea.pressed ? "#000000" : "#FFFFFF"
                        }
                        MouseArea { id: goArea; anchors.fill: parent; onClicked: (mouse) => dlg.runSearch() }
                    }
                }
            }
        }

        Text {
            visible: dlg.showSearch && (dlg.searching || dlg.results.length === 0)
            anchors.top: topCol.bottom
            anchors.topMargin: 50
            anchors.horizontalCenter: parent.horizontalCenter
            width: parent.width - 80
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            font.pixelSize: 24
            text: dlg.searching ? "Searching AniList…" : (dlg.searchError || "No matches")
        }

        ListView {
            id: resList
            visible: dlg.showSearch && !dlg.searching
            anchors.top: topCol.bottom
            anchors.topMargin: 12
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: kb.top
            anchors.leftMargin: 24
            anchors.rightMargin: 24
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            model: dlg.results
            delegate: Rectangle {
                width: resList.width
                height: 112
                color: resArea.pressed ? "#EAEAEA" : "#FFFFFF"
                Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: "#BBBBBB" }
                Column {
                    anchors.left: parent.left
                    anchors.leftMargin: 6
                    anchors.right: pickTxt.left
                    anchors.rightMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 4
                    Text {
                        width: parent.width
                        text: (modelData.exact ? "★ " : "") + modelData.title +
                              (modelData.title_english && modelData.title_english !== modelData.title ? "  ·  " + modelData.title_english : "")
                        font.pixelSize: 24
                        font.bold: true
                        elide: Text.ElideRight
                    }
                    Text {
                        width: parent.width
                        font.pixelSize: 19
                        color: "#555555"
                        elide: Text.ElideRight
                        text: {
                            let p = [];
                            if (modelData.format) p.push(modelData.format.replace("_", " "));
                            if (modelData.year) p.push(String(modelData.year));
                            if (modelData.chapters) p.push(modelData.chapters + " ch.");
                            if (modelData.list_status_label) p.push("On your list: " + modelData.list_status_label + ", Ch. " + (modelData.list_progress || 0));
                            if (dlg.info && dlg.info.media_id === modelData.media_id) p.push("✓ current link");
                            return p.join("  ·  ");
                        }
                    }
                }
                Text {
                    id: pickTxt
                    anchors.right: parent.right
                    anchors.rightMargin: 6
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Link →"
                    font.pixelSize: 22
                    font.bold: true
                }
                MouseArea {
                    id: resArea
                    anchors.fill: parent
                    onClicked: (mouse) => {
                        dlg.showSearch = false;
                        dlg.keyboardOpen = false;
                        dlg.linkRequested(modelData);
                    }
                }
            }
        }

        TouchKeyboard {
            id: kb
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            active: dlg.keyboardOpen
            submitLabel: "Search ↵"
            onKeyTyped: (txt) => queryInput.insert(queryInput.cursorPosition, txt)
            onBackspace: () => {
                if (queryInput.cursorPosition > 0) queryInput.remove(queryInput.cursorPosition - 1, queryInput.cursorPosition);
            }
            onClearAll: () => { queryInput.text = ""; }
            onSubmitted: () => dlg.runSearch()
            onDismissed: () => { dlg.keyboardOpen = false; }
        }
    }
}
