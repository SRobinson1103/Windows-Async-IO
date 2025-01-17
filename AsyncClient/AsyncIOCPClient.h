#pragma once

#include <CommonIncludes.h>

// A small structure that holds I/O data
struct CLIENT_IO_CONTEXT
{
    OVERLAPPED overlapped;
    WSABUF wsabuf;
    char buffer[1024];
    DWORD bytesTransferred;
};

class AsyncIOCPClient
{
public:
    AsyncIOCPClient(const std::string& host, int port);
    ~AsyncIOCPClient();

    // Initialize Winsock, create socket, create IOCP, start worker thread
    bool Start();

    // Connect to the remote server (asynchronously with ConnectEx or synchronously)
    bool Connect();

    // Send data (asynchronously)
    bool Send(const std::string& data);

    // Stop everything: close socket, stop IOCP and worker thread, cleanup
    void Stop();

private:
    // Internal helpers
    bool initWinsock();
    bool createSocket();
    bool createIOCP();
    bool associateSocketToIOCP(SOCKET s, ULONG_PTR completionKey = 0);
    bool bindAnyAddress(SOCKET s);
    bool getConnectExPtr(SOCKET s, LPFN_CONNECTEX& outConnectEx);
    bool postRecv(CLIENT_IO_CONTEXT* ctx);

    // Worker thread function
    static DWORD WINAPI workerThread(LPVOID lpParam);

    // Handle completed I/O
    void handleIO(DWORD bytesTransferred, CLIENT_IO_CONTEXT* ioCtx);

private:
    // Connection parameters
    std::string m_host;
    int m_port;

    // Winsock entities
    SOCKET m_socket;
    HANDLE m_hIOCP;
    HANDLE m_hWorkerThread;
    bool   m_running;

    // For the connect operation
    sockaddr_in m_serverAddr;

    // IO context for sending/receiving
    CLIENT_IO_CONTEXT* m_ioContext;

    ConsoleLogger& m_logger;
};
