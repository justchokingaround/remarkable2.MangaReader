import QtQuick

// AniList tab: connect flow, sync status, the user's manga list (offline cached).
Item {
    id: alRoot

    property var alState: ({})        // EVT_ANILIST_STATE payload
    property var connectInfo: ({})    // EVT_ANILIST_CONNECT payload
    property var sourceResults: []    // EVT_ANILIST_SOURCES results for findEntry
    property bool sourcesLoading: false
    property string sourcesError: ""
    property var findEntry: null      // entry whose sources are being searched
    property var installedSources: [] // for the "which sources" filter pills
    property var sourceFilterSel: []  // selected source ids (empty = all)
    property bool sourcesProbing: true
    property string statusFilter: "CURRENT"
    property bool confirmLogout: false

    signal requestConnect(bool start)
    signal requestSync()
    signal requestLogout()
    signal openLinked(var entry)
    signal requestFindSources(var entry, var sources, bool probe)
    signal linkAndOpen(var entry, var result)
    signal showConflicts()

    readonly property bool connected: Boolean(alState && alState.connected)
    readonly property var entries: (alState && alState.entries) ? alState.entries : []

    readonly property var filters: [
        { id: "CURRENT", label: "Reading" },
        { id: "REPEATING", label: "Rereading" },
        { id: "PLANNING", label: "Planning" },
        { id: "PAUSED", label: "Paused" },
        { id: "COMPLETED", label: "Completed" },
        { id: "DROPPED", label: "Dropped" }
    ]

    function countFor(st) {
        let n = 0;
        for (let i = 0; i < entries.length; ++i) if (entries[i].status === st) ++n;
        return n;
    }

    readonly property var filteredEntries: {
        let out = [];
        for (let i = 0; i < entries.length; ++i) {
            if (entries[i].status === statusFilter) out.push(entries[i]);
        }
        out.sort(function(a, b) { return (b.updated_at || 0) - (a.updated_at || 0); });
        return out;
    }

    function relTime(ts) {
        if (!ts) return "never";
        let d = Math.floor(Date.now() / 1000) - ts;
        if (d < 60) return "just now";
        if (d < 3600) return Math.floor(d / 60) + " min ago";
        if (d < 86400) return Math.floor(d / 3600) + " h ago";
        return Math.floor(d / 86400) + " d ago";
    }

    readonly property string syncText: {
        if (!alState) return "";
        if (alState.needs_reauth) return "Login expired. Reconnect to resume syncing";
        if (alState.syncing) return "Syncing…";
        let parts = [];
        if (alState.online === 0) parts.push("Offline");
        if ((alState.pending_count || 0) > 0) {
            parts.push(alState.pending_count + (alState.pending_count === 1 ? " change waiting to sync" : " changes waiting to sync"));
        }
        if (parts.length === 0) parts.push("Synced " + relTime(alState.last_sync_at));
        else if (alState.last_sync_at) parts.push("last sync " + relTime(alState.last_sync_at));
        return parts.join(" · ");
    }

    // ------------------------------------------------------------ not connected
    readonly property bool setupRunning: Boolean(connectInfo && connectInfo.running)

    Flickable {
        visible: !alRoot.connected || alRoot.setupRunning
        anchors.fill: parent
        contentHeight: connectCol.height + 60
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        Column {
            id: connectCol
            x: 40
            y: 36
            width: parent.width - 80
            spacing: 26

            Text {
                text: "Connect AniList"
                font.pixelSize: 38
                font.bold: true
                color: "#000000"
            }

            Text {
                width: parent.width
                wrapMode: Text.WordWrap
                font.pixelSize: 23
                color: "#222222"
                text: "Your reading progress updates on AniList automatically. Your lists show up here so you " +
                      "can see where you stopped. Everything also works offline: changes are saved and " +
                      "synced the next time Wi-Fi is on. If both sides changed, you get asked."
            }

            // Step card (QR) while the setup page is running
            Rectangle {
                visible: Boolean(alRoot.connectInfo && alRoot.connectInfo.running)
                width: parent.width
                height: visible ? qrRow.height + 48 : 0
                color: "#FFFFFF"
                border.width: 3
                border.color: "#000000"

                Row {
                    id: qrRow
                    x: 24
                    y: 24
                    spacing: 32

                    // QR code drawn from backend module rows
                    Rectangle {
                        id: qrBox
                        readonly property var rows: (alRoot.connectInfo && alRoot.connectInfo.qr) ? alRoot.connectInfo.qr : []
                        readonly property int mod: rows.length > 0 ? Math.floor(400 / (rows.length + 8)) : 8
                        width: (rows.length + 8) * mod
                        height: width
                        color: "#FFFFFF"

                        Column {
                            x: 4 * qrBox.mod
                            y: 4 * qrBox.mod
                            Repeater {
                                model: qrBox.rows
                                delegate: Row {
                                    readonly property string rowStr: modelData
                                    Repeater {
                                        model: rowStr.length
                                        delegate: Rectangle {
                                            width: qrBox.mod
                                            height: qrBox.mod
                                            color: rowStr.charAt(index) === "1" ? "#000000" : "#FFFFFF"
                                        }
                                    }
                                }
                            }
                        }
                    }

                    Column {
                        width: connectCol.width - qrBox.width - 32 - 48
                        spacing: 16

                        Text {
                            width: parent.width
                            wrapMode: Text.WordWrap
                            text: "1. Scan this with your phone's camera"
                            font.pixelSize: 25
                            font.bold: true
                        }
                        Text {
                            width: parent.width
                            wrapMode: Text.WrapAnywhere
                            text: "or open: " + ((alRoot.connectInfo && alRoot.connectInfo.urls && alRoot.connectInfo.urls.length > 0)
                                                 ? alRoot.connectInfo.urls[0] : "")
                            font.pixelSize: 21
                            color: "#333333"
                        }
                        Text {
                            width: parent.width
                            wrapMode: Text.WordWrap
                            text: "2. Log in to AniList on your phone and paste the token it shows into that page."
                            font.pixelSize: 23
                        }
                        Text {
                            width: parent.width
                            wrapMode: Text.WordWrap
                            text: "Your phone needs to be on the same Wi-Fi as the reMarkable. This screen updates by itself once you're connected."
                            font.pixelSize: 20
                            color: "#555555"
                        }
                    }
                }
            }

            Text {
                visible: Boolean(alRoot.connectInfo && alRoot.connectInfo.error)
                width: parent.width
                wrapMode: Text.WordWrap
                text: "⚠ " + ((alRoot.connectInfo && alRoot.connectInfo.error) ? alRoot.connectInfo.error : "")
                font.pixelSize: 23
                font.bold: true
            }

            Rectangle {
                width: 420
                height: 84
                color: connectArea.pressed ? "#FFFFFF" : "#000000"
                border.width: 3
                border.color: "#000000"
                Text {
                    anchors.centerIn: parent
                    text: (alRoot.connectInfo && alRoot.connectInfo.running) ? "Cancel" : "Connect AniList  →"
                    font.pixelSize: 27
                    font.bold: true
                    color: connectArea.pressed ? "#000000" : "#FFFFFF"
                }
                MouseArea {
                    id: connectArea
                    anchors.fill: parent
                    onClicked: (mouse) => alRoot.requestConnect(!(alRoot.connectInfo && alRoot.connectInfo.running))
                }
            }
        }
    }

    // ------------------------------------------------------------ connected
    Item {
        visible: alRoot.connected && !alRoot.setupRunning
        anchors.fill: parent

        Column {
            id: headCol
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.margins: 24
            spacing: 16

            Item {
                width: parent.width
                height: 64

                Column {
                    anchors.left: parent.left
                    anchors.right: headBtns.left
                    anchors.rightMargin: 16
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 4
                    Text {
                        text: "AniList · " + ((alRoot.alState && alRoot.alState.user_name) ? alRoot.alState.user_name : "")
                        font.pixelSize: 28
                        font.bold: true
                        elide: Text.ElideRight
                        width: parent.width
                    }
                    Text {
                        text: alRoot.syncText
                        font.pixelSize: 20
                        color: "#444444"
                        elide: Text.ElideRight
                        width: parent.width
                    }
                }

                Row {
                    id: headBtns
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 12

                    Rectangle {
                        visible: Boolean(alRoot.alState && alRoot.alState.needs_reauth)
                        width: 190
                        height: 58
                        color: reauthArea.pressed ? "#FFFFFF" : "#000000"
                        border.width: 2
                        border.color: "#000000"
                        Text {
                            anchors.centerIn: parent
                            text: "Reconnect"
                            font.pixelSize: 21
                            font.bold: true
                            color: reauthArea.pressed ? "#000000" : "#FFFFFF"
                        }
                        MouseArea {
                            id: reauthArea
                            anchors.fill: parent
                            onClicked: (mouse) => alRoot.requestConnect(true)
                        }
                    }

                    Rectangle {
                        width: 150
                        height: 58
                        color: syncBtnArea.pressed ? "#000000" : "#FFFFFF"
                        border.width: 2
                        border.color: "#000000"
                        Text {
                            anchors.centerIn: parent
                            text: (alRoot.alState && alRoot.alState.syncing) ? "Syncing…" : "↻ Sync"
                            font.pixelSize: 21
                            font.bold: true
                            color: syncBtnArea.pressed ? "#FFFFFF" : "#000000"
                        }
                        MouseArea {
                            id: syncBtnArea
                            anchors.fill: parent
                            onClicked: (mouse) => alRoot.requestSync()
                        }
                    }

                    Rectangle {
                        width: alRoot.confirmLogout ? 250 : 170
                        height: 58
                        color: (alRoot.confirmLogout || logoutArea.pressed) ? "#000000" : "#FFFFFF"
                        border.width: 2
                        border.color: "#000000"
                        Text {
                            anchors.centerIn: parent
                            text: alRoot.confirmLogout ? "Tap to disconnect" : "Disconnect"
                            font.pixelSize: 20
                            font.bold: true
                            color: (alRoot.confirmLogout || logoutArea.pressed) ? "#FFFFFF" : "#000000"
                        }
                        MouseArea {
                            id: logoutArea
                            anchors.fill: parent
                            onClicked: (mouse) => {
                                if (alRoot.confirmLogout) {
                                    alRoot.confirmLogout = false;
                                    alRoot.requestLogout();
                                } else {
                                    alRoot.confirmLogout = true;
                                    logoutResetTimer.restart();
                                }
                            }
                        }
                        Timer {
                            id: logoutResetTimer
                            interval: 4000
                            onTriggered: () => { alRoot.confirmLogout = false; }
                        }
                    }
                }
            }

            // Conflict banner
            Rectangle {
                visible: alRoot.alState && (alRoot.alState.conflict_count || 0) > 0
                width: parent.width
                height: visible ? 74 : 0
                color: conflictBannerArea.pressed ? "#FFFFFF" : "#000000"
                border.width: 2
                border.color: "#000000"
                Text {
                    anchors.centerIn: parent
                    text: "⚠ " + ((alRoot.alState && alRoot.alState.conflict_count) || 0) +
                          ((alRoot.alState && alRoot.alState.conflict_count === 1) ? " entry changed on both sides. Tap to choose" :
                                                                               " entries changed on both sides. Tap to choose")
                    font.pixelSize: 22
                    font.bold: true
                    color: conflictBannerArea.pressed ? "#000000" : "#FFFFFF"
                }
                MouseArea {
                    id: conflictBannerArea
                    anchors.fill: parent
                    onClicked: (mouse) => alRoot.showConflicts()
                }
            }

            // Status filter pills
            Flow {
                width: parent.width
                spacing: 10
                Repeater {
                    model: alRoot.filters
                    delegate: Rectangle {
                        id: stPill
                        readonly property bool sel: alRoot.statusFilter === modelData.id
                        readonly property int cnt: alRoot.countFor(modelData.id)
                        visible: cnt > 0 || modelData.id === "CURRENT" || sel
                        width: visible ? stTxt.implicitWidth + 32 : 0
                        height: 54
                        color: (sel || stArea.pressed) ? "#000000" : "#FFFFFF"
                        border.width: 2
                        border.color: "#000000"
                        Text {
                            id: stTxt
                            anchors.centerIn: parent
                            text: modelData.label + " (" + stPill.cnt + ")"
                            font.pixelSize: 21
                            font.bold: stPill.sel
                            color: (stPill.sel || stArea.pressed) ? "#FFFFFF" : "#000000"
                        }
                        MouseArea {
                            id: stArea
                            anchors.fill: parent
                            onClicked: (mouse) => { alRoot.statusFilter = modelData.id; }
                        }
                    }
                }
            }
        }

        Text {
            visible: alRoot.filteredEntries.length === 0
            anchors.top: headCol.bottom
            anchors.topMargin: 60
            anchors.horizontalCenter: parent.horizontalCenter
            width: parent.width - 120
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            font.pixelSize: 24
            color: "#444444"
            text: (alRoot.alState && alRoot.alState.last_sync_at)
                  ? "Nothing in this list."
                  : "Your list appears here after the first sync (needs Wi-Fi)."
        }

        ListView {
            id: entryList
            anchors.top: headCol.bottom
            anchors.topMargin: 14
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.leftMargin: 24
            anchors.rightMargin: 24
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            model: alRoot.filteredEntries

            delegate: Rectangle {
                width: entryList.width
                height: 150
                color: rowArea.pressed ? "#EAEAEA" : "#FFFFFF"

                Rectangle {
                    anchors.bottom: parent.bottom
                    width: parent.width
                    height: 1
                    color: "#BBBBBB"
                }

                Rectangle {
                    id: thumb
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    width: 92
                    height: 130
                    color: "#F0F0F0"
                    border.width: 1
                    border.color: "#000000"
                    clip: true
                    Text {
                        anchors.centerIn: parent
                        visible: thumbImg.status !== Image.Ready
                        text: modelData.title ? modelData.title.slice(0, 1).toUpperCase() : "?"
                        font.pixelSize: 36
                        font.bold: true
                    }
                    Image {
                        id: thumbImg
                        anchors.fill: parent
                        anchors.margins: 1
                        source: modelData.cover_path || ""
                        fillMode: Image.PreserveAspectCrop
                        asynchronous: true
                    }
                }

                Column {
                    anchors.left: thumb.right
                    anchors.leftMargin: 20
                    anchors.right: nextBox.left
                    anchors.rightMargin: 16
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 6

                    Text {
                        width: parent.width
                        text: modelData.title_english && modelData.title_english !== modelData.title
                              ? modelData.title + "  ·  " + modelData.title_english : modelData.title
                        font.pixelSize: 25
                        font.bold: true
                        elide: Text.ElideRight
                        maximumLineCount: 2
                        wrapMode: Text.WordWrap
                    }
                    Text {
                        width: parent.width
                        font.pixelSize: 21
                        color: "#222222"
                        text: {
                            let s = "Ch. " + (modelData.progress || 0);
                            if (modelData.chapters > 0) s += " / " + modelData.chapters;
                            if (modelData.conflict) s += "  ·  ⚠ needs decision";
                            else if (modelData.pending) s += "  ·  ⟳ waiting to sync";
                            return s;
                        }
                    }
                    Text {
                        width: parent.width
                        font.pixelSize: 19
                        color: "#555555"
                        elide: Text.ElideRight
                        text: modelData.link
                              ? ("Reading on " + (modelData.link.source_name || modelData.link.source_id))
                              : "Not linked to a source yet. Tap to find it"
                    }
                }

                Rectangle {
                    id: nextBox
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    width: 200
                    height: 70
                    color: rowArea.pressed ? "#FFFFFF" : (modelData.link ? "#000000" : "#FFFFFF")
                    border.width: 2
                    border.color: "#000000"
                    Text {
                        anchors.centerIn: parent
                        text: modelData.link
                              ? ((modelData.chapters > 0 && modelData.progress >= modelData.chapters)
                                 ? "Open" : ("▶ Ch. " + ((modelData.progress || 0) + 1)))
                              : "Find  ⌕"
                        font.pixelSize: 22
                        font.bold: true
                        color: (modelData.link && !rowArea.pressed) ? "#FFFFFF" : "#000000"
                    }
                }

                MouseArea {
                    id: rowArea
                    anchors.fill: parent
                    onClicked: (mouse) => {
                        if (modelData.link) {
                            alRoot.openLinked(modelData);
                        } else {
                            alRoot.findEntry = modelData;
                            alRoot.requestFindSources(modelData);
                        }
                    }
                }
            }
        }
    }

    // ------------------------------------------------------------ "find on sources" sheet
    Rectangle {
        visible: alRoot.findEntry !== null
        anchors.fill: parent
        color: "#80000000"
        z: 40

        MouseArea { anchors.fill: parent; onClicked: (mouse) => { alRoot.findEntry = null; } }

        Rectangle {
            anchors.centerIn: parent
            width: parent.width - 80
            height: parent.height - 120
            color: "#FFFFFF"
            border.width: 3
            border.color: "#000000"

            MouseArea { anchors.fill: parent }  // swallow taps

            Column {
                id: findHead
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.margins: 24
                spacing: 8

                Item {
                    width: parent.width
                    height: 60
                    Text {
                        anchors.left: parent.left
                        anchors.right: closeFind.left
                        anchors.verticalCenter: parent.verticalCenter
                        text: "Find “" + (alRoot.findEntry ? alRoot.findEntry.title : "") + "”"
                        font.pixelSize: 27
                        font.bold: true
                        elide: Text.ElideRight
                    }
                    Rectangle {
                        id: closeFind
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        width: 130
                        height: 56
                        color: closeFindArea.pressed ? "#000000" : "#FFFFFF"
                        border.width: 2
                        border.color: "#000000"
                        Text {
                            anchors.centerIn: parent
                            text: "✕ Close"
                            font.pixelSize: 21
                            font.bold: true
                            color: closeFindArea.pressed ? "#FFFFFF" : "#000000"
                        }
                        MouseArea {
                            id: closeFindArea
                            anchors.fill: parent
                            onClicked: (mouse) => { alRoot.findEntry = null; }
                        }
                    }
                }
                Text {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    font.pixelSize: 20
                    color: "#444444"
                    text: "Pick the series on one of your sources. It gets linked, so progress on AniList and on the device stays in sync."
                }

                // Which sources to search (tap to toggle, none selected = all)
                Flow {
                    id: srcFilterFlow
                    width: parent.width
                    spacing: 8

                    Rectangle {
                        readonly property bool sel: alRoot.sourceFilterSel.length === 0
                        width: allTxt.implicitWidth + 28
                        height: 46
                        color: (sel || allArea.pressed) ? "#000000" : "#FFFFFF"
                        border.width: 2
                        border.color: "#000000"
                        Text {
                            id: allTxt
                            anchors.centerIn: parent
                            text: "All sources"
                            font.pixelSize: 18
                            font.bold: true
                            color: (parent.sel || allArea.pressed) ? "#FFFFFF" : "#000000"
                        }
                        MouseArea {
                            id: allArea
                            anchors.fill: parent
                            onClicked: (mouse) => {
                                alRoot.sourceFilterSel = [];
                                alRoot.requestFindSources(alRoot.findEntry, [], alRoot.sourcesProbing);
                            }
                        }
                    }

                    Repeater {
                        model: alRoot.installedSources
                        delegate: Rectangle {
                            id: srcPill
                            readonly property bool sel: alRoot.sourceFilterSel.indexOf(modelData.id) >= 0
                            width: pillTxt.implicitWidth + 28
                            height: 46
                            color: (sel || pillArea2.pressed) ? "#000000" : "#FFFFFF"
                            border.width: 2
                            border.color: "#000000"
                            Text {
                                id: pillTxt
                                anchors.centerIn: parent
                                text: modelData.name || modelData.id
                                font.pixelSize: 18
                                font.bold: srcPill.sel
                                color: (srcPill.sel || pillArea2.pressed) ? "#FFFFFF" : "#000000"
                            }
                            MouseArea {
                                id: pillArea2
                                anchors.fill: parent
                                onClicked: (mouse) => {
                                    let next = alRoot.sourceFilterSel.slice();
                                    let at = next.indexOf(modelData.id);
                                    if (at >= 0) next.splice(at, 1);
                                    else next.push(modelData.id);
                                    alRoot.sourceFilterSel = next;
                                    alRoot.requestFindSources(alRoot.findEntry, next, alRoot.sourcesProbing);
                                }
                            }
                        }
                    }
                }
            }

            Text {
                visible: alRoot.sourcesLoading || (alRoot.sourceResults.length === 0)
                anchors.top: findHead.bottom
                anchors.topMargin: 60
                anchors.horizontalCenter: parent.horizontalCenter
                width: parent.width - 80
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                font.pixelSize: 24
                text: alRoot.sourcesLoading ? "Searching your sources…" : (alRoot.sourcesError || "No results")
            }

            ListView {
                id: srcList
                visible: !alRoot.sourcesLoading
                anchors.top: findHead.bottom
                anchors.topMargin: 12
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: 24
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                model: alRoot.sourceResults
                delegate: Rectangle {
                    width: srcList.width
                    height: 96
                    color: srcArea.pressed ? "#EAEAEA" : "#FFFFFF"
                    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: "#BBBBBB" }
                    Column {
                        anchors.left: parent.left
                        anchors.leftMargin: 8
                        anchors.right: srcGo.left
                        anchors.rightMargin: 12
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 4
                        Text {
                            width: parent.width
                            text: modelData.title
                            font.pixelSize: 24
                            font.bold: true
                            elide: Text.ElideRight
                        }
                        Text {
                            width: parent.width
                            color: modelData.chapter_count === 0 ? "#000000" : "#555555"
                            font.bold: modelData.chapter_count === 0
                            font.pixelSize: 19
                            elide: Text.ElideRight
                            text: {
                                let parts = [];
                                if (modelData.chapter_count > 0) parts.push(modelData.chapter_count + " chapters");
                                else if (modelData.chapter_count === 0) parts.push("no chapters (licensed or login needed)");
                                parts.push(modelData.source_name || modelData.source_id);
                                if (modelData.author) parts.push(modelData.author);
                                if (modelData.in_library) parts.push("In Library");
                                return parts.join("  ·  ");
                            }
                        }
                    }
                    Text {
                        id: srcGo
                        anchors.right: parent.right
                        anchors.rightMargin: 8
                        anchors.verticalCenter: parent.verticalCenter
                        text: "Link & open →"
                        font.pixelSize: 21
                        font.bold: true
                    }
                    MouseArea {
                        id: srcArea
                        anchors.fill: parent
                        onClicked: (mouse) => {
                            let e = alRoot.findEntry;
                            alRoot.findEntry = null;
                            alRoot.linkAndOpen(e, modelData);
                        }
                    }
                }
            }
        }
    }
}
