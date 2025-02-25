import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import QtQml 2.15

ApplicationWindow {
    visible: true
    width: 1920
    height: 1080
    visibility: "FullScreen"
    
    property int currentIndex: 0

    FontLoader {
        id: fontAwesome
        source: "fonts/fa-solid-900.ttf"
    }

    Rectangle {
        id: mainRect
        anchors.fill: parent
        focus: true
        color: "#ffffff"

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

        Keys.onLeftPressed: currentIndex = Math.max(0, currentIndex - 1)
        Keys.onRightPressed: currentIndex = Math.min(menuRepeater.count - 1, currentIndex + 1)
        Keys.onReturnPressed: {
            if (menuRepeater.model.get(currentIndex).name === "Live TV") {
                var component = Qt.createComponent("qrc:/LiveTvScreen.qml")
                if (component.status === Component.Ready) {
                    mainRect.visible = false
                    var screen = component.createObject(mainRect.parent)
                    screen.closed.connect(function() {
                        screen.destroy()
                        mainRect.visible = true
                        mainRect.focus = true
                    })
                }
            }
        }

        ColumnLayout {
            anchors {
                fill: parent
                margins: 40
            }
            spacing: 40

            Row {
                spacing: 1000
                Layout.fillWidth: true

                Image {
                    source: "file:///root/dashboard/logo.png"
                    fillMode: Image.PreserveAspectFit
                    width: 400
                }

                Row {
                    spacing: 10
                    layoutDirection: Qt.RightToLeft
                    Layout.alignment: Qt.AlignRight
                    Layout.fillWidth: true

                    Text {
                        id: clockText
                        text: Qt.formatDateTime(new Date(), "hh:mm")
                        color: "#ffffff"
                        font.pixelSize: 38
                        font.weight: Font.Bold
                    }
                    Text {
                        id: clockText2
                        text: Qt.formatDateTime(new Date(), "ddd MMMM d yyyy ")
                        color: "#ffffff"
                        font.pixelSize: 20
                        font.weight: Font.Bold
                    }
                }
            }            

            Row {
                Layout.alignment: Qt.AlignHCenter
                spacing: 40
                Repeater {
                    id: menuRepeater
                    model: ListModel {
                        ListElement { name: "Live TV"; icon: "\uf26c"; color: "#4287f5" }
                        ListElement { name: "VOD"; icon: "\uf008"; color: "#f542a1" }
                        ListElement { name: "Apps"; icon: "\uf3cd"; color: "#42f5b3" }
                        ListElement { name: "Market"; icon: "\uf290"; color: "#f5d442" }
                        ListElement { name: "Settings"; icon: "\uf013"; color: "#a142f5" }
                    }
                    Rectangle {
                        width: 250
                        height: 250
                        color: "#33ffffff"
                        border.color: index === currentIndex ? model.color : "transparent"
                        border.width: 4
                        radius: 40                        
                        
                        ColumnLayout {
                            anchors.centerIn: parent
                            spacing: 20
                            Text {
                                text: model.icon
                                font.pixelSize: 64
                                Layout.alignment: Qt.AlignHCenter
                            }

                            Text {
                                text: model.name
                                color: index === currentIndex ? model.color : "white"
                                font.pixelSize: 24
                                font.weight: Font.DemiBold
                                Layout.alignment: Qt.AlignHCenter
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
            clockText.text = Qt.formatDateTime(new Date(), "hh:mm")
        }
    }
}
