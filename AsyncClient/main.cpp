#include "AsyncIOCPClient.h"

int main()
{
    // Create an async client that connects to 127.0.0.1:27015
    AsyncIOCPClient client("127.0.0.1", 27015);

    // Start the client (initialize Winsock, IOCP, worker thread)
    if (!client.Start()) {
        std::cerr << "[MAIN] Client failed to start.\n";
        return 1;
    }

    // Initiate async connection
    if (!client.Connect()) {
        std::cerr << "[MAIN] Connect call failed.\n";
        client.Stop();
        return 1;
    }

    std::cout << "[MAIN] Press Enter after the client is connected...\n";
    std::cin.get();

    // Send some data to the server
    if (!client.Send("Hello from Async Client!")) {
        std::cerr << "[MAIN] Send failed.\n";
    }

    std::cout << "[MAIN] Press Enter to stop client...\n";
    std::cin.get();

    // Stop the client
    client.Stop();

    std::cin.get();
    return 0;
}
