import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import QtQuick.Window
import QtWebEngine
import QtWebChannel
import StashNative 1.0

ApplicationWindow {
    id: win
    width: 1440
    height: 900
    minimumWidth: 640
    minimumHeight: 400
    visible: true
    color: "#000000"
    title: web.title ? web.title : "Stash"
    // The toolbar below doubles as the title bar (drag to move, double-click
    // to maximize). Windows: no native title bar, our own window buttons on
    // the right. macOS: the native frame with its traffic-light buttons on
    // the left, over the toolbar (see winframe_mac.mm).
    readonly property bool mac: Qt.platform.os === "osx"
    flags: mac ? Qt.Window : Qt.Window | Qt.FramelessWindowHint
    font.family: mac ? "PingFang TC" : "Microsoft JhengHei UI"

    readonly property bool maximized: visibility === Window.Maximized
    readonly property bool fullScreen: visibility === Window.FullScreen

    property bool setupVisible: !app.hasServer
    property int visibilityBeforeFullscreen: Window.Windowed

    WebEngineProfile {
        id: profile
        storageName: "StashNativePlayer"
        persistentCookiesPolicy: WebEngineProfile.ForcePersistentCookies
        httpCacheType: WebEngineProfile.DiskHttpCache
    }

    WebChannel {
        id: channel
    }

    // Native video layer, positioned under the hole the Stash plugin leaves in
    // the page. Coordinates arrive in CSS pixels; scale by the page zoom.
    MpvItem {
        id: video
        visible: app.bridge.videoVisible && !win.setupVisible
        x: web.x + app.bridge.videoRect.x * web.zoomFactor
        y: web.y + app.bridge.videoRect.y * web.zoomFactor
        width: Math.max(1, app.bridge.videoRect.width * web.zoomFactor)
        height: Math.max(1, app.bridge.videoRect.height * web.zoomFactor)
    }

    // Mouse wheel over the video seeks 1 s per notch (down = forward, up = back). Takes no
    // buttons or hover, so clicks and mouse moves still reach the page's player
    // controls. Ctrl+wheel is left to the page (zoom).
    MouseArea {
        z: 5
        x: video.x; y: video.y; width: video.width; height: video.height
        visible: video.visible
        acceptedButtons: Qt.NoButton
        property real acc: 0
        onWheel: (wheel) => {
            if (wheel.modifiers & Qt.ControlModifier) { wheel.accepted = false; return; }
            acc += wheel.angleDelta.y !== 0 ? wheel.angleDelta.y : wheel.angleDelta.x;
            const steps = acc > 0 ? Math.floor(acc / 120) : Math.ceil(acc / 120);
            if (steps !== 0) {
                acc -= steps * 120;
                app.bridge.seekRelative(-steps * 1.0);
            }
            wheel.accepted = true;
        }
    }

    // --- toolbar --------------------------------------------------------------
    Rectangle {
        id: toolbar
        anchors { left: parent.left; right: parent.right; top: parent.top }
        // macOS: about the height of a standard title bar, so the traffic
        // lights sit centred in it
        height: visible ? (win.mac ? 30 : 34) : 0
        visible: !win.fullScreen
        color: "#182026"
        z: 10

        // Empty toolbar space acts as the title bar.
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton
            onPressed: win.startSystemMove()
            onDoubleClicked: win.maximized ? win.showNormal() : win.showMaximized()
        }

        component ToolBtn: ToolButton {
            implicitWidth: 34
            implicitHeight: 30
            font.pixelSize: 16
            palette.buttonText: enabled ? "#e1e8ed" : "#5c7080"
            ToolTip.visible: hovered && ToolTip.text !== ""
            ToolTip.delay: 600
        }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: win.mac ? 78 : 4 // macOS: room for the traffic lights
            anchors.rightMargin: win.mac ? 6 : 0
            spacing: 2

            ToolBtn { text: "←"; visible: !win.setupVisible; enabled: web.canGoBack; onClicked: web.goBack(); ToolTip.text: "Back (Alt+Left)" }
            ToolBtn { text: "→"; visible: !win.setupVisible; enabled: web.canGoForward; onClicked: web.goForward(); ToolTip.text: "Forward (Alt+Right)" }
            ToolBtn {
                visible: !win.setupVisible && app.hasServer
                text: web.loading ? "✕" : "↻"
                onClicked: web.loading ? web.stop() : web.reload()
                ToolTip.text: web.loading ? "Stop" : "Reload (F5)"
            }

            Label {
                Layout.fillWidth: true
                Layout.leftMargin: 8
                text: win.title
                color: "#8a9ba8"
                font.pixelSize: 12
                elide: Text.ElideRight
            }

            // Live subtitles (speech recognition + translation, all local)
            ToolButton {
                id: subsBtn
                visible: app.hasServer && !win.setupVisible
                implicitHeight: 30
                readonly property var labels: ["字幕 關", "字幕 原文", "字幕 中文", "字幕 雙語"]
                text: labels[app.liveSubs.mode] + (app.liveSubs.status !== "" && app.liveSubs.mode !== 0 ? " …" : "")
                      + (app.liveSubs.genActive && !app.liveSubs.genPaused ? "  ⟳" : "") + "  ▾"
                palette.buttonText: app.liveSubs.mode !== 0 ? "#e8eef2" : "#a7b6c2"
                font.pixelSize: 12
                onClicked: subsMenu.open()
                ToolTip.visible: hovered
                ToolTip.delay: 300
                ToolTip.text: [app.liveSubs.status, app.liveSubs.genStatus,
                               app.liveSubs.source === "stash" ? "字幕來自 Stash" : "",
                               "Live subtitles (Ctrl+Shift+L to cycle)"].filter(t => t !== "").join("\n")

                Menu {
                    id: subsMenu
                    y: subsBtn.height
                    width: 360
                    Repeater {
                        model: ["關閉 Off", "原文 Original", "中文翻譯 Chinese", "雙語 Both"]
                        MenuItem {
                            text: (index === app.liveSubs.mode ? "✓  " : "     ") + modelData
                            onTriggered: app.liveSubs.mode = index
                        }
                    }
                    MenuSeparator {}
                    MenuItem {
                        text: "     產生整部字幕（這部影片）"
                        enabled: app.liveSubs.hasScene && app.liveSubs.source !== "stash"
                        onTriggered: app.liveSubs.queueCurrentScene()
                    }
                    MenuItem {
                        text: "     所有沒有字幕的影片加入佇列…"
                        onTriggered: queueAllDlg.ask()
                    }
                    MenuItem {
                        text: "     處理字幕佇列（標籤「產生字幕」）"
                        enabled: !app.liveSubs.genActive && !app.liveSubs.genPaused
                        onTriggered: app.liveSubs.checkQueue()
                    }
                    MenuItem {
                        text: (app.liveSubs.genPaused ? "✓  " : "     ") + "暫停產生字幕"
                        onTriggered: app.liveSubs.genPaused = !app.liveSubs.genPaused
                    }
                    MenuSeparator {}
                    MenuItem {
                        text: "     字幕設定（模型）…"
                        onTriggered: { app.liveSubs.refreshModels(); subsSettings.open(); }
                    }
                    MenuItem {
                        visible: app.liveSubs.genStatus !== ""
                        height: visible ? implicitHeight : 0
                        enabled: false
                        text: "     " + app.liveSubs.genStatus
                    }
                }
            }

            ToolButton {
                id: serverBtn
                visible: app.hasServer
                implicitHeight: 30
                text: "●  " + app.serverUrl.toString().replace(/^https?:\/\//, "").replace(/\/$/, "") + "  ▾"
                palette.buttonText: web.loadError !== "" ? "#f5a5a5" : "#a7b6c2"
                font.pixelSize: 12
                onClicked: serverMenu.open()
                ToolTip.visible: hovered
                ToolTip.delay: 600
                ToolTip.text: "Change server (Ctrl+Shift+S)"

                Menu {
                    id: serverMenu
                    y: serverBtn.height
                    Repeater {
                        model: app.recentServers
                        MenuItem {
                            text: (modelData === app.serverUrl.toString() ? "✓  " : "     ") + modelData
                            onTriggered: win.connectTo(modelData)
                        }
                    }
                    MenuSeparator { visible: app.recentServers.length > 0 }
                    MenuItem { text: "Other address…"; onTriggered: win.setupVisible = true }
                }
            }

            Item { implicitWidth: 8; visible: !win.mac }

            // Windows only: macOS has its own buttons on the left
            component WinBtn: ToolButton {
                visible: !win.mac
                implicitWidth: 44
                implicitHeight: 34
                font.pixelSize: 14
                palette.buttonText: "#e1e8ed"
                background: Rectangle {
                    color: parent.hovered ? (parent.danger ? "#c42b1c" : "#2f3b45") : "transparent"
                }
                property bool danger: false
            }
            WinBtn { text: "─"; onClicked: win.showMinimized() }
            WinBtn { text: win.maximized ? "❐" : "☐"; onClicked: win.maximized ? win.showNormal() : win.showMaximized() }
            WinBtn { text: "✕"; danger: true; onClicked: win.close() }
        }
    }

    // 字幕 menu → 所有沒有字幕的影片加入佇列: count first, then confirm.
    Dialog {
        id: queueAllDlg
        title: "所有沒有字幕的影片加入佇列"
        modal: true
        anchors.centerIn: Overlay.overlay
        width: Math.min(520, win.width - 40)
        property int count: -2 // -2 = counting, -1 = failed
        property double seconds: 0
        property string error: ""
        standardButtons: count > 0 ? Dialog.Ok | Dialog.Cancel : Dialog.Close

        function ask() {
            count = -2;
            open();
            app.liveSubs.countUncaptioned();
        }
        function hours(h) {
            return h >= 1 ? h.toFixed(1) + " 小時" : Math.max(1, Math.round(h * 60)) + " 分鐘";
        }
        Connections {
            target: app.liveSubs
            function onUncaptionedCounted(count, seconds, error) {
                queueAllDlg.count = count;
                queueAllDlg.seconds = seconds;
                queueAllDlg.error = error;
            }
        }
        onAccepted: app.liveSubs.queueAllUncaptioned()

        Label {
            width: parent.width
            wrapMode: Text.Wrap
            text: {
                const d = queueAllDlg;
                if (d.count === -2)
                    return "計算中…";
                if (d.count === -1)
                    return "無法查詢 Stash：" + d.error;
                if (d.count === 0)
                    return "所有影片都已經有字幕，或已經在佇列裡。";
                const h = d.seconds / 3600;
                return "找到 " + d.count + " 部沒有任何字幕的影片，總長 " + d.hours(h) + "。\n\n"
                     + "預估產生時間約 " + d.hours(h * 2 / 60) + "–" + d.hours(h * 4 / 60)
                     + "（每 1 小時影片約 2–4 分鐘，app 需要開著）。\n\n"
                     +"會替這些影片加上「產生字幕」標籤；已經在佇列，或標了「產生字幕失敗」「無對白」「內嵌字幕」「畫面字幕」「略過字幕」的不會加入。"
                     + "隨時可以用「暫停產生字幕」暫停，或在 Stash 拿掉標籤取消。";
            }
        }
    }

    // Subtitle model settings (字幕 menu → 字幕設定).
    Dialog {
        id: subsSettings
        title: "字幕設定"
        modal: true
        anchors.centerIn: Overlay.overlay
        width: Math.min(640, win.width - 40)
        standardButtons: Dialog.Close

        function llmLabel(name) {
            if (name === "none")
                return "不翻譯（只顯示原文）";
            const size = app.liveSubs.modelSize(name);
            return name.replace(/\.gguf$/i, "") + (size ? "   " + size : "");
        }
        function whisperLabel(name) {
            const size = app.liveSubs.modelSize(name);
            return name + (size ? "   " + size : "");
        }

        FolderDialog {
            id: folderPicker
            title: "選擇模型資料夾"
            currentFolder: "file:///" + app.liveSubs.modelsDir
            onAccepted: app.liveSubs.modelsDir = decodeURIComponent(selectedFolder.toString().replace(/^file:\/{3}/, ""))
        }

        component ModelCombo: ComboBox {
            id: combo
            property var names: []
            property string current: ""
            property var label: (n) => n
            signal picked(string name)
            Layout.fillWidth: true
            model: names
            currentIndex: names.indexOf(current)
            displayText: currentIndex >= 0 ? label(names[currentIndex]) : "（資料夾裡沒有模型）"
            delegate: ItemDelegate {
                required property var modelData
                required property int index
                width: ListView.view ? ListView.view.width : combo.width
                text: combo.label(modelData)
                highlighted: combo.highlightedIndex === index
            }
            onActivated: (i) => picked(names[i])
        }

        ColumnLayout {
            anchors.fill: parent
            spacing: 6

            Label { text: "模型資料夾"; font.bold: true }
            RowLayout {
                Layout.fillWidth: true
                TextField {
                    id: dirField
                    Layout.fillWidth: true
                    text: app.liveSubs.modelsDir.replace(/\//g, "\\")
                    selectByMouse: true
                    onEditingFinished: app.liveSubs.modelsDir = text
                }
                Button { text: "選擇…"; onClicked: folderPicker.open() }
                Button { text: "重新整理"; onClicked: app.liveSubs.refreshModels() }
            }

            Label { text: "語音辨識（whisper）"; font.bold: true; Layout.topMargin: 8 }
            ModelCombo {
                names: app.liveSubs.whisperModels
                current: app.liveSubs.whisperModel
                label: subsSettings.whisperLabel
                onPicked: (n) => app.liveSubs.whisperModel = n
            }

            Label { text: "即時字幕的翻譯模型"; font.bold: true; Layout.topMargin: 8 }
            ModelCombo {
                names: ["none"].concat(app.liveSubs.llmModels)
                current: app.liveSubs.liveLlm
                label: subsSettings.llmLabel
                onPicked: (n) => app.liveSubs.liveLlm = n
            }

            Label { text: "預做字幕（整部／佇列）的翻譯模型"; font.bold: true; Layout.topMargin: 8 }
            ModelCombo {
                names: ["none"].concat(app.liveSubs.llmModels)
                current: app.liveSubs.genLlm
                label: subsSettings.llmLabel
                onPicked: (n) => app.liveSubs.genLlm = n
            }

            Label { text: "預做字幕：跳過哪些影片"; font.bold: true; Layout.topMargin: 12 }
            RowLayout {
                Layout.fillWidth: true
                Label { text: "內嵌字幕" }
                ComboBox {
                    Layout.fillWidth: true
                    model: ["不管內嵌字幕（照樣產生）", "有內嵌中文字幕就跳過", "有任何內嵌字幕就跳過"]
                    currentIndex: app.liveSubs.embeddedSkip
                    onActivated: (i) => app.liveSubs.embeddedSkip = i
                }
            }
            RowLayout {
                Layout.fillWidth: true
                Label { text: "畫面字幕" }
                ComboBox {
                    Layout.fillWidth: true
                    model: ["不管畫面上的字幕（照樣產生）", "畫面已有中文字幕就跳過", "畫面已有任何字幕就跳過"]
                    currentIndex: app.liveSubs.hardsubSkip
                    onActivated: (i) => app.liveSubs.hardsubSkip = i
                }
            }
            RowLayout {
                id: skipRow
                Layout.fillWidth: true
                readonly property var codes: app.liveSubs.skipLanguages === "" ? [] : app.liveSubs.skipLanguages.split(",")
                readonly property var common: ["zh", "en", "ja", "ko"]
                function setCode(code, on) {
                    let list = codes.filter(c => c !== code);
                    if (on)
                        list.push(code);
                    app.liveSubs.skipLanguages = list.join(",");
                }
                Label { text: "對白語言是" }
                Repeater {
                    model: [["zh", "中文"], ["en", "英語"], ["ja", "日語"], ["ko", "韓語"]]
                    CheckBox {
                        required property var modelData
                        text: modelData[1]
                        checked: skipRow.codes.indexOf(modelData[0]) >= 0
                        onToggled: skipRow.setCode(modelData[0], checked)
                    }
                }
                Label { text: "其他" }
                TextField {
                    Layout.fillWidth: true
                    placeholderText: "代碼，如 fr,de"
                    text: skipRow.codes.filter(c => skipRow.common.indexOf(c) < 0).join(",")
                    selectByMouse: true
                    onEditingFinished: app.liveSubs.skipLanguages =
                        skipRow.codes.filter(c => skipRow.common.indexOf(c) >= 0).concat(text.split(/[\s,，、;]+/)).join(",")
                }
                Label { text: "就跳過" }
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: "#5c7080"
                font.pixelSize: 12
                text: "產生前會先讀影片的字幕軌、取幾段 30 秒的聲音判斷語言，並用 Windows 文字辨識看有人說話時的畫面下方。跳過的影片會標上「內嵌字幕」「畫面字幕」或「略過字幕」，"
                      + "「所有沒有字幕的影片加入佇列」不會再加入它們；改設定後想重做，拿掉標籤再加入佇列即可。"
            }

            Label {
                Layout.fillWidth: true
                Layout.topMargin: 10
                wrapMode: Text.Wrap
                color: "#5c7080"
                font.pixelSize: 12
                text: "同一時間只會載入一個翻譯模型（顯示卡記憶體放不下兩個）。兩邊選不同模型時，播放中的即時字幕優先；"
                      + "預做字幕會繼續轉錄，翻譯等播放結束再換模型接著做。\n"
                      + "目前載入的翻譯模型：" + (app.liveSubs.loadedLlm !== "" ? app.liveSubs.loadedLlm : "（無）")
            }
        }
    }

    // macOS trackpad swipes between pages (winframe_mac.mm). dir: 1 = back,
    // -1 = forward; (x, y) is where the gesture started, in window coordinates.
    function canSwipe(x, y, dir) {
        if (win.setupVisible || win.fullScreen || !web.visible || y < web.y)
            return false;
        // not over the video: there the trackpad belongs to the player
        if (video.visible && x >= video.x && x < video.x + video.width && y >= video.y && y < video.y + video.height)
            return false;
        return dir > 0 ? web.canGoBack : web.canGoForward;
    }
    function swipeNavigate(dir) {
        if (dir > 0)
            web.goBack();
        else
            web.goForward();
    }

    function connectTo(address) {
        if (!app.setServer(address))
            return false;
        win.setupVisible = false;
        web.loadError = "";
        web.url = app.serverUrl;
        return true;
    }

    WebEngineView {
        id: web
        anchors { left: parent.left; right: parent.right; top: toolbar.bottom; bottom: parent.bottom }
        visible: app.hasServer
        profile: profile
        backgroundColor: "transparent"
        webChannel: channel
        zoomFactor: app.zoomFactor
        url: app.serverUrl

        settings.fullScreenSupportEnabled: true
        settings.playbackRequiresUserGesture: false
        settings.focusOnNavigationEnabled: true

        property string loadError: ""

        Component.onCompleted: {
            const script = WebEngine.script();
            script.name = "stash-native-player";
            script.sourceCode = app.injectedScript;
            script.injectionPoint = WebEngineScript.DocumentCreation;
            script.worldId = WebEngineScript.MainWorld;
            script.runsOnSubFrames = false;
            userScripts.insert(script);
        }

        onFullScreenRequested: function(request) {
            request.accept();
            if (request.toggleOn) {
                win.visibilityBeforeFullscreen = win.visibility;
                win.showFullScreen();
            } else if (win.visibilityBeforeFullscreen === Window.Maximized) {
                win.showMaximized();
            } else {
                win.showNormal();
            }
        }

        // Links that open a new tab/window (e.g. performer URLs) go to the
        // system browser, unless they point back at the Stash server.
        onNewWindowRequested: function(request) {
            if (app.isOnServer(request.requestedUrl))
                web.url = request.requestedUrl;
            else
                Qt.openUrlExternally(request.requestedUrl);
        }

        onLoadingChanged: function(info) {
            if (info.status === WebEngineView.LoadFailedStatus && info.url.toString().startsWith(app.serverUrl.toString()))
                loadError = info.errorString || "Could not connect";
            else if (info.status === WebEngineView.LoadSucceededStatus)
                loadError = "";
        }

        onRenderProcessTerminated: function(status, code) {
            reloadTimer.start();
        }

        Timer {
            id: reloadTimer
            interval: 500
            onTriggered: web.reload()
        }
    }

    Component.onCompleted: {
        channel.registerObjects({ "stashNative": app.bridge });
        app.attach(profile, video);
    }

    // --- connection error ---------------------------------------------------
    Rectangle {
        anchors { left: parent.left; right: parent.right; top: toolbar.bottom; bottom: parent.bottom }
        visible: !win.setupVisible && web.loadError !== ""
        color: "#202b33"

        ColumnLayout {
            anchors.centerIn: parent
            spacing: 14
            Label {
                text: "Can't reach Stash at " + app.serverUrl
                color: "white"
                font.pixelSize: 20
                Layout.alignment: Qt.AlignHCenter
            }
            Label {
                text: web.loadError
                color: "#a7b6c2"
                Layout.alignment: Qt.AlignHCenter
            }
            RowLayout {
                Layout.alignment: Qt.AlignHCenter
                Button { text: "Retry"; onClicked: web.reload() }
                Button { text: "Change server"; onClicked: win.setupVisible = true }
            }
        }
    }

    // --- first-run / server setup ------------------------------------------
    Rectangle {
        id: setup
        anchors { left: parent.left; right: parent.right; top: toolbar.bottom; bottom: parent.bottom }
        visible: win.setupVisible
        color: "#202b33"

        function connect() {
            if (!win.connectTo(urlField.text)) {
                hint.text = "Enter an address like http://192.168.1.10:9999";
                hint.color = "#f5a5a5";
            }
        }

        onVisibleChanged: if (visible) { urlField.text = app.serverUrl.toString(); urlField.forceActiveFocus(); }

        ColumnLayout {
            anchors.centerIn: parent
            width: Math.min(parent.width - 48, 460)
            spacing: 12

            Label {
                text: "Stash Native Player"
                color: "white"
                font.pixelSize: 26
                font.bold: true
            }
            Label {
                text: "Address of your Stash server (e.g. 192.168.1.10:9999)"
                color: "#a7b6c2"
            }
            TextField {
                id: urlField
                Layout.fillWidth: true
                placeholderText: "http://192.168.1.10:9999"
                text: app.serverUrl.toString()
                selectByMouse: true
                focus: true
                onAccepted: setup.connect()
            }
            // Previously used servers: one click to switch.
            Flow {
                Layout.fillWidth: true
                spacing: 6
                visible: app.recentServers.length > 0
                Repeater {
                    model: app.recentServers
                    Button {
                        text: modelData.replace(/^https?:\/\//, "").replace(/\/$/, "")
                        flat: true
                        font.pixelSize: 12
                        onClicked: win.connectTo(modelData)
                    }
                }
            }
            Label {
                id: hint
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: "#a7b6c2"
                text: "Videos play in the embedded player once the \"Stash Native Player\" plugin is installed on the server."
            }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                Button {
                    text: "Cancel"
                    visible: app.hasServer
                    onClicked: win.setupVisible = false
                }
                Button {
                    text: "Connect"
                    highlighted: true
                    onClicked: setup.connect()
                }
            }
            Label {
                text: "Version " + app.version
                color: "#5c7080"
                font.pixelSize: 11
            }
        }
    }

    // --- resize handles (frameless window) -----------------------------------
    component ResizeEdge: MouseArea {
        property int edges: 0
        z: 100
        enabled: !win.maximized && !win.fullScreen
        hoverEnabled: true
        onPressed: win.startSystemResize(edges)
    }
    readonly property int grip: 5
    ResizeEdge { edges: Qt.LeftEdge; cursorShape: Qt.SizeHorCursor
        anchors { left: parent.left; top: parent.top; bottom: parent.bottom }
        width: win.grip }
    ResizeEdge { edges: Qt.RightEdge; cursorShape: Qt.SizeHorCursor
        anchors { right: parent.right; top: parent.top; bottom: parent.bottom }
        width: win.grip }
    ResizeEdge { edges: Qt.TopEdge; cursorShape: Qt.SizeVerCursor
        anchors { left: parent.left; right: parent.right; top: parent.top }
        height: win.grip }
    ResizeEdge { edges: Qt.BottomEdge; cursorShape: Qt.SizeVerCursor
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
        height: win.grip }
    ResizeEdge { edges: Qt.TopEdge | Qt.LeftEdge; cursorShape: Qt.SizeFDiagCursor
        anchors { left: parent.left; top: parent.top }
        width: win.grip * 2; height: win.grip * 2 }
    ResizeEdge { edges: Qt.TopEdge | Qt.RightEdge; cursorShape: Qt.SizeBDiagCursor
        anchors { right: parent.right; top: parent.top }
        width: win.grip * 2; height: win.grip * 2 }
    ResizeEdge { edges: Qt.BottomEdge | Qt.LeftEdge; cursorShape: Qt.SizeBDiagCursor
        anchors { left: parent.left; bottom: parent.bottom }
        width: win.grip * 2; height: win.grip * 2 }
    ResizeEdge { edges: Qt.BottomEdge | Qt.RightEdge; cursorShape: Qt.SizeFDiagCursor
        anchors { right: parent.right; bottom: parent.bottom }
        width: win.grip * 2; height: win.grip * 2 }

    // --- shortcuts ----------------------------------------------------------
    Shortcut { sequences: ["Ctrl+Shift+S"]; onActivated: win.setupVisible = true }
    Shortcut { sequences: ["Ctrl+Shift+L"]; onActivated: app.liveSubs.mode = (app.liveSubs.mode + 1) % 4 }
    Shortcut { sequences: [StandardKey.Refresh]; onActivated: web.reload() }
    Shortcut { sequences: ["Ctrl+R"]; onActivated: web.reload() }
    Shortcut { sequences: [StandardKey.Back, "Alt+Left"]; onActivated: web.goBack() }
    Shortcut { sequences: [StandardKey.Forward, "Alt+Right"]; onActivated: web.goForward() }
    Shortcut { sequences: [StandardKey.ZoomIn, "Ctrl+="]; onActivated: app.zoomFactor = app.zoomFactor + 0.1 }
    Shortcut { sequences: [StandardKey.ZoomOut]; onActivated: app.zoomFactor = app.zoomFactor - 0.1 }
    Shortcut { sequences: ["Ctrl+0"]; onActivated: app.zoomFactor = 1.0 }
    // macOS: Cmd+W closes the window (there is no File menu to provide it).
    // "Ctrl" is Cmd on macOS. Not on Windows, where Ctrl+W is not expected
    // to close an app's only window.
    Shortcut { sequences: [StandardKey.Close]; enabled: win.mac; onActivated: win.close() }
    Shortcut {
        sequences: [StandardKey.FullScreen, "F11"]
        onActivated: {
            if (win.visibility === Window.FullScreen) {
                web.triggerWebAction(WebEngineView.ExitFullScreen);
                win.showNormal();
            } else {
                win.showFullScreen();
            }
        }
    }
}
