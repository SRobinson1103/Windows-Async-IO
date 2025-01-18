#pragma once

#include <CommonIncludes.h>

#include <deque>
#include <functional>
#include <unordered_map>

using ClientID = uint64_t;

struct ClientContext
{
    SOCKET socket = INVALID_SOCKET;

    // Overlapped I/O structures
    OVERLAPPED acceptOverlapped;
    OVERLAPPED recvOverlapped;
    OVERLAPPED sendOverlapped;

    WSABUF recvBuf;
    char   recvBuffer[4096];
    WSABUF sendBuf;
    char   sendBuffer[4096];
    WSABUF acceptBuf;
    char   acceptBuffer[4096];

    // Identify which operation is happening:
    enum class OperationType { Accept, Recv, Send, Idle } operation;

    // For partial sends
    size_t bytesToSend = 0;  // how many bytes left to send in current chunk
    size_t bytesSentSoFar = 0;  // how many bytes have been sent in the current chunk

    // Queue of pending messages
    // Each std::string is a single message we want to send to this client
    std::deque<std::string> sendQueue;

    // The server assigns a unique ID when the client is fully accepted
    ClientID clientId;
};

enum OP_TYPE
{
    OP_ACCEPT,
    OP_RECV,
    OP_SEND
};

// Per-socket I/O context
struct PER_SOCKET_CONTEXT
{
    OVERLAPPED overlapped;
    SOCKET socket;
    WSABUF wsabuf;
    char buffer[1024];
    DWORD bytesTransferred;
    OP_TYPE operation;
};

// Callback type definitions
using OnClientConnect = std::function<void(ClientID)>;
using OnClientDisconnect = std::function<void(ClientID)>;
using OnDataReceived = std::function<void(ClientID, const std::string&)>;

// Forward declaration of AsyncIOCPServer
class AsyncIOCPServer
{
public:
    AsyncIOCPServer();
    ~AsyncIOCPServer();

    // Initializes and starts the server (creates listen socket, IOCP, worker threads, etc.)
    bool Start(int port, int maxClients, int workerThreadCount = 2);

    // Stops the server and cleans up (closes sockets, ends threads, etc.)
    void Stop();

    bool SendToClient(ClientID clientId, const std::string& data);
    void Broadcast(const std::string& data);

    // Set callbacks
    void SetOnClientConnect(OnClientConnect cb);
    void SetOnClientDisconnect(OnClientDisconnect cb);
    void SetOnDataReceived(OnDataReceived cb);

private:
    // Internal helpers
    bool initWinsock();
    bool createListenSocket();
    bool createWorkerThreads();
    bool associateDeviceToIOCP(HANDLE device, ULONG_PTR completionKey = 0);
    bool associateSocketWithIOCP(SOCKET s);

    bool postInitialAccepts(int count);
    bool postAccept();

    void postNextSend(ClientContext* ctx);

    void onClientDisconnect(ClientContext* ctx);

    // Worker thread function (static so it can match the LPTHREAD_START_ROUTINE signature)
    static DWORD WINAPI workerThread(LPVOID lpParam);

    // This function is called from workerThread to handle the IO completions
    void handleIO(DWORD bytesTransferred, ClientContext* ctx);

    // Internal method to accept a new client
    void onAcceptComplete(ClientContext* ctx, DWORD bytesTransferred);
    // Internal method to receive data from a client
    void onRecvComplete(ClientContext* ctx, DWORD bytesTransferred);
    // Internal method to send data to a client
    void onSendComplete(ClientContext* ctx, DWORD bytesTransferred);

    // Utility to get next unique client ID
    ClientID getNextClientId();

private:
    int m_port;

    int m_workerThreadCount;
    int m_maxClients;
    int m_currentClientCount;

    ClientID m_nextClientId;

    SOCKET m_listenSocket;
    HANDLE m_hIOCP;
    LPFN_ACCEPTEX m_lpfnAcceptEx;
    std::vector<HANDLE> m_workerThreads;
    bool m_running;

    // We store pending accept contexts using Overlapped pointer as the key
    std::unordered_map<OVERLAPPED*, ClientContext*> m_overlappedMap;
    std::mutex m_overlappedMapMutex;

    // A map of clientID -> client context
    std::unordered_map<ClientID, std::unique_ptr<ClientContext>> m_clients;
    std::mutex m_clientsMutex;

    ConsoleLogger& m_logger;

    // Callback functions
    OnClientConnect    m_onClientConnect = nullptr;
    OnClientDisconnect m_onClientDisconnect = nullptr;
    OnDataReceived     m_onDataReceived = nullptr;
};
