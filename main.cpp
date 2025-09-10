#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <poll.h>
#include <unistd.h>

#include <cstring>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

using namespace std;

static constexpr int kDefaultPort = 6380;
static constexpr int maxPending = 128;
static constexpr int kMaxEvents = 1024;
static constexpr int kReadChunk = 4096;

struct Connection {
    string inbuf;
    string outbuf;
    bool closed = false;
};

int setNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int createListenSocket(int port)
{
    int serverSocket = socket(AF_INET, SOCK_STREAM, 0);

    if (serverSocket < 0)
    {
        printf("Error in opening a socket");
        exit(0);
    }
    printf("Client Socket Created\n");

    int opt = 1;
    setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));


    struct sockaddr_in serverAddress;

    memset(&serverAddress, 0, sizeof(serverAddress));
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_port = htons(port);
    serverAddress.sin_addr.s_addr = htonl(INADDR_ANY);
    printf("Server address assigned\n");

    int temp = ::bind(serverSocket, (struct sockaddr *)&serverAddress, sizeof(serverAddress));
    if (temp < 0)
    {
        printf("Error while binding\n");
        exit(0);
    }
    printf("Binding successful\n");


    int temp1 = listen(serverSocket, maxPending);
    if (temp1 < 0)
    {
        printf("Error in listen");
        exit(0);
    }
    printf("Now Listening\n");

    if (setNonBlocking(serverSocket) < 0) {
        perror("fcntl(O_NONBLOCK)");
        close(serverSocket);
        return -1;
    }


    return serverSocket;
}

int addClient(int client_fd, vector<pollfd> &pfds, unordered_map<int, Connection> &conns) {
    setNonBlocking(client_fd);
    pfds.push_back({client_fd, POLLIN, 0});
    conns.emplace(client_fd, Connection{});
    cout << "Client " << client_fd << " connected\n";
}

int removeClient(int index,  vector<pollfd> &pfds, unordered_map<int, Connection> &conns) {
    int fd = pfds[index].fd;
    cout << "Client " << fd << " disconnected\n";
    conns.erase(fd);
    close(fd);
    // swap-remove from pfds
    pfds[index] = pfds.back();
    pfds.pop_back();
}

int main(int argc, char** argv) {
    int port = (argc > 2) ? stoi(argv[1]) : kDefaultPort;
    int lfd = createListenSocket(port);
    
    // pollfd list: index 0 is always the listening socket
    vector<pollfd> pfds;
    pfds.reserve(kMaxEvents);
    pfds.push_back({lfd, POLLIN, 0});

    unordered_map<int, Connection> conns;

    while (true) {
        // Update POLLOUT flags for sockets with pending output
        for (size_t i = 1; i < pfds.size(); ++i) {
            int fd = pfds[i].fd;
            auto it = conns.find(fd);
            if (it != conns.end()) {
                if (!it->second.outbuf.empty()) {
                    pfds[i].events = POLLIN | POLLOUT;
                } else {
                    pfds[i].events = POLLIN;
                }
            }
        }

        int n = poll(pfds.data(), pfds.size(), /*timeout_ms=*/1000);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }

        // 1) New connections?
        if (pfds[0].revents & POLLIN) {
            while (true) {
                sockaddr_in cli{};
                socklen_t len = sizeof(cli);
                int cfd = accept(lfd, reinterpret_cast<sockaddr*>(&cli), &len);
                if (cfd < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    perror("accept");
                    break;
                }
                addClient(cfd, pfds, conns);
            }
        }

        // 2) Handle client events
        for (int i = (pfds.size()) - 1; i >= 1; --i) {
            auto &p = pfds[i];
            if (!(p.revents)) continue;

            // Errors/hangups
            if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                removeClient(i, pfds, conns);
                continue;
            }

            // Readable
            if (p.revents & POLLIN) {
                char buf[kReadChunk];
                while (true) {
                    ssize_t r = ::read(p.fd, buf, sizeof(buf));
                    if (r > 0) {
                        // Echo server: push read bytes to outbuf
                        auto &conn = conns[p.fd];
                        conn.inbuf.append(buf, r);
                        conn.outbuf.append(buf, r);
                    } else if (r == 0) {
                        // Client closed
                        removeClient(i, pfds, conns);
                        goto next_fd; // pfds[i] is now swapped; skip to next
                    } else {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        perror("read");
                        removeClient(i, pfds, conns);
                        goto next_fd;
                    }
                }
            }

            // Writable
            if (p.revents & POLLOUT) {
                auto it = conns.find(p.fd);
                if (it != conns.end() && !it->second.outbuf.empty()) {
                    const char* data = it->second.outbuf.data();
                    size_t len = it->second.outbuf.size();
                    ssize_t w = ::write(p.fd, data, len);
                    if (w > 0) {
                        it->second.outbuf.erase(0, static_cast<size_t>(w));
                    } else if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                        perror("write");
                        removeClient(i, pfds, conns);
                        goto next_fd;
                    }
                }
            }
        next_fd:;
        }
    }
 
     close(lfd);
     return 0;
}
