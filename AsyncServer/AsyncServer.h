#pragma once

#include <CommonIncludes.h>

enum OP_TYPE {
    OP_ACCEPT,
    OP_RECV,
    OP_SEND
};

// Per-socket I/O context
struct PER_SOCKET_CONTEXT
{
    SOCKET socket;
    OVERLAPPED overlapped;
    WSABUF wsabuf;
    char buffer[1024];
    DWORD bytesTransferred;
    OP_TYPE operation;
};

// Forward declaration of AsyncIOCPServer
class AsyncIOCPServer
{
public:
    AsyncIOCPServer(int port, int workerThreadCount);
    ~AsyncIOCPServer();

    // Initializes and starts the server (creates listen socket, IOCP, worker threads, etc.)
    bool Start();

    // Stops the server and cleans up (closes sockets, ends threads, etc.)
    void Stop();

private:
    // Internal helpers
    bool initWinsock();
    bool createListenSocket();
    bool associateDeviceToIOCP(HANDLE device, ULONG_PTR completionKey = 0);
    bool postInitialAccepts(int count);
    bool createAcceptSocket(SOCKET& acceptSocket);
    bool postAccept(SOCKET listenSock, SOCKET acceptSock, PER_SOCKET_CONTEXT* ctx);

    // Worker thread function (static so it can match the LPTHREAD_START_ROUTINE signature)
    static DWORD WINAPI workerThread(LPVOID lpParam);

    // This function is called from workerThread to handle the IO completions
    void handleIO(DWORD bytesTransferred, PER_SOCKET_CONTEXT* ctx);

private:
    int m_port;
    int m_workerThreadCount;

    SOCKET m_listenSocket;
    HANDLE m_hIOCP;
    std::vector<HANDLE> m_workerThreads;
    bool m_running;
};

