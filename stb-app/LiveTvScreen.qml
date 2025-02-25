import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15

Rectangle {
    id: liveTvScreen
    anchors.fill: parent
    color: "transparent"
    focus: true
    signal closed()

    Component.onCompleted: {
        forceActiveFocus()
        // Delay start slightly to ensure layout is complete
        Qt.callLater(startMpvPlayer)
    }

    Component.onDestruction: {
        stopMpvPlayer()
    }

    function startMpvPlayer() {
        // Calculate position based on the video container
        var pos = videoContainer.mapToGlobal(0, 0)
        console.log("Starting MPV at:", pos.x, pos.y)
        
        videoPlayer.start("mpv", [
            "--vo=x11",
            "--ao=alsa",
            "--audio-device=alsa/hw:1,0",
            "--audio-channels=2",
            "--audio-format=s16",
            "--audio-samplerate=48000",
            "--no-border",
            "--geometry=" + videoContainer.width + "x" + videoContainer.height + "+" + 
                          Math.round(pos.x) + "+" + Math.round(pos.y),
            "--no-keepaspect-window",
            "udp://224.2.2.2:10000"
        ])
    }

    function stopMpvPlayer() {
        videoPlayer.stop()
    }

    Image {
        anchors.fill: parent
        source: "file:///root/dashboard/background.jpg"
        fillMode: Image.PreserveAspectCrop
        opacity: 0.3
    }

    Rectangle {
        anchors.fill: parent
        gradient: Gradient {
            GradientStop { position: 0.0; color: "#1a1a3a" }
            GradientStop { position: 1.0; color: "#0f0f1e" }
        }
        opacity: 0.85
    }

    Keys.onPressed: {
        if (event.key === Qt.Key_Backspace || event.key === Qt.Key_Back) {
            stopMpvPlayer()
            closed()
            event.accepted = true
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 20
        spacing: 10

        RowLayout {
            Layout.fillWidth: true
            spacing: 20

            Image {
                source: "file:///root/dashboard/logo.png"
                Layout.preferredWidth: 100
                Layout.preferredHeight: 50
                fillMode: Image.PreserveAspectFit
            }

            Text {
                text: "TV Guide"
                color: "white"
                font.pixelSize: 32
                font.weight: Font.Bold
            }

            Item { Layout.fillWidth: true }

            Text {
                id: clockText
                text: Qt.formatDateTime(new Date(), "h:mm AP")
                color: "white"
                font.pixelSize: 24
            }
        }

        RowLayout {
        Layout.fillWidth: true
        spacing: 20

        Rectangle {
            id: videoContainer
            Layout.preferredWidth: 640
            Layout.preferredHeight: 360
            color: "#1a1a1a"  // Changed from transparent to see the container
            
            // Add position change handling
            onXChanged: Qt.callLater(updateMpvPosition)
            onYChanged: Qt.callLater(updateMpvPosition)
            
            function updateMpvPosition() {
                if (videoPlayer.running) {
                    var pos = mapToGlobal(0, 0)
                    console.log("Updating MPV position to:", pos.x, pos.y)
                    videoPlayer.sendCommand(["set_property", "geometry", 
                        width + "x" + height + "+" + 
                        Math.round(pos.x) + "+" + Math.round(pos.y)])
                }
            }
        }
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 360
                color: "#1a1a1a"
                opacity: 0.7
                radius: 8
                
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 15
                    spacing: 10

                    Text {
                        text: "Channel 1"
                        color: "white"
                        font.pixelSize: 24
                        font.weight: Font.Bold
                    }

                    Row {
                        spacing: 10
                        Rectangle {
                            width: 40
                            height: 20
                            color: "#2196F3"
                            radius: 4
                            Text {
                                anchors.centerIn: parent
                                text: "LIVE"
                                color: "white"
                                font.pixelSize: 12
                            }
                        }
                        Text {
                            text: "001 Test Pattern | Live TV"
                            color: "white"
                            font.pixelSize: 16
                        }
                    }

                    Text {
                        text: "Test Pattern Video Feed"
                        color: "#cccccc"
                        font.pixelSize: 16
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                    }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: "#1a1a1a"
            opacity: 0.7
            radius: 8

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 15
                spacing: 0

                Row {
                    spacing: 20
                    Repeater {
                        model: ["All", "Entertainment", "Kids", "Lifestyle", "Movies", "Music", "News", "Sports"]
                        Text {
                            text: modelData
                            color: "white"
                            font.pixelSize: 16
                        }
                    }
                }

                Row {
                    Layout.fillWidth: true
                    Layout.topMargin: 20
                    spacing: 20
                    Repeater {
                        model: ["9:30 AM", "10:00 AM", "10:30 AM", "11:00 AM"]
                        Text {
                            text: modelData
                            color: "#888888"
                            font.pixelSize: 14
                        }
                    }
                }

                ListView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: ListModel {
                        ListElement { channel: "001"; name: "Test"; program: "Test Pattern" }
                        ListElement { channel: "002"; name: "Test 2"; program: "Test Pattern 2" }
                        ListElement { channel: "003"; name: "Test 3"; program: "Test Pattern 3" }
                        ListElement { channel: "004"; name: "Test 4"; program: "Test Pattern 4" }
                        ListElement { channel: "005"; name: "Test 5"; program: "Test Pattern 5" }
                    }
                    delegate: Rectangle {
                        width: parent.width
                        height: 50
                        color: "transparent"
                        border.color: "#333333"
                        border.width: 1

                        RowLayout {
                            anchors.fill: parent
                            anchors.margins: 10
                            spacing: 10

                            Text {
                                text: channel
                                color: "#888888"
                                font.pixelSize: 14
                            }

                            Image {
                                Layout.preferredWidth: 30
                                Layout.preferredHeight: 30
                                source: "file:///root/dashboard/channels/" + channel + ".png"
                                fillMode: Image.PreserveAspectFit
                            }

                            Text {
                                text: program
                                color: "white"
                                font.pixelSize: 14
                                Layout.fillWidth: true
                            }
                        }
                    }
                }
            }
        }
    }

    Timer {
        interval: 1000
        running: true
        repeat: true
        onTriggered: {
            clockText.text = Qt.formatDateTime(new Date(), "h:mm AP")
        }
    }
}