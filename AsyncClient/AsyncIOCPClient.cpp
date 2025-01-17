#include "AsyncIOCPClient.h"

AsyncIOCPClient::AsyncIOCPClient(const std::string& host, int port)
    : m_host(host),
    m_port(port),
    m_socket(INVALID_SOCKET),
    m_hIOCP(NULL),
    m_hWorkerThread(NULL),
    m_running(false),
    m_ioContext(nullptr),
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

bool AsyncIOCPClient::Start()
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
    if (!createIOCP())
    {
        m_logger.log(ConsoleLogger::LogLevel::ERR, "[CLIENT] IOCP creation failed.\n");
        return false;
    }

    // Create a worker thread to handle all I/O completions
    m_running = true;
    m_hWorkerThread = CreateThread(nullptr, 0, workerThread, this, 0, nullptr);
    if (!m_hWorkerThread)
    {
        m_logger.log(ConsoleLogger::LogLevel::ERR, "[CLIENT] CreateThread failed: " + std::to_string(GetLastError()) + "\n");
        return false;
    }

    return true;
}

bool AsyncIOCPClient::Connect()
{
    // Convert m_host (std::string) to std::wstring
    std::wstring wHost(m_host.begin(), m_host.end());

    // Convert the string host to a numeric address.
    if (InetPton(AF_INET, wHost.c_str(), &m_serverAddr.sin_addr) <= 0)
    {
        m_logger.log(ConsoleLogger::LogLevel::ERR, "[CLIENT] Invalid host or InetPton failed.\n");
        return false;
    }

    // overlapped connect using ConnectEx.
    LPFN_CONNECTEX lpfnConnectEx = nullptr;
    if (!getConnectExPtr(m_socket, lpfnConnectEx))
    {
        m_logger.log(ConsoleLogger::LogLevel::ERR, "[CLIENT] Failed to retrieve ConnectEx pointer.\n");
        return false;
    }

    // Bind the socket to any local IP/port before using ConnectEx.
    if (!bindAnyAddress(m_socket))
    {
        m_logger.log(ConsoleLogger::LogLevel::ERR, "[CLIENT] bindAnyAddress() failed.\n");
        return false;
    }

    // Associate the socket with the IOCP
    if (!associateSocketToIOCP(m_socket))
    {
        m_logger.log(ConsoleLogger::LogLevel::ERR, "[CLIENT] Failed to associate socket with IOCP.\n");
        return false;
    }

    // Create an IO context for this connection
    m_ioContext = new CLIENT_IO_CONTEXT{};
    m_ioContext->wsabuf.buf = m_ioContext->buffer;
    m_ioContext->wsabuf.len = sizeof(m_ioContext->buffer);
    m_ioContext->operation = OP_CONNECT;

    // Use ConnectEx in an overlapped manner
    ZeroMemory(&m_ioContext->overlapped, sizeof(OVERLAPPED));

    // parameters can be 0 if no message is sent with connect
    BOOL bRet = lpfnConnectEx(
        m_socket,
        reinterpret_cast<sockaddr*>(&m_serverAddr),
        sizeof(m_serverAddr),
        nullptr,
        0,
        nullptr,
        &m_ioContext->overlapped
    );
    if (!bRet)
    {
        int err = WSAGetLastError();
        if (err != ERROR_IO_PENDING)
        {
            m_logger.log(ConsoleLogger::LogLevel::ERR, "[CLIENT] ConnectEx failed: " + std::to_string(err) + "\n");
            return false;
        }
        // If it's ERROR_IO_PENDING, the connection is in progress
    }

    m_logger.log(ConsoleLogger::LogLevel::INFO, "[CLIENT] Connection initiated (async) to "
         + m_host  + ":" + std::to_string(m_port) + "\n");
    return true;
}

bool AsyncIOCPClient::Send(const std::string& data)
{
    if (!m_running || m_socket == INVALID_SOCKET)
    {
        m_logger.log(ConsoleLogger::LogLevel::ERR, "[CLIENT] Socket not connected or client not started.\n");
        return false;
    }

    // Allocate a fresh IO context for sending
    CLIENT_IO_CONTEXT* sendCtx = new CLIENT_IO_CONTEXT{};
    ZeroMemory(&sendCtx->overlapped, sizeof(OVERLAPPED));
    sendCtx->operation = OP_SEND;

    // Copy data into the buffer
    size_t len = data.size();
    if (len > sizeof(sendCtx->buffer))
    {
        m_logger.log(ConsoleLogger::LogLevel::ERR, "[CLIENT] Data too large for buffer.\n");
        delete sendCtx;
        return false;
    }
    memcpy(sendCtx->buffer, data.c_str(), len);

    sendCtx->wsabuf.buf = sendCtx->buffer;
    sendCtx->wsabuf.len = static_cast<ULONG>(len);

    // Asynchronously send
    DWORD bytesSent = 0;
    DWORD flags = 0;
    int ret = WSASend(m_socket, &sendCtx->wsabuf, 1, &bytesSent, flags,
        &sendCtx->overlapped, nullptr);
    if (ret == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING)
    {
        m_logger.log(ConsoleLogger::LogLevel::ERR, "[CLIENT] WSASend failed: " + std::to_string(WSAGetLastError()) + "\n");
        closesocket(m_socket);
        delete sendCtx;
        return false;
    }
    return true;
}

void AsyncIOCPClient::Stop()
{
    if (!m_running)
        return;

    m_running = false;

    // Close IOCP (this will make GetQueuedCompletionStatus fail in worker thread)
    CloseHandle(m_hIOCP);
    m_hIOCP = NULL;

    // Wait for worker thread to exit
    if (m_hWorkerThread)
    {
        WaitForSingleObject(m_hWorkerThread, INFINITE);
        CloseHandle(m_hWorkerThread);
        m_hWorkerThread = NULL;
    }

    // Close socket
    if (m_socket != INVALID_SOCKET)
    {
        closesocket(m_socket);
        m_socket = INVALID_SOCKET;
    }

    // Cleanup the main IO context if allocated
    if (m_ioContext)
    {
        delete m_ioContext;
        m_ioContext = nullptr;
    }

    // Cleanup Winsock
    WSACleanup();

    m_logger.log(ConsoleLogger::LogLevel::INFO, "[CLIENT] Stopped.\n");
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

bool AsyncIOCPClient::createIOCP()
{
    m_hIOCP = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
    if (m_hIOCP == nullptr)
    {
        m_logger.log(ConsoleLogger::LogLevel::ERR, "[CLIENT] CreateIoCompletionPort failed: " + std::to_string(GetLastError()) + "\n");
        return false;
    }
    return true;
}

bool AsyncIOCPClient::associateSocketToIOCP(SOCKET s, ULONG_PTR completionKey)
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
    ctx->operation == OP_RECV;

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
    ULONG_PTR completionKey = 0;
    OVERLAPPED* pOverlapped = nullptr;

    while (true)
    {
        BOOL success = GetQueuedCompletionStatus(
            client->m_hIOCP,
            &bytesTransferred,
            &completionKey,
            &pOverlapped,
            INFINITE
        );
        if (!success && pOverlapped == nullptr)
        {
            // IOCP closed or serious error
            // This likely means the client is shutting down
            break;
        }
        if (!pOverlapped) {
            break;
        }

        CLIENT_IO_CONTEXT* ioCtx = reinterpret_cast<CLIENT_IO_CONTEXT*>(pOverlapped);

        if (bytesTransferred == 0 && ioCtx->operation != OP_CONNECT)
        {
            // This usually means the remote side closed the connection
            ConsoleLogger::getInstance().log(ConsoleLogger::LogLevel::ERR, "[CLIENT] Connection closed by server or zero-byte I/O.\n");
            closesocket(client->m_socket);
            delete ioCtx; // free this context
            client->m_ioContext = nullptr;
            break;
        }

        // Let the instance handle the completion
        client->handleIO(bytesTransferred, ioCtx);
    }

    return 0;
}

void AsyncIOCPClient::handleIO(DWORD bytesTransferred, CLIENT_IO_CONTEXT* ioCtx)
{
    // We need to figure out if this is the result of a ConnectEx, a Send, or a Recv

    // If we haven't posted a recv yet, assume we've just connected
    // so we can post the first recv now.
    if (ioCtx->operation == OP_CONNECT)
    {
        // This might be the completion of ConnectEx if no data was in the buffer.
        m_logger.log(ConsoleLogger::LogLevel::INFO, "[CLIENT] Connection established!\n");

        int result = setsockopt(m_socket, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, NULL, 0);
        if (result == SOCKET_ERROR)
        {
            m_logger.log(ConsoleLogger::LogLevel::ERR, "[CLIENT] setsockopt(SO_UPDATE_CONNECT_CONTEXT) failed: "
                + std::to_string(WSAGetLastError()) + "\n");
        }
        else
        {
            m_logger.log(ConsoleLogger::LogLevel::INFO, "[CLIENT] setsockopt(SO_UPDATE_CONNECT_CONTEXT) succeeded.\n");
        }

        // Post a read so we can receive data from the server
        if (!postRecv(ioCtx))
        {
            m_logger.log(ConsoleLogger::LogLevel::ERR, "[CLIENT] postRecv failed after connect.\n");
        }
        return;
    }

    // Otherwise, if we have data in buffer, interpret it as received data
    // and maybe post another recv.
    if (bytesTransferred > 0)
    {
        std::string received(ioCtx->buffer, bytesTransferred);
        m_logger.log(ConsoleLogger::LogLevel::INFO, "[CLIENT] Received: " + received + "\n");
    }

    // Post another recv for continuous reading
    if (!postRecv(ioCtx))
    {
        m_logger.log(ConsoleLogger::LogLevel::ERR, "[CLIENT] postRecv failed.\n");
    }
}
