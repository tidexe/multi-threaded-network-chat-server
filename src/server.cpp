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
#include <csignal>
#include <cerrno>

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
    std::thread worker; // thread handling this client (moved-into after creation)
};

std::vector<std::shared_ptr<Client>> clients;
std::mutex clients_mutex;
std::atomic<bool> running{true};

// Global listen socket so signal handler can close it during shutdown
socket_t g_listen_sock = INVALID_SOCKET;

// Signal handler declared later (defined after close_socket)
void on_signal(int);

// Helper: close socket cross-platform
void close_socket(socket_t s)
{
#if defined(_WIN32)
    closesocket(s);
#else
    close(s);
#endif
}

// Wait until socket is readable/writable (returns true if ready)
bool wait_for_socket(socket_t s, bool for_write, int timeout_ms = 5000)
{
    fd_set readfds;
    fd_set writefds;
    FD_ZERO(&readfds);
    FD_ZERO(&writefds);
    if (for_write)
        FD_SET(s, &writefds);
    else
        FD_SET(s, &readfds);
    timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
#if defined(_WIN32)
    int rv = select(0, &readfds, &writefds, nullptr, &tv);
#else
    int rv = select(static_cast<int>(s) + 1, &readfds, &writefds, nullptr, &tv);
#endif
    return rv > 0;
}

// Signal handler to trigger graceful shutdown (definition placed after close_socket)
void on_signal(int)
{
    running = false;
    if (g_listen_sock != INVALID_SOCKET)
    {
        close_socket(g_listen_sock);
        g_listen_sock = INVALID_SOCKET;
    }
}

bool send_all(socket_t s, const char *data, size_t len)
{
    size_t sent = 0;
    while (sent < len)
    {
#if defined(_WIN32)
        int n = send(s, data + sent, static_cast<int>(len - sent), 0);
        if (n == SOCKET_ERROR)
        {
            int err = WSAGetLastError();
            if (err == WSAEINTR)
                continue;
            if (err == WSAEWOULDBLOCK)
            {
                if (!wait_for_socket(s, true))
                {
                    std::cerr << "send_all: socket not writable (WSAEWOULDBLOCK)\n";
                    return false;
                }
                continue;
            }
            std::cerr << "send_all: send failed, WSA error=" << err << "\n";
            return false;
        }
        if (n <= 0)
            return false;
        sent += static_cast<size_t>(n);
#else
        ssize_t n = ::send(s, data + sent, len - sent, 0);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                if (!wait_for_socket(s, true))
                {
                    std::cerr << "send_all: socket not writable (EAGAIN)\n";
                    return false;
                }
                continue;
            }
            std::cerr << "send_all: send failed, errno=" << errno << "\n";
            return false;
        }
        if (n == 0)
            return false;
        sent += static_cast<size_t>(n);
#endif
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
        if (n == SOCKET_ERROR)
        {
            int err = WSAGetLastError();
            if (err == WSAEINTR)
                continue;
            if (err == WSAEWOULDBLOCK)
            {
                if (!wait_for_socket(s, false))
                {
                    std::cerr << "recv_all: socket not readable (WSAEWOULDBLOCK)\n";
                    return false;
                }
                continue;
            }
            std::cerr << "recv_all: recv failed, WSA error=" << err << "\n";
            return false;
        }
        if (n <= 0)
            return false;
        recvd += static_cast<size_t>(n);
#else
        ssize_t n = ::recv(s, data + recvd, len - recvd, 0);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                if (!wait_for_socket(s, false))
                {
                    std::cerr << "recv_all: socket not readable (EAGAIN)\n";
                    return false;
                }
                continue;
            }
            std::cerr << "recv_all: recv failed, errno=" << errno << "\n";
            return false;
        }
        if (n == 0)
            return false;
        recvd += static_cast<size_t>(n);
#endif
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
            std::string who = c->name.empty() ? "anonymous" : c->name;
            std::cerr << "broadcast: failed to send to " << who << " (sock=" << c->sock << ")\n";
            close_socket(c->sock);
            it = clients.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

// Send the current online user list to the given client (excluding that client)
// Returns false if sending failed (caller should treat as client disconnected)
bool send_user_list_to_client(const std::shared_ptr<Client> &client)
{
    std::lock_guard<std::mutex> lk(clients_mutex);
    std::string list;
    bool first = true;
    for (auto &c : clients)
    {
        if (c->sock == client->sock)
            continue;
        std::string n = c->name.empty() ? "anonymous" : c->name;
        if (!first)
            list += ", ";
        list += n;
        first = false;
    }
    if (list.empty())
        list = "（无其他在线用户）";
    else
        list = "在线用户: " + list;

    // Send the assembled user list directly to the new client
    return send_message(client->sock, list);
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
        // First send the current online user list to this client
        if (!send_user_list_to_client(client))
        {
            std::cerr << "Failed to send user list to client; closing\n";
            close_socket(client->sock);
            std::lock_guard<std::mutex> lk(clients_mutex);
            clients.erase(std::remove_if(clients.begin(), clients.end(),
                                         [&](const std::shared_ptr<Client> &c)
                                         { return c->sock == client->sock; }),
                          clients.end());
            return;
        }

        // Announce join to others
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

    // install simple signal handler for graceful shutdown (Ctrl-C)
    std::signal(SIGINT, on_signal);

    socket_t listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock == INVALID_SOCKET)
    {
        std::cerr << "socket() failed\n";
        return 1;
    }

    // store global listen socket for signal handler
    g_listen_sock = listen_sock;

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
        // start worker thread and store it in the client object so we can join later
        c->worker = std::thread(handle_client, c);
    }

    // shutdown
    std::cout << "Shutting down server..." << std::endl;

    // notify clients that server is shutting down
    broadcast("Server", "服务器正在关闭");

    {
        std::lock_guard<std::mutex> lk(clients_mutex);
        for (auto &c : clients)
            close_socket(c->sock);
        // do NOT clear clients here — we need their thread objects for joining
    }

    // close listen socket (if not already closed by signal handler)
    if (g_listen_sock != INVALID_SOCKET)
    {
        close_socket(g_listen_sock);
        g_listen_sock = INVALID_SOCKET;
    }

    // join worker threads stored in client objects
    {
        std::vector<std::shared_ptr<Client>> copy;
        {
            std::lock_guard<std::mutex> lk(clients_mutex);
            copy = clients; // copy shared_ptrs to keep clients alive while joining
        }
        for (auto &c : copy)
        {
            try
            {
                if (c->worker.joinable())
                    c->worker.join();
            }
            catch (const std::system_error &e)
            {
                std::cerr << "Error joining client thread: " << e.what() << std::endl;
            }
        }

        // now safe to clear clients container
        std::lock_guard<std::mutex> lk(clients_mutex);
        clients.clear();
    }
#if defined(_WIN32)
    WSACleanup();
#endif

    return 0;
}
