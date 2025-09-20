#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <iostream>
#include <string>
#include <vector>

int main() {
    // Create socket
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        std::cerr << "Socket creation failed" << std::endl;
        return 1;
    }

    // Server address
    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(6380);
    inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr);

    // Connect
    if (connect(sock, (sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        std::cerr << "Connection failed" << std::endl;
        close(sock);
        return 1;
    }

    std::cout << "Connected to Redis server on port 6380" << std::endl;

    // Test commands
    std::vector<std::string> test_commands = {
        "*1\r\n$4\r\nPING\r\n",                                    // PING
        "*2\r\n$4\r\nPING\r\n$5\r\nHello\r\n",                    // PING Hello
        "*2\r\n$4\r\nECHO\r\n$11\r\nHello World\r\n",             // ECHO Hello World
        "*3\r\n$3\r\nSET\r\n$4\r\nkey1\r\n$6\r\nvalue1\r\n",     // SET key1 value1
        "*2\r\n$3\r\nGET\r\n$4\r\nkey1\r\n",                      // GET key1
        "*2\r\n$3\r\nDEL\r\n$4\r\nkey1\r\n",                      // DEL key1
        "*2\r\n$3\r\nGET\r\n$4\r\nkey1\r\n"                       // GET key1 (should be null)
    };

    std::vector<std::string> test_names = {
        "PING",
        "PING Hello", 
        "ECHO Hello World",
        "SET key1 value1",
        "GET key1",
        "DEL key1",
        "GET key1 (after delete)"
    };

    for (size_t i = 0; i < test_commands.size(); i++) {
        std::cout << "\n=== Test " << (i+1) << ": " << test_names[i] << " ===" << std::endl;
        
        // Send command
        const std::string& cmd = test_commands[i];
        std::cout << "Sending command..." << std::endl;
        
        ssize_t sent = send(sock, cmd.c_str(), cmd.length(), 0);
        if (sent < 0) {
            std::cerr << "Send failed" << std::endl;
            continue;
        }
        std::cout << "Sent " << sent << " bytes" << std::endl;

        // Receive response
        char buffer[1024] = {0};
        ssize_t received = recv(sock, buffer, sizeof(buffer) - 1, 0);
        
        if (received < 0) {
            std::cerr << "Receive failed" << std::endl;
            continue;
        } else if (received == 0) {
            std::cerr << "Connection closed by server" << std::endl;
            break;
        }

        std::cout << "Received " << received << " bytes: ";
        
        // Print response in readable format
        for (int j = 0; j < received; j++) {
            char c = buffer[j];
            if (c == '\r') {
                std::cout << "\\r";
            } else if (c == '\n') {
                std::cout << "\\n";
            } else if (c >= 32 && c < 127) {
                std::cout << c;
            } else {
                std::cout << "\\x" << std::hex << (int)(unsigned char)c << std::dec;
            }
        }
        std::cout << std::endl;

        // Small delay between commands
        usleep(100000); // 100ms
    }

    close(sock);
    std::cout << "\nConnection closed." << std::endl;
    return 0;
}