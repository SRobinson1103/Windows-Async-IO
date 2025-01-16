#include "AsyncServer.h"

// --------------------------------------------------
// Constructor / Destructor
// --------------------------------------------------

AsyncIOCPServer::AsyncIOCPServer(int port, int workerThreadCount)
    : m_port(port),
    m_workerThreadCount(workerThreadCount),
    m_listenSocket(INVALID_SOCKET),
    m_hIOCP(NULL),
    m_running(false)
{
}

AsyncIOCPServer::~AsyncIOCPServer()
{
    Stop(); // Ensure cleanup if not already done
}

// --------------------------------------------------
// Public Methods
// --------------------------------------------------

bool AsyncIOCPServer::Start()
{
    if (!initWinsock()) {
        std::cerr << "[SERVER] Winsock initialization failed.\n";
        return false;
    }

    if (!createListenSocket()) {
        std::cerr << "[SERVER] createListenSocket failed.\n";
        return false;
    }

    // Create IO Completion Port
    m_hIOCP = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, m_workerThreadCount);
    if (!m_hIOCP) {
        std::cerr << "[SERVER] CreateIoCompletionPort failed: " << GetLastError() << "\n";
        return false;
    }

    // Associate the listen socket with IOCP
    if (!associateDeviceToIOCP(reinterpret_cast<HANDLE>(m_listenSocket))) {
        std::cerr << "[SERVER] Failed to associate listen socket to IOCP.\n";
        return false;
    }

    // Create worker threads
    for (int i = 0; i < m_workerThreadCount; ++i) {
        HANDLE hThread = CreateThread(
            nullptr, 0, workerThread, this, 0, nullptr
        );
        if (!hThread) {
            std::cerr << "[SERVER] CreateThread failed: " << GetLastError() << "\n";
            return false;
        }
        m_workerThreads.push_back(hThread);
    }

    // Post initial AcceptEx calls
    if (!postInitialAccepts(m_workerThreadCount)) {
        std::cerr << "[SERVER] postInitialAccepts failed.\n";
        return false;
    }

    std::cout << "[SERVER] Listening on port " << m_port << "...\n";
    m_running = true;

    return true;
}

void AsyncIOCPServer::Stop()
{
    if (!m_running) return;

    // Signal threads to stop if needed (in advanced scenarios).
    // For now, we just close the IOCP handle to break GetQueuedCompletionStatus.

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
    std::cout << "[SERVER] Stopped.\n";
}

// --------------------------------------------------
// Private Methods
// --------------------------------------------------

bool AsyncIOCPServer::initWinsock()
{
    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        std::cerr << "[SERVER] WSAStartup failed with error: " << result << "\n";
        return false;
    }
    return true;
}

bool AsyncIOCPServer::createListenSocket()
{
    m_listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_listenSocket == INVALID_SOCKET) {
        std::cerr << "[SERVER] socket() failed: " << WSAGetLastError() << "\n";
        return false;
    }

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    serverAddr.sin_port = htons(static_cast<u_short>(m_port));

    if (bind(m_listenSocket, reinterpret_cast<SOCKADDR*>(&serverAddr), sizeof(serverAddr)) == SOCKET_ERROR) {
        std::cerr << "[SERVER] bind() failed: " << WSAGetLastError() << "\n";
        closesocket(m_listenSocket);
        m_listenSocket = INVALID_SOCKET;
        return false;
    }

    if (listen(m_listenSocket, SOMAXCONN) == SOCKET_ERROR) {
        std::cerr << "[SERVER] listen() failed: " << WSAGetLastError() << "\n";
        closesocket(m_listenSocket);
        m_listenSocket = INVALID_SOCKET;
        return false;
    }

    return true;
}

bool AsyncIOCPServer::associateDeviceToIOCP(HANDLE device, ULONG_PTR completionKey)
{
    HANDLE h = CreateIoCompletionPort(device, m_hIOCP, completionKey, m_workerThreadCount);
    if (!h) {
        std::cerr << "[SERVER] CreateIoCompletionPort (associate device) failed: " << GetLastError() << "\n";
        return false;
    }
    return true;
}

bool AsyncIOCPServer::postInitialAccepts(int count)
{
    for (int i = 0; i < count; ++i) {
        SOCKET acceptSocket;
        if (!createAcceptSocket(acceptSocket)) {
            return false;
        }

        // Allocate a context
        PER_SOCKET_CONTEXT* ctx = new PER_SOCKET_CONTEXT{};
        ctx->socket = acceptSocket;
        ctx->operation = OP_ACCEPT;

        // Post AcceptEx        
        if (!postAccept(m_listenSocket, acceptSocket, ctx)) {
            return false;
        }
    }
    return true;
}

bool AsyncIOCPServer::createAcceptSocket(SOCKET& acceptSocket)
{
    acceptSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (acceptSocket == INVALID_SOCKET) {
        std::cerr << "[SERVER] createAcceptSocket failed: " << WSAGetLastError() << "\n";
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
        std::cerr << "[SERVER] WSAIoctl for AcceptEx failed: " << WSAGetLastError() << "\n";
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

    if (!bRet && WSAGetLastError() != ERROR_IO_PENDING) {
        std::cerr << "[SERVER] AcceptEx failed: " << WSAGetLastError() << "\n";
        return false;
    }

    // Optionally associate the accept socket with IOCP now. However,
    // typically you do this after AcceptEx completes. 
    // We'll do it here so we catch completions for this socket on the same IOCP.
    if (!associateDeviceToIOCP(reinterpret_cast<HANDLE>(acceptSock))) {
        return false;
    }

    return true;
}

// --------------------------------------------------
// Static Worker Thread Entry
// --------------------------------------------------

DWORD WINAPI AsyncIOCPServer::workerThread(LPVOID lpParam)
{
    AsyncIOCPServer* server = reinterpret_cast<AsyncIOCPServer*>(lpParam);

    DWORD bytesTransferred = 0;
    ULONG_PTR completionKey = 0;
    OVERLAPPED* pOverlapped = nullptr;

    while (true) {
        BOOL success = GetQueuedCompletionStatus(
            server->m_hIOCP,
            &bytesTransferred,
            &completionKey,
            &pOverlapped,
            INFINITE
        );

        // If pOverlapped == nullptr, the IOCP is probably closed (server stopping),
        // or a serious error occurred. Let's break out.
        if (!success && pOverlapped == nullptr) {
            std::cerr << "[WORKER] GQCS failed, or server is shutting down. LastError: "
                << GetLastError() << "\n";
            break;
        }

        if (!pOverlapped) {
            // If no overlapped structure is returned, we can't do further processing
            continue;
        }

        // Convert the overlapped pointer back to our context
        PER_SOCKET_CONTEXT* ctx = reinterpret_cast<PER_SOCKET_CONTEXT*>(pOverlapped);
        server->handleIO(bytesTransferred, ctx);
    }

    return 0;
}

// --------------------------------------------------
// Handle IO completions
// --------------------------------------------------

void AsyncIOCPServer::handleIO(DWORD bytesTransferred, PER_SOCKET_CONTEXT* ctx)
{
    DWORD op = ctx->operation;
    if (bytesTransferred == 0) {
        switch (op) {
        case OP_ACCEPT:
            // (A) If it’s AcceptEx with 0 bytes, this just means the client connected 
            // but didn't send any data immediately.
            // -> DO NOT close the socket. Instead, we want to post a WSARecv.
            // 
            // Step #4 from the previous instructions: 
            //     "Ensure you post a WSARecv after AcceptEx"
            
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
                    std::cerr << "[SERVER] setsockopt SO_UPDATE_ACCEPT_CONTEXT failed: "
                        << WSAGetLastError() << "\n";
                    closesocket(ctx->socket);
                    delete ctx;
                    return;
                }
            }
            ctx->operation = OP_RECV; // Mark that we're now going to do a receive
            ZeroMemory(&ctx->overlapped, sizeof(OVERLAPPED));
            ctx->wsabuf.buf = ctx->buffer;
            ctx->wsabuf.len = sizeof(ctx->buffer);

            {
                DWORD flags = 0;
                int ret = WSARecv(ctx->socket, &ctx->wsabuf, 1, NULL, &flags,
                    &ctx->overlapped, NULL);
                if (ret == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING)
                {
                    std::cerr << "[SERVER] WSARecv failed after AcceptEx (0 bytes): "
                        << WSAGetLastError() << "\n";
                    closesocket(ctx->socket);
                    delete ctx;
                }
            }
            break;

        case OP_RECV:
            // Zero bytes on a RECV means client disconnected
            std::cerr << "[WORKER] Zero bytes; closing socket.\n";
            closesocket(ctx->socket);
            delete ctx;
            break;

        case OP_SEND:
            // Zero bytes on a SEND is less common; handle if needed
            std::cerr << "[SERVER] 0 bytes on SEND - closing socket.\n";
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
            // (A) AcceptEx completed WITH immediate data in the buffer.
            // You can process that data if you want. Then, post WSARecv.
        {
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
                    std::cerr << "[SERVER] setsockopt SO_UPDATE_ACCEPT_CONTEXT failed: "
                        << WSAGetLastError() << "\n";
                    closesocket(ctx->socket);
                    delete ctx;
                    return;
                }
            }

            std::cout << "[SERVER] AcceptEx completed with " << bytesTransferred
                << " bytes of initial data.\n";
            // Possibly handle the data in ctx->buffer[0..bytesTransferred-1]
            // Then post a WSARecv:
            ctx->operation = OP_RECV;
            ZeroMemory(&ctx->overlapped, sizeof(OVERLAPPED));
            ctx->wsabuf.buf = ctx->buffer;
            ctx->wsabuf.len = sizeof(ctx->buffer);

            DWORD flags = 0;
            int ret = WSARecv(ctx->socket, &ctx->wsabuf, 1, NULL, &flags,
                &ctx->overlapped, NULL);
            if (ret == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING)
            {
                std::cerr << "[SERVER] WSARecv failed after AcceptEx (>0 bytes): "
                    << WSAGetLastError() << "\n";
                closesocket(ctx->socket);
                delete ctx;
            }
        }
        break;

        case OP_RECV:
            // (B) We got data from the client. Process it.
        {
            std::string msg(ctx->buffer, bytesTransferred);
            std::cout << "[SERVER] Received from client: " << msg << "\n";
            // Maybe echo back or do something else.

            // Then post another read if you want continuous reading:
            ctx->operation = OP_RECV; // stay in RECV mode
            ZeroMemory(&ctx->overlapped, sizeof(OVERLAPPED));
            ctx->wsabuf.buf = ctx->buffer;
            ctx->wsabuf.len = sizeof(ctx->buffer);

            DWORD flags = 0;
            int ret = WSARecv(ctx->socket, &ctx->wsabuf, 1, NULL, &flags,
                &ctx->overlapped, NULL);
            if (ret == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING)
            {
                std::cerr << "[SERVER] WSARecv failed: " << WSAGetLastError() << "\n";
                closesocket(ctx->socket);
                delete ctx;
            }
        }
        break;

        case OP_SEND:
            // (C) We finished sending some data to the client
            // If you have more data to send, you can queue another WSASend.
            // Otherwise, do nothing or free the context if it's a one-time send.
            std::cout << "[SERVER] Finished sending " << bytesTransferred << " bytes.\n";
            // Free or reuse. For example:
            //delete ctx; // if this was a one-off context
            break;
        }
    }
}

