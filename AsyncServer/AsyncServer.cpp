#include "AsyncServer.h"

AsyncIOCPServer::AsyncIOCPServer(int port, int workerThreadCount)
    : m_port(port),
    m_workerThreadCount(workerThreadCount),
    m_listenSocket(INVALID_SOCKET),
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

bool AsyncIOCPServer::Start()
{
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

    CloseHandle(m_hIOCP);
    m_hIOCP = NULL;

    // Wait for worker threads to exit
    for (HANDLE thread : m_workerThreads) {
        WaitForSingleObject(thread, INFINITE);
        CloseHandle(thread);
    }
    m_workerThreads.clear();

    // Close the listening socket
    if (m_listenSocket != INVALID_SOCKET) {
        closesocket(m_listenSocket);
        m_listenSocket = INVALID_SOCKET;
    }

    // Cleanup Winsock
    WSACleanup();

    m_running = false;
    m_logger.log(ConsoleLogger::LogLevel::INFO, "[SERVER] Stopped.\n");
}

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

bool AsyncIOCPServer::postInitialAccepts(int count)
{
    for (int i = 0; i < count; ++i)
    {
        SOCKET acceptSocket;
        if (!createAcceptSocket(acceptSocket)) 
        {
            return false;
        }

        // Allocate a context
        PER_SOCKET_CONTEXT* ctx = new PER_SOCKET_CONTEXT{};
        ctx->socket = acceptSocket;
        ctx->operation = OP_ACCEPT;
        m_logger.log(ConsoleLogger::LogLevel::VERBOSE, "[SERVER] Posting AcceptEx with OP_ACCEPT.\n");

        // Post AcceptEx        
        if (!postAccept(m_listenSocket, acceptSocket, ctx))
        {
            return false;
        }
    }
    return true;
}

bool AsyncIOCPServer::createAcceptSocket(SOCKET& acceptSocket)
{
    acceptSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (acceptSocket == INVALID_SOCKET)
    {
        m_logger.log(ConsoleLogger::LogLevel::ERR, "[SERVER] createAcceptSocket failed: " + std::to_string(WSAGetLastError()) + "\n");
        return false;
    }
    return true;
}

bool AsyncIOCPServer::postAccept(SOCKET listenSock, SOCKET acceptSock, PER_SOCKET_CONTEXT* ctx)
{
    ZeroMemory(&ctx->overlapped, sizeof(OVERLAPPED));
    ctx->wsabuf.buf = ctx->buffer;
    ctx->wsabuf.len = sizeof(ctx->buffer);

    DWORD bytesReceived = 0;
    GUID guidAcceptEx = WSAID_ACCEPTEX;
    LPFN_ACCEPTEX lpfnAcceptEx = NULL;
    DWORD dwBytes = 0;

    // Retrieve the AcceptEx function pointer
    if (WSAIoctl(
        listenSock,
        SIO_GET_EXTENSION_FUNCTION_POINTER,
        &guidAcceptEx,
        sizeof(guidAcceptEx),
        &lpfnAcceptEx,
        sizeof(lpfnAcceptEx),
        &dwBytes,
        NULL,
        NULL
    ) == SOCKET_ERROR)
    {
        m_logger.log(ConsoleLogger::LogLevel::ERR, "[SERVER] WSAIoctl for AcceptEx failed: " + std::to_string(WSAGetLastError()) + "\n");
        return false;
    }

    BOOL bRet = lpfnAcceptEx(
        listenSock,
        acceptSock,
        ctx->buffer,
        0,  // No extra data
        sizeof(sockaddr_in) + 16, // local sock addr size
        sizeof(sockaddr_in) + 16, // remote sock addr size
        &bytesReceived,
        &ctx->overlapped
    );

    if (!bRet && WSAGetLastError() != ERROR_IO_PENDING)
    {
        m_logger.log(ConsoleLogger::LogLevel::ERR, "[SERVER] AcceptEx failed: " + std::to_string(WSAGetLastError()) + "\n");
        return false;
    }

    //associate the accept socket with IOCP
    if (!associateDeviceToIOCP(reinterpret_cast<HANDLE>(acceptSock)))
    {
        return false;
    }

    return true;
}

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

        // Convert the overlapped pointer back to our context
        PER_SOCKET_CONTEXT* ctx = reinterpret_cast<PER_SOCKET_CONTEXT*>(pOverlapped);
        server->handleIO(bytesTransferred, ctx);
    }

    return 0;
}

void AsyncIOCPServer::handleIO(DWORD bytesTransferred, PER_SOCKET_CONTEXT* ctx)
{
    DWORD op = ctx->operation;
    if (bytesTransferred == 0)
    {
        switch (op) 
        {
        case OP_ACCEPT:
        {
            // If it’s AcceptEx with 0 bytes, this just means the client connected 
            // but didn't send any data immediately.
            m_logger.log(ConsoleLogger::LogLevel::VERBOSE, "[SERVER] OP_ACCEPT returned 0 bytes.");
            
            // Update the accept socket context
            int result = setsockopt(
                ctx->socket,
                SOL_SOCKET,
                SO_UPDATE_ACCEPT_CONTEXT,
                (char*)&m_listenSocket,
                sizeof(m_listenSocket)
            );
            if (result == SOCKET_ERROR)
            {
                m_logger.log(ConsoleLogger::LogLevel::ERR, "[SERVER] setsockopt SO_UPDATE_ACCEPT_CONTEXT failed: "
                    + std::to_string(WSAGetLastError()) + "\n");
                closesocket(ctx->socket);
                delete ctx;
                return;
            }
            
            ctx->operation = OP_RECV; // Mark that we're now going to do a receive
            ZeroMemory(&ctx->overlapped, sizeof(OVERLAPPED));
            ctx->wsabuf.buf = ctx->buffer;
            ctx->wsabuf.len = sizeof(ctx->buffer);

            DWORD flags = 0;
            m_logger.log(ConsoleLogger::LogLevel::VERBOSE, "[SERVER] Posting Recv after OP_ACCEPT");
            int ret = WSARecv(ctx->socket, &ctx->wsabuf, 1, NULL, &flags,
                &ctx->overlapped, NULL);
            if (ret == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING)
            {
                m_logger.log(ConsoleLogger::LogLevel::ERR, "[SERVER] WSARecv failed after AcceptEx (0 bytes): "
                    + std::to_string(WSAGetLastError()) + "\n");
                closesocket(ctx->socket);
                delete ctx;
            }
            
        }
            break;

        case OP_RECV:
            m_logger.log(ConsoleLogger::LogLevel::VERBOSE, "[SERVER] OP_RECV returned 0 bytes.");
            // Zero bytes on a RECV means client disconnected
            m_logger.log(ConsoleLogger::LogLevel::ERR, "[WORKER] Zero bytes; closing socket.\n");
            closesocket(ctx->socket);
            delete ctx;
            break;

        case OP_SEND:
            m_logger.log(ConsoleLogger::LogLevel::VERBOSE, "[SERVER] OP_SEND returned 0 bytes");
            // Zero bytes on a SEND is uncommon
            m_logger.log(ConsoleLogger::LogLevel::ERR, "[SERVER] 0 bytes on SEND - closing socket.\n");
            closesocket(ctx->socket);
            delete ctx;
            break;
        }
    }
    else // bytesTransferred > 0
    {
        switch (ctx->operation)
        {
        case OP_ACCEPT:
        {
            // AcceptEx completed with immediate data in the buffer.
            // process the data, then post WSARecv.
            m_logger.log(ConsoleLogger::LogLevel::VERBOSE, "[SERVER] OP_ACCEPT returned >0 bytes.");
            {
                // Update the accept socket context
                int result = setsockopt(
                    ctx->socket,
                    SOL_SOCKET,
                    SO_UPDATE_ACCEPT_CONTEXT,
                    (char*)&m_listenSocket,
                    sizeof(m_listenSocket)
                );
                if (result == SOCKET_ERROR) {
                    m_logger.log(ConsoleLogger::LogLevel::ERR, "[SERVER] setsockopt SO_UPDATE_ACCEPT_CONTEXT failed: "
                        + std::to_string(WSAGetLastError()) + "\n");
                    closesocket(ctx->socket);
                    delete ctx;
                    return;
                }
            }

            m_logger.log(ConsoleLogger::LogLevel::INFO, "[SERVER] AcceptEx completed with " + std::to_string(bytesTransferred)
                + " bytes of initial data.\n");

            // Possibly handle the data in ctx->buffer[0..bytesTransferred-1]
            // Then post a WSARecv:
            ctx->operation = OP_RECV;
            ZeroMemory(&ctx->overlapped, sizeof(OVERLAPPED));
            ctx->wsabuf.buf = ctx->buffer;
            ctx->wsabuf.len = sizeof(ctx->buffer);

            DWORD flags = 0;
            m_logger.log(ConsoleLogger::LogLevel::VERBOSE, "[SERVER] Posting Recv after OP_ACCEPT");
            int ret = WSARecv(ctx->socket, &ctx->wsabuf, 1, NULL, &flags,
                &ctx->overlapped, NULL);
            if (ret == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING)
            {
                m_logger.log(ConsoleLogger::LogLevel::ERR, "[SERVER] WSARecv failed after AcceptEx (>0 bytes): "
                    + std::to_string(WSAGetLastError()) + "\n");
                closesocket(ctx->socket);
                delete ctx;
            }
        }
        break;

        case OP_RECV:
        {            
            // We got data from the client. Process it.
            m_logger.log(ConsoleLogger::LogLevel::VERBOSE, "[SERVER] OP_RECV returned >0 bytes.");

            std::string msg(ctx->buffer, bytesTransferred);
            m_logger.log(ConsoleLogger::LogLevel::INFO, "[SERVER] Received from client: " + msg + "\n");

            ctx->operation = OP_RECV; // stay in RECV mode
            ZeroMemory(&ctx->overlapped, sizeof(OVERLAPPED));
            ctx->wsabuf.buf = ctx->buffer;
            ctx->wsabuf.len = sizeof(ctx->buffer);

            DWORD flags = 0;
            m_logger.log(ConsoleLogger::LogLevel::VERBOSE, "[SERVER] Posting Recv with OP_RECV");
            int ret = WSARecv(ctx->socket, &ctx->wsabuf, 1, NULL, &flags,
                &ctx->overlapped, NULL);
            if (ret == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING)
            {
                m_logger.log(ConsoleLogger::LogLevel::ERR, "[SERVER] WSARecv failed: " + std::to_string(WSAGetLastError()) + "\n");
                closesocket(ctx->socket);
                delete ctx;
            }
        }
        break;

        case OP_SEND:
            // We finished sending some data to the client
            m_logger.log(ConsoleLogger::LogLevel::VERBOSE, "[SERVER] OP_SEND returned >0 bytes.");
            m_logger.log(ConsoleLogger::LogLevel::INFO, "[SERVER] Finished sending " + std::to_string(bytesTransferred) + " bytes.\n");
        break;
        }
    }
}
