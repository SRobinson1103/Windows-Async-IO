#pragma once

#include <CommonIncludes.h>
#include <functional>
#include <queue>

enum OP_TYPE
{
    OP_CONNECT,
    OP_RECV,
    OP_SEND
};

// A small structure that holds I/O data
struct CLIENT_IO_CONTEXT
{
    OVERLAPPED overlapped;
    WSABUF wsabuf;
    char buffer[1024];
    DWORD bytesTransferred;
    OP_TYPE operation;
};

struct ClientContext
{
    // Overlapped structures
    OVERLAPPED connectOverlapped;
    OVERLAPPED recvOverlapped;
    OVERLAPPED sendOverlapped;

    // Buffers for recv/send
    char recvBuffer[4096];
    char sendBuffer[4096];

    WSABUF recvWSABuf;
    WSABUF sendWSABuf;

    // For partial send
    size_t bytesToSend = 0;
    size_t bytesSentSoFar = 0;

    // A queue of messages for partial sending
    std::deque<std::string> sendQueue;

    // Operation type enum if you want to track
    enum class Operation { None, Connect, Recv, Send };
    Operation operation = Operation::None;

    // Possibly store host/port or other info
    bool connected = false;
};

// Callback type definitions
using OnConnectCallback = std::function<void()>;
using OnDisconnectCallback = std::function<void()>;
using OnDataReceivedCB = std::function<void(const std::string&)>;

class AsyncIOCPClient
{
public:
    AsyncIOCPClient();
    ~AsyncIOCPClient();

    // Initialize Winsock, create socket, create IOCP, start worker thread
    bool Start(int workerThreadCount = 1);

    // Connect to the remote server asynchronously with ConnectEx
    bool Connect(const std::string& host, unsigned short port);

    // Send data, queues data for partial send
    bool Send(const std::string& data);

    // Stop everything: close socket, stop IOCP and worker thread, cleanup
    void Stop();

private:
    // Internal helpers
    bool initWinsock();
    bool createSocket();
    bool createIOCP(int workerThreadCount);
    bool associateSocketWithIOCP(SOCKET s, ULONG_PTR completionKey = 0);

    bool bindAnyAddress(SOCKET s);
    bool getConnectExPtr(SOCKET s, LPFN_CONNECTEX& outConnectEx);
    bool postRecv(CLIENT_IO_CONTEXT* ctx);

    // Worker thread function
    static DWORD WINAPI workerThread(LPVOID lpParam);

    // Handle completed I/O
    void handleIO(DWORD bytesTransferred, OVERLAPPED* pOverlapped);

    // Internal methods for connect, recv, send completions
    void onConnectComplete(ClientContext* ctx, DWORD bytesTransferred);
    void onRecvComplete(ClientContext* ctx, DWORD bytesTransferred);
    void onSendComplete(ClientContext* ctx, DWORD bytesTransferred);

    // Post overlapped receive
    bool postRecv();

    // Actually post the next overlapped send from the queue
    void postNextSend();

    // Some thread-safe disconnection routine
    void disconnect();

private:
    // Connection parameters
    std::string m_host;
    unsigned short m_port;

    // Single client context (if you only connect once)
    // If you plan to handle multiple servers, you'd store multiple contexts in a map
    std::unique_ptr<ClientContext> m_ctx;

    // Winsock entities
    SOCKET m_socket;
    HANDLE m_hIOCP;
    bool   m_running;

    // Worker threads
    std::vector<HANDLE> m_workerThreads;

    // Callback functions
    OnConnectCallback    m_onConnect;
    OnDisconnectCallback m_onDisconnect;
    OnDataReceivedCB     m_onDataReceived;

    // For thread-safety around queue, context, etc.
    std::mutex m_ctxMutex;

    // For the connect operation
    sockaddr_in m_serverAddr;

    // Pointer to ConnectEx if needed
    LPFN_CONNECTEX m_lpfnConnectEx = nullptr;

    ConsoleLogger& m_logger;
};
