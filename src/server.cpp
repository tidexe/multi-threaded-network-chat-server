// Simple multi-threaded TCP chat server
// Cross-platform (Windows / POSIX)

#include <iostream>
#include <thread>
#include <vector>
#include <string>
#include <mutex>
#include <memory>
#include <atomic>
#include <algorithm>
#include <cstring>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET socket_t;
#pragma comment(lib, "Ws2_32.lib")
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
typedef int socket_t;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#endif

static const uint16_t DEFAULT_PORT = 5555;

struct Client
{
    socket_t sock;
    std::string name;
};

std::vector<std::shared_ptr<Client>> clients;
std::mutex clients_mutex;
std::atomic<bool> running{true};

// Helper: close socket cross-platform
void close_socket(socket_t s)
{
#if defined(_WIN32)
    closesocket(s);
#else
    close(s);
#endif
}

bool send_all(socket_t s, const char *data, size_t len)
{
    size_t sent = 0;
    while (sent < len)
    {
#if defined(_WIN32)
        int n = send(s, data + sent, static_cast<int>(len - sent), 0);
#else
        ssize_t n = ::send(s, data + sent, len - sent, 0);
#endif
        if (n <= 0)
            return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool recv_all(socket_t s, char *data, size_t len)
{
    size_t recvd = 0;
    while (recvd < len)
    {
#if defined(_WIN32)
        int n = recv(s, data + recvd, static_cast<int>(len - recvd), 0);
#else
        ssize_t n = ::recv(s, data + recvd, len - recvd, 0);
#endif
        if (n <= 0)
            return false;
        recvd += static_cast<size_t>(n);
    }
    return true;
}

// Message framing: 4-byte big-endian length + payload
bool send_message(socket_t s, const std::string &msg)
{
    uint32_t len = static_cast<uint32_t>(msg.size());
    uint32_t be = htonl(len);
    if (!send_all(s, reinterpret_cast<const char *>(&be), sizeof(be)))
        return false;
    if (len > 0)
    {
        if (!send_all(s, msg.data(), len))
            return false;
    }
    return true;
}

bool recv_message(socket_t s, std::string &out)
{
    uint32_t be;
    if (!recv_all(s, reinterpret_cast<char *>(&be), sizeof(be)))
        return false;
    uint32_t len = ntohl(be);
    if (len == 0)
    {
        out.clear();
        return true;
    }
    out.resize(len);
    if (!recv_all(s, out.data(), len))
        return false;
    return true;
}

void broadcast(const std::string &from, const std::string &msg)
{
    std::string full = "[" + from + "] " + msg;
    std::lock_guard<std::mutex> lk(clients_mutex);
    for (auto it = clients.begin(); it != clients.end();)
    {
        auto c = *it;
        if (!send_message(c->sock, full))
        {
            // failed to send -> remove client
            close_socket(c->sock);
            it = clients.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void handle_client(std::shared_ptr<Client> client)
{
    try
    {
        // First message is username
        std::string name;
        if (!recv_message(client->sock, name))
        {
            std::cerr << "Failed username recv; closing client\n";
            close_socket(client->sock);
            std::lock_guard<std::mutex> lk(clients_mutex);
            clients.erase(std::remove_if(clients.begin(), clients.end(),
                                         [&](const std::shared_ptr<Client> &c)
                                         { return c->sock == client->sock; }),
                          clients.end());
            return;
        }
        client->name = name.empty() ? "anonymous" : name;
        std::cout << "Client connected: " << client->name << std::endl;
        // Announce join
        broadcast("Server", "用户 '" + client->name + "' 已加入聊天");

        // Loop receiving messages
        std::string msg;
        while (running && recv_message(client->sock, msg))
        {
            if (msg == "__quit__")
                break;
            broadcast(client->name, msg);
        }
    }
    catch (...)
    {
        // swallow
    }
    // cleanup
    std::cout << "Client disconnected: " << client->name << std::endl;
    close_socket(client->sock);
    {
        std::lock_guard<std::mutex> lk(clients_mutex);
        clients.erase(std::remove_if(clients.begin(), clients.end(),
                                     [&](const std::shared_ptr<Client> &c)
                                     { return c->sock == client->sock; }),
                      clients.end());
    }
    broadcast("Server", "用户 '" + client->name + "' 已离开聊天");
}

int main(int argc, char *argv[])
{
    uint16_t port = DEFAULT_PORT;
    if (argc >= 2)
    {
        port = static_cast<uint16_t>(std::stoi(argv[1]));
    }

#if defined(_WIN32)
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        std::cerr << "WSAStartup failed\n";
        return 1;
    }
#endif

    socket_t listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock == INVALID_SOCKET)
    {
        std::cerr << "socket() failed\n";
        return 1;
    }

    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char *>(&opt), sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(listen_sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == SOCKET_ERROR)
    {
        std::cerr << "bind() failed\n";
        close_socket(listen_sock);
        return 1;
    }

    if (listen(listen_sock, SOMAXCONN) == SOCKET_ERROR)
    {
        std::cerr << "listen() failed\n";
        close_socket(listen_sock);
        return 1;
    }

    std::cout << "Chat server listening on port " << port << std::endl;

    // Accept loop
    while (running)
    {
        sockaddr_in peer{};
        socklen_t plen = sizeof(peer);
        socket_t client_sock = accept(listen_sock, reinterpret_cast<sockaddr *>(&peer), &plen);
        if (client_sock == INVALID_SOCKET)
        {
            if (!running)
                break;
            std::cerr << "accept() failed\n";
            continue;
        }

        auto c = std::make_shared<Client>();
        c->sock = client_sock;
        {
            std::lock_guard<std::mutex> lk(clients_mutex);
            clients.push_back(c);
        }
        std::thread t(handle_client, c);
        t.detach();
    }

    // shutdown
    std::cout << "Shutting down server..." << std::endl;
    {
        std::lock_guard<std::mutex> lk(clients_mutex);
        for (auto &c : clients)
            close_socket(c->sock);
        clients.clear();
    }

    close_socket(listen_sock);
#if defined(_WIN32)
    WSACleanup();
#endif

    return 0;
}
