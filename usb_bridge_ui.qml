import QtQuick 2.12
import QtQuick.Window 2.12

Window {
    id: root
    width: 480
    height: 320
    visible: true
    visibility: Window.FullScreen
    flags: Qt.FramelessWindowHint
    color: "#05070d"

    property string phase: "service_start"
    property string scanText: "-"
    property string patientId: "-"
    property string reportNo: "-"
    property int selectedIndex: 0
    property int projectCount: 0
    property var projectOptions: []
    property int errorCount: 0
    property string statusTime: "-"

    function selectedProject() {
        if (projectOptions && selectedIndex >= 0 && selectedIndex < projectOptions.length)
            return projectOptions[selectedIndex]
        return "-"
    }

    function statusTitle() {
        if (phase === "service_start") return "\u670d\u52a1\u542f\u52a8"
        if (phase === "idle") return "\u7b49\u5f85\u626b\u7801"
        if (phase === "scan") return "\u67e5\u8be2\u63a5\u53e3"
        if (phase === "select_project") return "\u9009\u62e9\u9879\u76ee"
        if (phase === "ready_input" || phase === "inputting") return "\u6b63\u5728\u5f55\u5165"
        if (phase === "wait_print") return "\u7b49\u5f85\u6253\u5370"
        if (phase === "printing") return "\u63a5\u6536\u62a5\u544a"
        if (phase === "print_done" || phase === "converting") return "\u8f6c\u5b58PDF"
        if (phase === "uploading") return "\u4e0a\u4f20\u4e91\u7aef"
        if (phase === "done") return "\u6d41\u7a0b\u5b8c\u6210"
        if (phase === "error") return "\u6d41\u7a0b\u5f02\u5e38"
        return "\u7cfb\u7edf\u8fd0\u884c"
    }

    function statusColor() {
        if (phase === "error") return "#ff4d4d"
        if (phase === "select_project") return "#66ccff"
        if (phase === "ready_input" || phase === "inputting") return "#ffd166"
        if (phase === "done") return "#2ef58a"
        return "#2ef58a"
    }

    function hidText() {
        if (phase === "ready_input" || phase === "inputting") return "BUSY"
        if (phase === "wait_print" || phase === "printing" || phase === "uploading" || phase === "done") return "OK"
        return "WAIT"
    }

    function printerText() {
        if (phase === "wait_print") return "WAIT"
        if (phase === "printing") return "RECV"
        if (phase === "print_done" || phase === "converting" || phase === "uploading" || phase === "done") return "OK"
        return "READY"
    }

    function uploadText() {
        if (phase === "uploading") return "BUSY"
        if (phase === "done") return "DONE"
        return "READY"
    }

    function loadStatus() {
        var xhr = new XMLHttpRequest()
        xhr.onreadystatechange = function() {
            if (xhr.readyState === XMLHttpRequest.DONE && (xhr.status === 200 || xhr.status === 0)) {
                try {
                    var s = JSON.parse(xhr.responseText)
                    root.phase = s.phase || "idle"
                    root.scanText = s.scan_text || "-"
                    root.patientId = s.patient_id || "-"
                    root.reportNo = s.report_no || "-"
                    root.statusTime = s.time || "-"
                    root.selectedIndex = s.selected_index || 0
                    root.projectOptions = s.project_options || []
                    root.projectCount = s.project_count || root.projectOptions.length
                    root.errorCount = root.phase === "error" ? root.errorCount + 1 : root.errorCount
                } catch(e) {}
            }
        }
        xhr.open("GET", "file:///root/usb_bridge/state/ui_status.json?t=" + Date.now())
        xhr.send()
    }

    Timer {
        interval: 500
        running: true
        repeat: true
        onTriggered: {
            timeText.text = Qt.formatTime(new Date(), "hh:mm:ss")
            loadStatus()
        }
    }

    Rectangle {
        id: panel
        width: 480
        height: 320
        anchors.centerIn: parent
        color: "#05070d"
        clip: true

    Rectangle {
        anchors.fill: parent
        gradient: Gradient {
            GradientStop { position: 0.0; color: phase === "error" ? "#2b1113" : "#101827" }
            GradientStop { position: 1.0; color: "#02030a" }
        }
    }

    Text {
        id: title
        text: "USB Bridge"
        color: "white"
        font.pixelSize: 28
        font.bold: true
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: 16
    }

    Text {
        text: statusTitle()
        color: statusColor()
        font.pixelSize: 15
        font.bold: true
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: title.bottom
        anchors.topMargin: 3
    }

    Item {
        id: ring
        width: 150
        height: 150
        anchors.left: parent.left
        anchors.leftMargin: 24
        anchors.verticalCenter: parent.verticalCenter

        Rectangle {
            anchors.centerIn: parent
            width: 138
            height: 138
            radius: 69
            color: "transparent"
            border.width: 8
            border.color: "#162033"
        }

        Rectangle {
            id: dot
            width: 16
            height: 16
            radius: 8
            color: statusColor()
            anchors.horizontalCenter: parent.horizontalCenter
            y: 0
        }

        RotationAnimator {
            target: dot
            from: 0
            to: 360
            duration: phase === "idle" ? 3200 : 1800
            loops: Animation.Infinite
            running: phase !== "error" && phase !== "done"
        }

        Text {
            text: phase === "select_project" ? ((selectedIndex + 1) + "/" + Math.max(projectCount, projectOptions.length)) :
                  phase === "error" ? "ERROR" :
                  phase === "done" ? "DONE" : "RUNNING"
            color: statusColor()
            font.pixelSize: 21
            font.bold: true
            anchors.centerIn: parent
        }
    }

    Column {
        id: statusColumn
        spacing: 10
        anchors.left: ring.right
        anchors.leftMargin: 22
        anchors.right: parent.right
        anchors.rightMargin: 20
        anchors.verticalCenter: parent.verticalCenter
        visible: phase !== "select_project"

        Rectangle {
            width: parent.width
            height: 42
            radius: 14
            color: "#111827"
            border.color: "#243047"

            Text {
                text: "\u626b\u7801  " + root.scanText
                color: "white"
                font.pixelSize: 15
                elide: Text.ElideRight
                anchors.verticalCenter: parent.verticalCenter
                anchors.left: parent.left
                anchors.leftMargin: 14
                width: parent.width - 28
            }
        }

        Rectangle {
            width: parent.width
            height: 42
            radius: 14
            color: "#111827"
            border.color: "#243047"

            Text {
                text: "HID  " + hidText()
                color: "#2ef58a"
                font.pixelSize: 17
                font.bold: true
                anchors.verticalCenter: parent.verticalCenter
                anchors.left: parent.left
                anchors.leftMargin: 14
            }
        }

        Rectangle {
            width: parent.width
            height: 42
            radius: 14
            color: "#111827"
            border.color: "#243047"

            Text {
                text: "\u6253\u5370  " + printerText()
                color: "#ffd166"
                font.pixelSize: 17
                font.bold: true
                anchors.verticalCenter: parent.verticalCenter
                anchors.left: parent.left
                anchors.leftMargin: 14
            }
        }

        Rectangle {
            width: parent.width
            height: 42
            radius: 14
            color: "#111827"
            border.color: "#243047"

            Text {
                text: "\u4e0a\u4f20  " + uploadText()
                color: "#66ccff"
                font.pixelSize: 17
                font.bold: true
                anchors.verticalCenter: parent.verticalCenter
                anchors.left: parent.left
                anchors.leftMargin: 14
            }
        }
    }

    Column {
        spacing: 7
        anchors.left: ring.right
        anchors.leftMargin: 18
        anchors.right: parent.right
        anchors.rightMargin: 18
        anchors.verticalCenter: parent.verticalCenter
        visible: phase === "select_project"

        Text {
            text: "\u9009\u9879\u6765\u81ea exam_item"
            color: "#7f8da3"
            font.pixelSize: 13
            font.bold: true
        }

        Repeater {
            model: projectOptions

            Rectangle {
                width: parent.width
                height: Math.max(24, Math.min(36, 142 / Math.max(1, projectOptions.length)))
                radius: 10
                color: index === selectedIndex ? "#14532d" : "#111827"
                border.width: index === selectedIndex ? 2 : 1
                border.color: index === selectedIndex ? "#2ef58a" : "#243047"

                Text {
                    text: (index + 1) + ". " + modelData
                    color: "white"
                    font.pixelSize: projectOptions.length > 4 ? 12 : 14
                    font.bold: index === selectedIndex
                    elide: Text.ElideRight
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: parent.left
                    anchors.leftMargin: 10
                    width: parent.width - 20
                }
            }
        }

        Text {
            text: "\u6309\u952e\uff1a\u4e0a\u4e00\u4e2a / \u4e0b\u4e00\u4e2a / \u786e\u8ba4"
            color: "#d7deea"
            font.pixelSize: 13
            font.bold: true
        }
    }

    Text {
        id: timeText
        text: Qt.formatTime(new Date(), "hh:mm:ss")
        color: "#d7deea"
        font.pixelSize: 22
        font.bold: true
        anchors.left: parent.left
        anchors.leftMargin: 26
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 18
    }

    Text {
        text: root.phase + "  " + root.statusTime
        color: root.errorCount === 0 ? "#7f8da3" : "#ff4d4d"
        font.pixelSize: 12
        font.bold: true
        anchors.right: parent.right
        anchors.rightMargin: 26
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 20
    }
    }

    Component.onCompleted: loadStatus()
}
