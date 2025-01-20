#include "AsyncServer.h"

AsyncIOCPServer::AsyncIOCPServer()
    : m_port(0),
    m_maxClients(0),
    m_currentClientCount(0),
    m_workerThreadCount(0),
    m_nextClientId(1),
    m_listenSocket(INVALID_SOCKET),
    m_lpfnAcceptEx(NULL),
    m_hIOCP(NULL),
    m_running(false),
    m_logger(ConsoleLogger::getInstance())
{
}

AsyncIOCPServer::~AsyncIOCPServer()
{
    // Ensure cleanup if not already done
    Stop();
}

#pragma region publicFunctions
bool AsyncIOCPServer::Start(int port, int maxClients, int workerThreadCount)
{
    m_port = port;
    m_maxClients = maxClients;
    m_workerThreadCount = workerThreadCount;

    if (!initWinsock())
    {
        m_logger.log(ConsoleLogger::LogLevel::ERR, "[SERVER] Winsock initialization failed.\n");
        return false;
    }

    if (!createListenSocket())
    {
        m_logger.log(ConsoleLogger::LogLevel::ERR, "[SERVER] createListenSocket failed.\n");
        return false;
    }

    // Create IO Completion Port
    m_hIOCP = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, m_workerThreadCount);
    if (!m_hIOCP)
    {
        m_logger.log(ConsoleLogger::LogLevel::ERR, "[SERVER] CreateIoCompletionPort failed: " + std::to_string(GetLastError()) + "\n");
        return false;
    }

    // Associate the listen socket with IOCP
    if (!associateDeviceToIOCP(reinterpret_cast<HANDLE>(m_listenSocket)))
    {
        m_logger.log(ConsoleLogger::LogLevel::ERR, "[SERVER] Failed to associate listen socket to IOCP.\n");
        return false;
    }

    if (!createWorkerThreads())
    {
        m_logger.log(ConsoleLogger::LogLevel::ERR, "[SERVER] Failed to create worker threads.\n");
        return false;
    }

    // Post initial AcceptEx calls
    if (!postInitialAccepts(m_workerThreadCount))
    {
        m_logger.log(ConsoleLogger::LogLevel::ERR, "[SERVER] postInitialAccepts failed.\n");
        return false;
    }

    m_logger.log(ConsoleLogger::LogLevel::INFO, "[SERVER] Listening on port " + std::to_string(m_port) + "...\n");
    m_running = true;

    return true;
}

void AsyncIOCPServer::Stop()
{
    if (!m_running) return;

    //close the IOCP handle to break GetQueuedCompletionStatus.

        // Close IOCP handle
    if (m_hIOCP != INVALID_HANDLE_VALUE)
    {
        CloseHandle(m_hIOCP);
        m_hIOCP = INVALID_HANDLE_VALUE;
    }

    // Wait for worker threads to exit
    for (HANDLE thread : m_workerThreads)
    {
        WaitForSingleObject(thread, INFINITE);
        CloseHandle(thread);
    }
    m_workerThreads.clear();

    // Close the listening socket
    if (m_listenSocket != INVALID_SOCKET)
    {
        closesocket(m_listenSocket);
        m_listenSocket = INVALID_SOCKET;
    }

    // Close all client sockets
    {
        std::lock_guard<std::mutex> lock(m_overlappedMapMutex);
        m_overlappedMap.clear();
    }
    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        for (auto& kv : m_clients)
        {
            ClientContext* ctx = kv.second.get();
            closesocket(ctx->socket);
        }
        m_clients.clear();
    }

    // Cleanup Winsock
    WSACleanup();

    m_running = false;
    m_currentClientCount = 0;
    m_logger.log(ConsoleLogger::LogLevel::INFO, "[SERVER] Stopped.\n");
}

bool AsyncIOCPServer::SendToClient(ClientID cid, const std::string& data)
{
    ClientContext* ctx;
    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        auto it = m_clients.find(cid);
        if (it == m_clients.end())
        {
            m_logger.log(ConsoleLogger::LogLevel::ERR, "[SERVER] Failed to send. Client ID [" + std::to_string(cid) + "] does not exist.");
            return false;
        }

        ctx = it->second.get();
    }
    // Enqueue the data
    ctx->sendQueue.push_back(data);

    // If we are not currently in the middle of a send, post the first send
    if (ctx->operation != ClientContext::OperationType::Send)
    {
        // Post first overlapped send
        postNextSend(ctx);
    }

    return true;
}

// This helper picks the front message in sendQueue and posts an overlapped send
void AsyncIOCPServer::postNextSend(ClientContext* ctx)
{
    if (ctx->sendQueue.empty())
    {
        m_logger.log(ConsoleLogger::LogLevel::INFO, "[SERVER] Failed to post next send. No data to send.");
        return;
    }

    // Take the front message
    const std::string& frontMsg = ctx->sendQueue.front();

    // Initialize partial send tracking
    ctx->bytesToSend = frontMsg.size();
    ctx->bytesSentSoFar = 0;

    // We'll copy up to sendBuffer capacity:
    size_t chunk = std::min(frontMsg.size(), sizeof(ctx->sendBuffer));

    // Copy data into the sendBuffer
    memcpy(ctx->sendBuffer, frontMsg.data(), chunk);

    ctx->sendBuf.buf = ctx->sendBuffer;
    ctx->sendBuf.len = (ULONG)chunk;

    ctx->operation = ClientContext::OperationType::Send;

    ZeroMemory(&ctx->sendOverlapped, sizeof(ctx->sendOverlapped));

    DWORD flags = 0;
    DWORD bytesSent = 0;
    int ret = WSASend(ctx->socket,
        &ctx->sendBuf,
        1,
        &bytesSent,
        flags,
        &ctx->sendOverlapped,
        NULL);
    if (ret == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING)
    {
        m_logger.log(ConsoleLogger::LogLevel::ERR, "[SERVER] Failed to Post Next Send to client " + std::to_string(ctx->clientId) + ".");
        onClientDisconnect(ctx);
    }

    OVERLAPPED* sendKey = &ctx->sendOverlapped;
    {
        std::lock_guard<std::mutex> lock(m_overlappedMapMutex);
        m_overlappedMap[sendKey] = ctx;
    }
}

void AsyncIOCPServer::Broadcast(const std::string& data)
{
    // For each connected client, call SendToClient
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    for (auto& kv : m_clients)
    {
        SendToClient(kv.first, data);
    }
}

void AsyncIOCPServer::SetOnClientConnect(OnClientConnect cb)
{
    m_onClientConnect = cb;
}

void AsyncIOCPServer::SetOnClientDisconnect(OnClientDisconnect cb)
{
    m_onClientDisconnect = cb;
}

void AsyncIOCPServer::SetOnDataReceived(OnDataReceived cb)
{
    m_onDataReceived = cb;
}
#pragma endregion

#pragma region privateFunctions
bool AsyncIOCPServer::initWinsock()
{
    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0)
    {
        m_logger.log(ConsoleLogger::LogLevel::ERR, "[SERVER] WSAStartup failed with error: " + std::to_string(result) + "\n");
        return false;
    }
    return true;
}

bool AsyncIOCPServer::createListenSocket()
{
    m_listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_listenSocket == INVALID_SOCKET) {
        m_logger.log(ConsoleLogger::LogLevel::ERR, "[SERVER] socket() failed: " + std::to_string(WSAGetLastError()) + "\n");
        return false;
    }

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    serverAddr.sin_port = htons(static_cast<u_short>(m_port));

    if (bind(m_listenSocket, reinterpret_cast<SOCKADDR*>(&serverAddr), sizeof(serverAddr)) == SOCKET_ERROR)
    {
        m_logger.log(ConsoleLogger::LogLevel::ERR, "[SERVER] bind() failed: " + std::to_string(WSAGetLastError()) + "\n");
        closesocket(m_listenSocket);
        m_listenSocket = INVALID_SOCKET;
        return false;
    }

    if (listen(m_listenSocket, SOMAXCONN) == SOCKET_ERROR)
    {
        m_logger.log(ConsoleLogger::LogLevel::ERR, "[SERVER] listen() failed: " + std::to_string(WSAGetLastError()) + "\n");
        closesocket(m_listenSocket);
        m_listenSocket = INVALID_SOCKET;
        return false;
    }

    return true;
}

bool AsyncIOCPServer::createWorkerThreads()
{
    // Create worker threads
    for (int i = 0; i < m_workerThreadCount; ++i)
    {
        HANDLE hThread = CreateThread(nullptr, 0, workerThread, this, 0, nullptr);
        if (!hThread)
        {
            m_logger.log(ConsoleLogger::LogLevel::ERR, "[SERVER] CreateThread failed: " + std::to_string(GetLastError()) + "\n");
            return false;
        }
        m_workerThreads.push_back(hThread);
    }
    return true;
}

bool AsyncIOCPServer::associateDeviceToIOCP(HANDLE device, ULONG_PTR completionKey)
{
    HANDLE h = CreateIoCompletionPort(device, m_hIOCP, completionKey, m_workerThreadCount);
    if (!h)
    {
        m_logger.log(ConsoleLogger::LogLevel::ERR, "[SERVER] CreateIoCompletionPort (associate device) failed: " + std::to_string(GetLastError()) + "\n");
        return false;
    }
    return true;
}

bool AsyncIOCPServer::associateSocketWithIOCP(SOCKET s)
{
    /* completionKey, if desired pass the ClientContext* or client ID */
    HANDLE h = CreateIoCompletionPort((HANDLE)s, m_hIOCP, 0, 0);
    if (!h)
    {
        m_logger.log(ConsoleLogger::LogLevel::ERR, "[SERVER] CreateIoCompletionPort (associate socket) failed: " + std::to_string(GetLastError()) + "\n");
        return false;
    }
    return true;
}

bool AsyncIOCPServer::postInitialAccepts(int count)
{
    for (int i = 0; i < count; ++i)
    {
        if (!postAccept())
        {
            return false;
        }
    }
    return true;
}

bool AsyncIOCPServer::postAccept()
{
    // Create a new ClientContext for this accept operation.
    //    This context is temporary until AcceptEx completes, at which point
    //    we finalize the client in onAcceptComplete(...).
    auto ctx = std::make_unique<ClientContext>();

    ctx->operation = ClientContext::OperationType::Accept;
    ZeroMemory(&ctx->acceptOverlapped, sizeof(ctx->acceptOverlapped));

    // Create a new accept socket for this incoming connection
    SOCKET acceptSock = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
    if (acceptSock == INVALID_SOCKET)
    {
        m_logger.log(ConsoleLogger::LogLevel::ERR, "[SERVER] WSASocket for accept failed: " + std::to_string(WSAGetLastError()) + "\n");
        return false;
    }

    ctx->socket = acceptSock;

    // Prepare a buffer for AcceptEx
    ZeroMemory(ctx->acceptBuffer, sizeof(ctx->acceptBuffer));

    // Retrieve the AcceptEx function pointer if not already cached
    if (!m_lpfnAcceptEx)
    {
        GUID guidAcceptEx = WSAID_ACCEPTEX;
        DWORD bytes = 0;
        int rv = WSAIoctl(
            m_listenSocket,
            SIO_GET_EXTENSION_FUNCTION_POINTER,
            &guidAcceptEx,
            sizeof(guidAcceptEx),
            &m_lpfnAcceptEx,
            sizeof(m_lpfnAcceptEx),
            &bytes,
            nullptr,
            nullptr
        );
        if (rv == SOCKET_ERROR)
        {
            m_logger.log(ConsoleLogger::LogLevel::ERR, "[SERVER] WSAIoctl for AcceptEx failed: " + std::to_string(WSAGetLastError()) + "\n");
            closesocket(acceptSock);
            return false;
        }
    }

    // Call AcceptEx in overlapped mode
    DWORD bytesReceived = 0;
    BOOL bRet = m_lpfnAcceptEx(
        m_listenSocket,
        acceptSock,
        ctx->acceptBuffer,
        0,
        sizeof(sockaddr_in) + 16,
        sizeof(sockaddr_in) + 16,
        &bytesReceived,
        &ctx->acceptOverlapped
    );

    if (!bRet && WSAGetLastError() != ERROR_IO_PENDING)
    {
        m_logger.log(ConsoleLogger::LogLevel::ERR, "[SERVER] AcceptEx failed: " + std::to_string(WSAGetLastError()) + "\n");
        closesocket(acceptSock);
        return false;
    }

    // Store this context in m_overlappedMap keyed by its OVERLAPPED* 
    OVERLAPPED* key = &ctx->acceptOverlapped;
    {
        std::lock_guard<std::mutex> lock(m_overlappedMapMutex);
        m_overlappedMap[key] = ctx.release();
    }

    return true;
}
#pragma endregion

DWORD WINAPI AsyncIOCPServer::workerThread(LPVOID lpParam)
{
    AsyncIOCPServer* server = reinterpret_cast<AsyncIOCPServer*>(lpParam);
    DWORD bytesTransferred = 0;
    ULONG_PTR completionKey = 0;
    OVERLAPPED* pOverlapped = nullptr;

    while (true)
    {
        BOOL success = GetQueuedCompletionStatus(
            server->m_hIOCP,
            &bytesTransferred,
            &completionKey,
            &pOverlapped,
            INFINITE
        );

        // If pOverlapped == nullptr, the IOCP is probably closed (server stopping),
        // or a serious error occurred. Let's break out.
        if (!success && pOverlapped == nullptr)
        {
            ConsoleLogger::getInstance().log(ConsoleLogger::LogLevel::ERR, "[WORKER] GQCS failed, or server is shutting down. LastError: "
                + std::to_string(GetLastError()) + "\n");
            break;
        }

        if (!pOverlapped)
        {
            // If no overlapped structure is returned, we can't do further processing
            continue;
        }

        ClientContext* ctx = nullptr;
        {
            std::lock_guard<std::mutex> lock(server->m_overlappedMapMutex);
            //  Find the context in m_pendingAccepts
            auto it = server->m_overlappedMap.find(pOverlapped);
            if (it != server->m_overlappedMap.end())
            {
                ctx = it->second;
                //remove old mapping
                server->m_overlappedMap.erase(it);
            }
            else
            {
                ConsoleLogger::getInstance().log(ConsoleLogger::LogLevel::ERR, "Could not find ClientContext pointer in map.");
            }
        }

        // pass to handleIO
        if (ctx)
            server->handleIO(bytesTransferred, ctx);
    }

    return 0;
}

void AsyncIOCPServer::onClientDisconnect(ClientContext* ctx)
{
    closesocket(ctx->socket);
    {
        std::lock_guard<std::mutex> lock(m_overlappedMapMutex);
        auto acceptOverlap = m_overlappedMap.find(&ctx->acceptOverlapped);
        if (acceptOverlap != m_overlappedMap.end())
            m_overlappedMap.erase(acceptOverlap);

        auto recvOverlap = m_overlappedMap.find(&ctx->recvOverlapped);
        if (recvOverlap != m_overlappedMap.end())
            m_overlappedMap.erase(recvOverlap);

        auto sendOverlap = m_overlappedMap.find(&ctx->sendOverlapped);
        if (sendOverlap != m_overlappedMap.end())
            m_overlappedMap.erase(sendOverlap);
    }

    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        auto cid = m_clients.find(ctx->clientId);
        if (cid != m_clients.end())
            m_clients.erase(cid);
    }
}

void AsyncIOCPServer::handleIO(DWORD bytesTransferred, ClientContext* ctx)
{
    switch (ctx->operation)
    {
    case ClientContext::OperationType::Accept:
        onAcceptComplete(ctx, bytesTransferred);
        break;
    case ClientContext::OperationType::Recv:
        onRecvComplete(ctx, bytesTransferred);
        break;
    case ClientContext::OperationType::Send:
        onSendComplete(ctx, bytesTransferred);
        break;
    }
}

void AsyncIOCPServer::onAcceptComplete(ClientContext* ctx, DWORD bytesTransferred)
{
    // If bytesTransferred == 0 or more => the client connected
    // setsockopt(SO_UPDATE_ACCEPT_CONTEXT)
    int result = setsockopt(ctx->socket,
        SOL_SOCKET,
        SO_UPDATE_ACCEPT_CONTEXT,
        (char*)&m_listenSocket,
        sizeof(m_listenSocket));
    if (result == SOCKET_ERROR)
    {
        m_logger.log(ConsoleLogger::LogLevel::ERR, "[SERVER] setsockopt SO_UPDATE_ACCEPT_CONTEXT failed: " + std::to_string(WSAGetLastError()) + "\n");
        onClientDisconnect(ctx);
        return;
    }

    // Associate the accept socket with IOCP so that completion 
    // notifications arrive on the same IOCP.
    if (!associateSocketWithIOCP(ctx->socket))
    {
        m_logger.log(ConsoleLogger::LogLevel::ERR, "[SERVER] associateDeviceToIOCP failed for acceptSock.\n");
        onClientDisconnect(ctx);
        return;
    }

    // Generate a new clientId
    ClientID cid = getNextClientId();
    ctx->clientId = cid;

    // Insert into m_clients
    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        m_clients[cid] = std::unique_ptr<ClientContext>(ctx);
    }
    // If we have an OnClientConnect callback, call it
    if (m_onClientConnect)
    {
        m_onClientConnect(cid);
    }

    // Post first overlapped recv
    ZeroMemory(&ctx->recvOverlapped, sizeof(ctx->recvOverlapped));
    ctx->recvBuf.buf = ctx->recvBuffer;
    ctx->recvBuf.len = sizeof(ctx->recvBuffer);
    ctx->operation = ClientContext::OperationType::Recv;

    DWORD flags = 0;
    DWORD bytesRecv = 0;
    int ret = WSARecv(ctx->socket, &ctx->recvBuf, 1, &bytesRecv, &flags, &ctx->recvOverlapped, nullptr);
    if (ret == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING)
    {
        m_logger.log(ConsoleLogger::LogLevel::ERR, "[SERVER] WSARecv failed after AcceptEx: " + std::to_string(WSAGetLastError()) + "\n");
        onClientDisconnect(ctx);
        return;
    }

    OVERLAPPED* recvKey = &ctx->recvOverlapped;
    {
        std::lock_guard<std::mutex> lock(m_overlappedMapMutex);
        m_overlappedMap[recvKey] = ctx;
    }
    // Post another accept for the next incoming connection
    postAccept();
}

void AsyncIOCPServer::onRecvComplete(ClientContext* ctx, DWORD bytesTransferred)
{
    if (bytesTransferred == 0)
    {
        // client disconnected
        if (m_onClientDisconnect)
        {
            m_onClientDisconnect(ctx->clientId);
        }
        m_logger.log(ConsoleLogger::LogLevel::INFO, "[SERVER] Received 0 bytes. Client disconnected.");
        onClientDisconnect(ctx);
        return;
    }

    // We got data
    std::string msg(ctx->recvBuffer, bytesTransferred);

    // Fire the OnDataReceived callback
    if (m_onDataReceived)
    {
        m_onDataReceived(ctx->clientId, msg);
    }

    // Post another recv to keep reading
    ZeroMemory(&ctx->recvOverlapped, sizeof(ctx->recvOverlapped));
    ctx->recvBuf.buf = ctx->recvBuffer;
    ctx->recvBuf.len = sizeof(ctx->recvBuffer);
    ctx->operation = ClientContext::OperationType::Recv;

    DWORD flags = 0;
    DWORD bytesRead = 0;
    int ret = WSARecv(ctx->socket, &ctx->recvBuf, 1, &bytesRead, &flags, &ctx->recvOverlapped, nullptr);
    if (ret == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING)
    {
        m_logger.log(ConsoleLogger::LogLevel::INFO, "[SERVER] WSARecv failed: " + std::to_string(WSAGetLastError()) + ".");
        onClientDisconnect(ctx);
        return;
    }

    OVERLAPPED* recvKey = &ctx->recvOverlapped;
    {
        std::lock_guard<std::mutex> lock(m_overlappedMapMutex);
        m_overlappedMap[recvKey] = ctx;
    }
}

void AsyncIOCPServer::onSendComplete(ClientContext* ctx, DWORD bytesTransferred)
{
    if (bytesTransferred == 0)
    {
        // The client likely disconnected or something is off
        m_logger.log(ConsoleLogger::LogLevel::INFO, "[SERVER] 0 bytes on send. Closing client socket.");
        onClientDisconnect(ctx);
        return;
    }

    // Update partial send state
    ctx->bytesSentSoFar += bytesTransferred;

    // Check if we've finished sending the front message
    size_t frontMsgSize = ctx->sendQueue.front().size();

    if (ctx->bytesSentSoFar < frontMsgSize)
    {
        // We still have more of the same message to send
        // Calculate how many bytes left
        size_t remaining = frontMsgSize - ctx->bytesSentSoFar;
        size_t chunk = std::min(remaining, sizeof(ctx->sendBuffer));

        // Copy next chunk into sendBuffer
        memcpy(ctx->sendBuffer, ctx->sendQueue.front().data() + ctx->bytesSentSoFar, chunk);

        ctx->sendBuf.buf = ctx->sendBuffer;
        ctx->sendBuf.len = (ULONG)chunk;

        ZeroMemory(&ctx->sendOverlapped, sizeof(ctx->sendOverlapped));
        DWORD flags = 0;
        DWORD sent = 0;
        int ret = WSASend(ctx->socket, &ctx->sendBuf, 1, &sent, flags, &ctx->sendOverlapped, NULL);
        if (ret == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING)
        {
            m_logger.log(ConsoleLogger::LogLevel::INFO, "[SERVER] Failed to post send. Closing client socket: " + std::to_string(WSAGetLastError()) + ".\n");
            onClientDisconnect(ctx);
            return;
        }

        OVERLAPPED* sendKey = &ctx->recvOverlapped;
        {
            std::lock_guard<std::mutex> lock(m_overlappedMapMutex);
            m_overlappedMap[sendKey] = ctx;
        }
    }
    else
    {
        // We are done sending the front message
        ctx->sendQueue.pop_front();

        // If there's another message, send it
        if (!ctx->sendQueue.empty())
        {
            postNextSend(ctx);
        }
        else
        {
            // No more data to send, switch operation away from SEND
            ctx->operation = ClientContext::OperationType::Recv;
        }
    }
}

ClientID AsyncIOCPServer::getNextClientId()
{
    return m_nextClientId++;
}
