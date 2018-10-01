#include "tcpinterface.h"
#include <QDebug>

RemoteControlSocket::RemoteControlSocket(uint16_t port) : server() {
	server.listen(QHostAddress::Any, port);
	connect(&server, &QTcpServer::newConnection, this, &RemoteControlSocket::addClient);
}

void RemoteControlSocket::addClient() {
	auto* client = server.nextPendingConnection();
	clients.push_back(client);
	connect(client, &QTcpSocket::readyRead, [this, client]() {
		if(!client->canReadLine()) return;
		QString line(client->readLine());
		qInfo() << line;
		line = line.trimmed();
		this->handleLine(line, client);
	});
}

void RemoteControlSocket::handleLine(QString s, QTcpSocket* sock) {
	qInfo() << s;
	if (s == "start")
		emit start();
	else if (s == "stop")
		emit stop();
	sock->write("OK");
	//TODO: set filename
	//TODO: select /deselect streams
	//TODO: send acknowledgement
	//TODO: get current state
	//
	// else this->sender()->sender("Whoops");
}
