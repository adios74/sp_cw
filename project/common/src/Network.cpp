#include "../include/Network.h"

Socket::Socket() : fd_(-1) {}

Socket::~Socket() { close(); }

void Socket::close() {
    if (fd_ != -1) {
        ::close(fd_);
        fd_ = -1;
    }
}

void Socket::bind(int port) {
    fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) throw std::runtime_error("socket creation failed");

    int opt = 1;
    if (setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
        throw std::runtime_error("setsockopt failed");

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (::bind(fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0)
        throw std::runtime_error("bind failed");
}

void Socket::listen(int backlog) {
    if (::listen(fd_, backlog) < 0)
        throw std::runtime_error("listen failed");
}

int Socket::accept() {
    struct sockaddr_in client_addr;
    socklen_t len = sizeof(client_addr);
    int client_fd = ::accept(fd_, (struct sockaddr*)&client_addr, &len);
    if (client_fd < 0)
        throw std::runtime_error("accept failed");
    return client_fd;
}

void Socket::connect(const std::string& host, int port) {
    fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) throw std::runtime_error("socket creation failed");

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0)
        throw std::runtime_error("invalid address");

    if (::connect(fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0)
        throw std::runtime_error("connect failed");
}

void Socket::send(const std::string& msg) {
    size_t total = 0;
    while (total < msg.size()) {
        ssize_t sent = ::send(fd_, msg.data() + total, msg.size() - total, 0);
        if (sent <= 0) throw std::runtime_error("send failed");
        total += sent;
    }
}

std::string Socket::recv() {
    const size_t BUFFER_SIZE = 4096;
    char buffer[BUFFER_SIZE];
    std::string result;
    while (true) {
        ssize_t received = ::recv(fd_, buffer, BUFFER_SIZE - 1, 0);
        if (received <= 0) break;
        buffer[received] = '\0';
        result += buffer;
        if (received < BUFFER_SIZE - 1) break;
    }
    return result;
    
}

void Socket::shutdownWrite() {
    if (fd_ != -1) {
        ::shutdown(fd_, SHUT_WR);
    }
}

void Socket::setTimeout(int seconds) {
    struct timeval tv;
    tv.tv_sec = seconds;
    tv.tv_usec = 0;
    setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

