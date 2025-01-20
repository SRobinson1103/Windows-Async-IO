#include "AsyncIOCPClient.h"

AsyncIOCPClient::AsyncIOCPClient()
    : m_host(""),
    m_port(0),
    m_socket(INVALID_SOCKET),
    m_hIOCP(NULL),
    m_running(false),
    m_logger(ConsoleLogger::getInstance())
{
    ZeroMemory(&m_serverAddr, sizeof(m_serverAddr));
    m_serverAddr.sin_family = AF_INET;
    m_serverAddr.sin_port = htons(static_cast<u_short>(m_port));
}

AsyncIOCPClient::~AsyncIOCPClient()
{
    // Ensure cleanup if user forgets
    Stop();
}

bool AsyncIOCPClient::Start(int workerThreadCount)
{
    if (!initWinsock())
    {
        m_logger.log(ConsoleLogger::LogLevel::ERR, "[CLIENT] Winsock initialization failed.\n");
        return false;
    }
    if (!createSocket())
    {
        m_logger.log(ConsoleLogger::LogLevel::ERR, "[CLIENT] Socket creation failed.\n");
        return false;
    }
    if (!createIOCP(workerThreadCount))
    {
        m_logger.log(ConsoleLogger::LogLevel::ERR, "[CLIENT] IOCP creation failed.\n");
        return false;
    }

    // Create a worker thread to handle all I/O completions
    m_running = true;

    return true;
}

bool AsyncIOCPClient::Connect(const std::string& host, unsigned short port)
{
    std::lock_guard<std::mutex> lock(m_ctxMutex);
    m_host = host;
    m_port = port;

    m_serverAddr.sin_family = AF_INET;
    m_serverAddr.sin_port = htons(port);

    // Convert m_host (std::string) to std::wstring
    std::wstring wHost(host.begin(), host.end());
    // Convert the string host to a numeric address.
    if (InetPton(AF_INET, wHost.c_str(), &m_serverAddr.sin_addr) <= 0)
    {
        m_logger.log(ConsoleLogger::LogLevel::ERR, "[CLIENT] Invalid host or InetPton failed.\n");
        return false;
    }
    
    // Bind local address
    if (!bindAnyAddress(m_socket))
    {
        m_logger.log(ConsoleLogger::LogLevel::ERR, "[CLIENT] bindAnyAddress() failed.\n");
        return false;
    }
    
    //get connectEx ptr
    if (!m_lpfnConnectEx)
    {
        if (!getConnectExPtr(m_socket, m_lpfnConnectEx))
        {
            m_logger.log(ConsoleLogger::LogLevel::ERR, "[CLIENT] Failed to retrieve ConnectEx pointer.\n");
            return false;
        }
    }

    // Associate socket with IOCP
    if (!associateSocketWithIOCP(m_socket))
    {
        m_logger.log(ConsoleLogger::LogLevel::ERR, "[CLIENT] Coult not complete connection. Cailed to associate socket with IOCP.");
        disconnect();
        return false;
    }

    // Create the main client context
    m_ctx = std::make_unique<ClientContext>();
    ZeroMemory(&m_ctx->connectOverlapped, sizeof(m_ctx->connectOverlapped));
    m_ctx->operation = ClientContext::Operation::Connect;

    // Asynchronous ConnectEx
    BOOL bRet = m_lpfnConnectEx(
        m_socket,
        (sockaddr*)&m_serverAddr,
        sizeof(m_serverAddr),
        nullptr,
        0,
        nullptr,
        &m_ctx->connectOverlapped
    );
    if (!bRet && WSAGetLastError() != ERROR_IO_PENDING)
    {
        m_logger.log(ConsoleLogger::LogLevel::ERR, "[CLIENT] ConnectEx failed: " + std::to_string(WSAGetLastError()) + "\n");
        return false;
    }

    // The completion will be delivered to our IOCP thread
    // Once we get it, we'll call onConnectComplete.
    m_logger.log(ConsoleLogger::LogLevel::INFO, "[CLIENT] Connection initiated (async) to " + m_host + ":" + std::to_string(m_port) + "\n");
    return true;
}

bool AsyncIOCPClient::Send(const std::string& data)
{
    std::lock_guard<std::mutex> lock(m_ctxMutex);
    if (!m_ctx)
    {
        // not connected
        m_logger.log(ConsoleLogger::LogLevel::ERR, "[CLIENT] Send failed. Client Context does not exist.");
        return false;
    }

    // Enqueue data
    m_ctx->sendQueue.push_back(data);

    // If not currently in "Send" operation, post the next send
    if (m_ctx->operation != ClientContext::Operation::Send)
    {
        postNextSend();
    }
    return true;
}

void AsyncIOCPClient::Stop()
{
    if (!m_running) return;
    m_running = false;

    // Close the IOCP handle to break the worker thread loop
    if (m_hIOCP != INVALID_HANDLE_VALUE)
    {
        CloseHandle(m_hIOCP);
        m_hIOCP = INVALID_HANDLE_VALUE;
    }

    // Wait for worker threads
    for (HANDLE thr : m_workerThreads)
    {
        WaitForSingleObject(thr, INFINITE);
        CloseHandle(thr);
    }
    m_workerThreads.clear();

    // Close the socket
    if (m_socket != INVALID_SOCKET)
    {
        closesocket(m_socket);
        m_socket = INVALID_SOCKET;
    }

    // Cleanup context
    {
        std::lock_guard<std::mutex> lock(m_ctxMutex);
        m_ctx.reset();
    }

    // Cleanup winsock
    WSACleanup();
    m_logger.log(ConsoleLogger::LogLevel::INFO, "[CLIENT] Client stopped.");
}

bool AsyncIOCPClient::initWinsock()
{
    WSADATA wsaData;
    int ret = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (ret != 0)
    {
        m_logger.log(ConsoleLogger::LogLevel::ERR, "[CLIENT] WSAStartup failed: " + std::to_string(ret) + "\n");
        return false;
    }
    return true;
}

bool AsyncIOCPClient::createSocket()
{
    m_socket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
    if (m_socket == INVALID_SOCKET)
    {
        m_logger.log(ConsoleLogger::LogLevel::ERR, "[CLIENT] WSASocket failed: " + std::to_string(WSAGetLastError()) + "\n");
        return false;
    }
    return true;
}

bool AsyncIOCPClient::createIOCP(int workerThreadCount)
{
    m_hIOCP = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
    if (m_hIOCP == nullptr)
    {
        m_logger.log(ConsoleLogger::LogLevel::ERR, "[CLIENT] CreateIoCompletionPort failed: " + std::to_string(GetLastError()) + "\n");
        return false;
    }

    // Create worker threads
    for (int i = 0; i < workerThreadCount; ++i)
    {
        HANDLE thr = CreateThread(nullptr, 0, workerThread, this, 0, nullptr);
        if (!thr) return false;
        m_workerThreads.push_back(thr);
    }
    return true;
}

bool AsyncIOCPClient::associateSocketWithIOCP(SOCKET s, ULONG_PTR completionKey)
{
    HANDLE h = CreateIoCompletionPort(reinterpret_cast<HANDLE>(s), m_hIOCP, completionKey, 1);
    if (h == nullptr)
    {
        m_logger.log(ConsoleLogger::LogLevel::ERR, "[CLIENT] associateSocketToIOCP failed: " + std::to_string(GetLastError()) + "\n");
        return false;
    }
    return true;
}

bool AsyncIOCPClient::bindAnyAddress(SOCKET s)
{
    sockaddr_in anyAddr{};
    anyAddr.sin_family = AF_INET;
    anyAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    anyAddr.sin_port = 0; // let OS choose a random port
    if (bind(s, reinterpret_cast<sockaddr*>(&anyAddr), sizeof(anyAddr)) == SOCKET_ERROR)
    {
        m_logger.log(ConsoleLogger::LogLevel::ERR, "[CLIENT] bind failed: " + std::to_string(WSAGetLastError()) + "\n");
        return false;
    }
    return true;
}

bool AsyncIOCPClient::getConnectExPtr(SOCKET s, LPFN_CONNECTEX& outConnectEx)
{
    // Use WSAIoctl to retrieve the ConnectEx extension function pointer
    GUID guidConnectEx = WSAID_CONNECTEX;
    DWORD bytes = 0;
    int result = WSAIoctl(
        s,
        SIO_GET_EXTENSION_FUNCTION_POINTER,
        &guidConnectEx,
        sizeof(guidConnectEx),
        &outConnectEx,
        sizeof(outConnectEx),
        &bytes,
        nullptr,
        nullptr
    );
    if (result == SOCKET_ERROR)
    {
        m_logger.log(ConsoleLogger::LogLevel::ERR, "[CLIENT] WSAIoctl for ConnectEx failed: "  + std::to_string(WSAGetLastError()) + "\n");
        return false;
    }
    return true;
}

bool AsyncIOCPClient::postRecv(CLIENT_IO_CONTEXT* ctx)
{
    ZeroMemory(&ctx->overlapped, sizeof(OVERLAPPED));
    ctx->wsabuf.buf = ctx->buffer;
    ctx->wsabuf.len = sizeof(ctx->buffer);
    ctx->operation = OP_RECV;

    DWORD flags = 0;
    DWORD bytesRecv = 0;
    int ret = WSARecv(m_socket, &ctx->wsabuf, 1, &bytesRecv, &flags, &ctx->overlapped, nullptr);
    if (ret == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING)
    {
        m_logger.log(ConsoleLogger::LogLevel::ERR, "[CLIENT] WSARecv failed: " + std::to_string(WSAGetLastError()) + "\n");
        return false;
    }
    return true;
}

DWORD WINAPI AsyncIOCPClient::workerThread(LPVOID lpParam)
{
    AsyncIOCPClient* client = reinterpret_cast<AsyncIOCPClient*>(lpParam);
    DWORD bytesTransferred = 0;
    ULONG_PTR compKey = 0;
    OVERLAPPED* pOverlapped = nullptr;

    while (true)
    {
        BOOL success = GetQueuedCompletionStatus(
            client->m_hIOCP,
            &bytesTransferred,
            &compKey,
            &pOverlapped,
            INFINITE
        );
        if (!success && pOverlapped == nullptr)
        {
            ConsoleLogger::getInstance().log(ConsoleLogger::LogLevel::ERR, "[CLIENT] IOCP handle closed. Shutting down.");
            break;
        }
        if (!pOverlapped) continue;

        // handleIO will figure out what operation finished
        client->handleIO(bytesTransferred, pOverlapped);
    }
    return 0;
}

void AsyncIOCPClient::handleIO(DWORD bytesTransferred, OVERLAPPED* pOverlapped)
{
    std::lock_guard<std::mutex> lock(m_ctxMutex);
    if (!m_ctx)
    {
        // may have disconnected
        m_logger.log(ConsoleLogger::LogLevel::ERR, "[CLIENT] Client context null in handleIO.");
        return;
    }
    // Check which overlapped
    if (pOverlapped == &m_ctx->connectOverlapped)
    {
        onConnectComplete(m_ctx.get(), bytesTransferred);
    }
    else if (pOverlapped == &m_ctx->recvOverlapped)
    {
        onRecvComplete(m_ctx.get(), bytesTransferred);
    }
    else if (pOverlapped == &m_ctx->sendOverlapped)
    {
        onSendComplete(m_ctx.get(), bytesTransferred);
    }
}

void AsyncIOCPClient::onConnectComplete(ClientContext* ctx, DWORD bytesTransferred)
{
    // Confirm the ConnectEx operation completed successfully
    DWORD flags = 0;
    DWORD bytes = 0;
    BOOL result = WSAGetOverlappedResult(
        m_socket,
        &m_ctx->connectOverlapped,
        &bytes,
        FALSE,   // don't wait (should already be completed)
        &flags
    );

    if (!result)
    {
        m_logger.log(ConsoleLogger::LogLevel::ERR, "[CLIENT] ConnectEx did not complete successfully. Error: " + std::to_string(WSAGetLastError()) + "\n");
        disconnect();
        return;
    }

    int r = setsockopt(m_socket, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, NULL, 0);
    if (r == SOCKET_ERROR)
    {
        m_logger.log(ConsoleLogger::LogLevel::ERR, "[CLIENT] Could not complete connection: "+ std::to_string(WSAGetLastError()) + ".");
        disconnect();
        return;
    }

    // Mark connected
    ctx->connected = true;
    m_logger.log(ConsoleLogger::LogLevel::INFO, "[CLIENT] Connection complete.");

    // Fire onConnect callback
    if (m_onConnect)
    {
        m_onConnect();
    }

    // If there are queued messages, start sending them
    if (!ctx->sendQueue.empty())
    {
        postNextSend();
    }

    // Post first recv
    postRecv();
}

void AsyncIOCPClient::onRecvComplete(ClientContext* ctx, DWORD bytesTransferred)
{
    if (bytesTransferred == 0)
    {
        // Sevrer closed. Fire onDisconnect callback
        m_logger.log(ConsoleLogger::LogLevel::ERR, "[CLIENT] Did not receive data. Server closed.");
        if (m_onDisconnect)
        {
            m_onDisconnect();
        }
        disconnect();
        return;
    }

    // We got data
    std::string msg(ctx->recvBuffer, bytesTransferred);

    // Fire onDataReceived callback
    if (m_onDataReceived)
    {
        m_onDataReceived(msg);
    }

    // Post another recv
    postRecv();
}

bool AsyncIOCPClient::postRecv()
{
    if (!m_ctx || !m_ctx->connected)
    {
        m_logger.log(ConsoleLogger::LogLevel::ERR, "[CLIENT] Could not receive data. Not connected.");
        return false;
    }

    ZeroMemory(&m_ctx->recvOverlapped, sizeof(m_ctx->recvOverlapped));
    m_ctx->recvWSABuf.buf = m_ctx->recvBuffer;
    m_ctx->recvWSABuf.len = sizeof(m_ctx->recvBuffer);

    DWORD flags = 0;
    DWORD bytesRecv = 0;
    int ret = WSARecv(m_socket, &m_ctx->recvWSABuf, 1, &bytesRecv, &flags, &m_ctx->recvOverlapped, nullptr);
    if (ret == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING)
    {
        m_logger.log(ConsoleLogger::LogLevel::ERR, "[CLIENT] Failed to post WSARecv.");
        disconnect();
        return false;
    }
    return true;
}

void AsyncIOCPClient::onSendComplete(ClientContext* ctx, DWORD bytesTransferred)
{
    if (bytesTransferred == 0)
    {
        m_logger.log(ConsoleLogger::LogLevel::ERR, "[CLIENT] Could not perform send.");
        if (m_onDisconnect)
        {
            m_onDisconnect();
        }
        disconnect();
        return;
    }

    ctx->bytesSentSoFar += bytesTransferred;
    const auto& frontMsg = ctx->sendQueue.front();
    if (ctx->bytesSentSoFar < frontMsg.size())
    {
        // partial send
        size_t remaining = frontMsg.size() - ctx->bytesSentSoFar;
        size_t chunk = std::min(remaining, sizeof(ctx->sendBuffer));
        memcpy(ctx->sendBuffer,
            frontMsg.data() + ctx->bytesSentSoFar,
            chunk);

        ctx->sendWSABuf.buf = ctx->sendBuffer;
        ctx->sendWSABuf.len = (ULONG)chunk;

        ZeroMemory(&ctx->sendOverlapped, sizeof(ctx->sendOverlapped));
        DWORD flags = 0, sent = 0;
        int ret = WSASend(m_socket, &ctx->sendWSABuf, 1, &sent, flags, &ctx->sendOverlapped, nullptr);
        if (ret == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING)
        {
            m_logger.log(ConsoleLogger::LogLevel::ERR, "[CLIENT] Failed to post next WSASend.");
            disconnect();
        }
    }
    else
    {
        // front message done
        ctx->sendQueue.pop_front();
        if (!ctx->sendQueue.empty())
        {
            // send next
            postNextSend();
        }
        else
        {
            // no more data => idle
            ctx->operation = ClientContext::Operation::Recv;
        }
    }
}

void AsyncIOCPClient::postNextSend()
{
    if (!m_ctx)
    {
        m_logger.log(ConsoleLogger::LogLevel::ERR, "[CLIENT] Could not post next send. Client context does not exist.");
        return;
    }
    if (m_ctx->sendQueue.empty())
    {
        m_logger.log(ConsoleLogger::LogLevel::ERR, "[CLIENT] Could not post next send. No data to send.");
        return;
    }

    // Prepare first chunk
    const std::string& frontMsg = m_ctx->sendQueue.front();
    m_ctx->bytesToSend = frontMsg.size();
    m_ctx->bytesSentSoFar = 0;

    size_t chunk = std::min(frontMsg.size(), sizeof(m_ctx->sendBuffer));
    memcpy(m_ctx->sendBuffer, frontMsg.data(), chunk);

    m_ctx->sendWSABuf.buf = m_ctx->sendBuffer;
    m_ctx->sendWSABuf.len = (ULONG)chunk;

    m_ctx->operation = ClientContext::Operation::Send;
    ZeroMemory(&m_ctx->sendOverlapped, sizeof(m_ctx->sendOverlapped));

    DWORD flags = 0, bytesSent = 0;
    int ret = WSASend(m_socket, &m_ctx->sendWSABuf, 1, &bytesSent, flags,
        &m_ctx->sendOverlapped, nullptr);
    if (ret == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING)
    {
        m_logger.log(ConsoleLogger::LogLevel::ERR, "[CLIENT] Failed to post WSASend.");
        disconnect();
    }
}

void AsyncIOCPClient::disconnect()
{
    m_logger.log(ConsoleLogger::LogLevel::INFO, "[CLIENT] Disconnecting...");
    // Close the socket
    if (m_socket != INVALID_SOCKET)
    {
        closesocket(m_socket);
        m_socket = INVALID_SOCKET;
    }

    // Clear context
    if (m_ctx) 
    {
        m_ctx->connected = false;
        m_ctx.reset();
    }
}
