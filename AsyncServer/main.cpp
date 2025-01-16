#include "AsyncServer.h"

int main()
{
    // Create a server listening on port 27015 with 2 worker threads
    AsyncIOCPServer server(27015, 2);

    // Start the server
    if (!server.Start()) {
        std::cerr << "[MAIN] Failed to start server.\n";
        return 1;
    }

    std::cout << "[MAIN] Server running. Press Enter to stop...\n";
    std::cin.get();
    return 0;
}
