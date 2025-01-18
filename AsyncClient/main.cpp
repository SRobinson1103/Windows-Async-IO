#include "AsyncIOCPClient.h"

int main()
{
    // Create an async client that connects to 127.0.0.1:27015
    AsyncIOCPClient client("127.0.0.1", 27015);

    // Start the client (initialize Winsock, IOCP, worker thread)
    if (!client.Start())
    {
        std::cerr << "[MAIN] Client failed to start.\n";
        return 1;
    }

    // Initiate async connection
    if (!client.Connect())
    {
        std::cerr << "[MAIN] Connect call failed.\n";
        client.Stop();
        return 1;
    }

    std::string msg;
    while (true)
    {
        std::cout << "Please enter your message. type \"EXIT\" to quit." << std::endl;
        std::cin >> msg;
        if (msg.compare("EXIT") == 0)
            break;
        if (!client.Send(msg))
        {
            std::cerr << "[MAIN] Send failed.\n";
            break;
        }
    }


    std::cout << "[MAIN] Exiting...\n";

    // Stop the client
    client.Stop();

    std::cin.get();
    return 0;
}
