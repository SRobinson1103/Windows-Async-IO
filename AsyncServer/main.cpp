#include "AsyncServer.h"

int maxClients = 100;
int workerThreads = 4;

int main()
{
    AsyncIOCPServer server;

    // Set callbacks
    server.SetOnClientConnect([](ClientID cid)
    {
    std::cout << "Client " << cid << " connected.\n";
    });

    server.SetOnDataReceived([&server](ClientID cid, const std::string& msg)
    {
    std::cout << "Client " << cid << " says: " << msg << "\n";
    // Echo back
    //server.SendToClient(cid, "Got your message!");
    });

    server.SetOnClientDisconnect([](ClientID cid)
    {
    std::cout << "Client " << cid << " disconnected.\n";
    });

    if (!server.Start(27015, maxClients, workerThreads))
    {
        std::cerr << "Failed to start server.\n";
        return 1;
    }

    std::cout << "Server started. Press ENTER to exit.\n";
    std::cin.get();

    server.Stop();
    return 0;
}
