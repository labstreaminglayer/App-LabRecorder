#include "tcpinterface.h"
#include <QDebug>

RemoteControlSocket::RemoteControlSocket(uint16_t port) : server() {
	server.listen(QHostAddress::Any, port);
	connect(&server, &QTcpServer::newConnection, this, &RemoteControlSocket::addClient);
}

void RemoteControlSocket::addClient() {
	auto *client = server.nextPendingConnection();
	clients.push_back(client);
	connect(client, &QTcpSocket::readyRead, this, [this, client]() {
		while(client->canReadLine())
			this->handleLine(client->readLine().trimmed(), client);
	});
}

void RemoteControlSocket::handleLine(QString s, QTcpSocket *sock) {
	qInfo() << s;
	if (s == "start") {
		emit start(sock);
		return;
	} else if (s == "stop")
		emit stop();
	else if (s == "update")
			emit refresh_streams();
	else if (s == "filename" || s.startsWith("filename ")) {
		emit filename(s);
	} else if (s == "select all") {
		emit select_all();
	} else if (s == "select none") {
		emit select_none();
	} else if (s.startsWith("select ")) {
		emit select_stream(s.mid(QStringLiteral("select ").size()), sock);
		return;
	} else {
		sock->write("ERROR unknown command");
		return;
	}
	sock->write("OK");
	// TODO: select /deselect streams
	// TODO: send acknowledgement
	// TODO: get current state
	//
	// else this->sender()->sender("Whoops");
}
