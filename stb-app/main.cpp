#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QProcess>
#include <QQmlContext>

class VideoPlayer : public QObject {
    Q_OBJECT
public:
    explicit VideoPlayer(QObject *parent = nullptr) : QObject(parent) {}

    Q_INVOKABLE void start(const QString &program, const QStringList &arguments) {
        if (m_process.state() == QProcess::NotRunning) {
            m_process.start(program, arguments);
        }
    }

    Q_INVOKABLE void stop() {
        if (m_process.state() == QProcess::Running) {
            m_process.terminate();
            m_process.waitForFinished(3000);
            if (m_process.state() == QProcess::Running) {
                m_process.kill();
            }
        }
    }

    Q_INVOKABLE void executeCommand(const QString &command) {
    QStringList args = command.split(' ');
    QString program = args.takeFirst();
    QProcess::execute(program, args);
}


private:
    QProcess m_process;
};

#include "main.moc"

int main(int argc, char *argv[]) {
    QGuiApplication app(argc, argv);
    QQmlApplicationEngine engine;

    VideoPlayer videoPlayer;
    engine.rootContext()->setContextProperty("videoPlayer", &videoPlayer);
    
    engine.load(QUrl(QStringLiteral("qrc:/main.qml")));
    return app.exec();
}