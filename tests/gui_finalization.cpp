#include "mainwindow.h"
#include <QApplication>
#include <QFile>
#include <QMessageBox>
#include <QPushButton>
#include <QStatusBar>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

extern std::atomic<bool> release_finalization;
extern std::atomic<int> recording_starts;

static void require(bool value, const char *message) {
	if (!value) throw std::runtime_error(message);
}
static void pump(int milliseconds) {
	QElapsedTimer timer;
	timer.start();
	do {
		QApplication::processEvents();
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	} while (timer.elapsed() < milliseconds);
}
static QByteArray command(QTcpSocket &socket, const char *text) {
	socket.write(text);
	socket.flush();
	QElapsedTimer deadline;
	deadline.start();
	while (!socket.bytesAvailable() && deadline.elapsed() < 3000)
		pump(1);
	return socket.readAll();
}

int main(int argc, char **argv) {
	QApplication app(argc, argv);
	try {
		QTemporaryDir directory;
		QTcpServer reservation;
		require(reservation.listen(QHostAddress::LocalHost, 0), "cannot reserve port");
		const auto port = reservation.serverPort();
		reservation.close();
		const auto config = directory.filePath("test.cfg");
		QFile file(config);
		require(file.open(QIODevice::WriteOnly), "cannot write config");
		file.write(("StudyRoot=" + directory.path() +
					"\nPathTemplate=test.xdf\nRCSEnabled=1\nRCSPort=" + QString::number(port) +
					'\n')
					   .toUtf8());
		file.close();
		const auto configBytes = config.toUtf8();
		MainWindow window(nullptr, configBytes.constData());
		window.show();
		QTcpSocket socket;
		socket.connectToHost(QHostAddress::LocalHost, port);
		require(socket.waitForConnected(1000), "cannot connect remote control");
		require(command(socket, "start\n") == "OK", "start was not accepted");
		require(recording_starts == 1, "recording was not started");
		int heartbeats = 0;
		QTimer heartbeat;
		QObject::connect(&heartbeat, &QTimer::timeout, [&] { ++heartbeats; });
		heartbeat.start(10);
		QElapsedTimer stop;
		stop.start();
		require(command(socket, "stop\n") == "OK", "stop was not accepted");
		require(stop.elapsed() < 200, "stop blocked the GUI");
		require(command(socket, "status\n") == "finishing\n", "premature stopped status");
		require(command(socket, "start\n") == "ERROR finishing\n", "start allowed while finishing");
		auto *start = window.findChild<QPushButton *>("startButton");
		auto *stopButton = window.findChild<QPushButton *>("stopButton");
		require(start && stopButton && !start->isEnabled(), "Start enabled before completion");
		pump(5200);
		require(heartbeats > 100, "GUI event loop stopped during stalled finalization");
		require(command(socket, "status\n") == "stalled\n", "stall was not reported");
		require(stopButton->isEnabled() && stopButton->text().contains("Force quit"),
				"force-quit action unavailable");
		// Closing while stalled must offer an escape, with Keep waiting as the safe default.
		const bool forceQuit = argc > 1 && std::string(argv[1]) == "--force-quit";
		QTimer::singleShot(0, [forceQuit] {
			for (auto *widget : QApplication::topLevelWidgets())
				if (auto *dialog = qobject_cast<QMessageBox *>(widget)) {
					if (forceQuit) {
						for (auto *button : dialog->buttons())
							if (button->text() == "Force quit") button->click();
					} else dialog->reject();
				}
		});
		window.close();
		require(!forceQuit, "Force quit did not terminate the process");
		require(window.isVisible(), "Keep waiting closed the window");
		release_finalization = true;
		pump(300);
		require(command(socket, "status\n") == "stopped\n", "completion not reported");
		require(!window.isVisible(), "pending close was not completed");
		std::cout << "GUI stayed responsive, rejected restart, and closed only after completion\n";
		return 0;
	} catch (const std::exception &e) {
		release_finalization = true;
		std::cerr << e.what() << '\n';
		return 1;
	}
}
