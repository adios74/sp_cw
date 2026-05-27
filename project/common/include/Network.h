#ifndef NETWORK_H
#define NETWORK_H

#include <string>
#include <stdexcept>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

class Socket {
public:
	Socket();
	~Socket();
	void close();

	// Серверные методы
	void bind(int port);
	void listen(int backlog = 5);
	int accept();

	// Клиентские методы
	void connect(const std::string& host, int port);

	// Отправка/получение строк
	void send(const std::string& msg);
	std::string recv();

	// Для передачи дескриптора клиенту
	int getFd() const { return fd_; }
	void setFd(int fd) { fd_ = fd; }

	void shutdownWrite();                     // закрывает канал записи
    void setTimeout(int seconds);

private:
	int fd_;
};

#endif