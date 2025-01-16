#include "AsyncIOCPClient.h"

// ---------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------
AsyncIOCPClient::AsyncIOCPClient(const std::string& host, int port)
    : m_host(host),
    m_port(port),
    m_socket(INVALID_SOCKET),
    m_hIOCP(NULL),
    m_hWorkerThread(NULL),
    m_running(false),
    m_ioContext(nullptr)
{
    ZeroMemory(&m_serverAddr, sizeof(m_serverAddr));
    m_serverAddr.sin_family = AF_INET;
    m_serverAddr.sin_port = htons(static_cast<u_short>(m_port));
    // We'll fill in the sin_addr later in Connect() or immediately below
}

AsyncIOCPClient::~AsyncIOCPClient()
{
    Stop(); // Ensure cleanup if user forgets
}

// ---------------------------------------------------------
// Public Methods
// ---------------------------------------------------------
bool AsyncIOCPClient::Start()
{
    if (!initWinsock()) {
        std::cerr << "[CLIENT] Winsock initialization failed.\n";
        return false;
    }
    if (!createSocket()) {
        std::cerr << "[CLIENT] Socket creation failed.\n";
        return false;
    }
    if (!createIOCP()) {
        std::cerr << "[CLIENT] IOCP creation failed.\n";
        return false;
    }

    // Create a worker thread that handles all I/O completions
    m_running = true;
    m_hWorkerThread = CreateThread(nullptr, 0, workerThread, this, 0, nullptr);
    if (!m_hWorkerThread) {
        std::cerr << "[CLIENT] CreateThread failed: " << GetLastError() << "\n";
        return false;
    }

    return true;
}

bool AsyncIOCPClient::Connect()
{
    // Convert m_host (std::string) to std::wstring
    std::wstring wHost(m_host.begin(), m_host.end());

    // Convert the string host to a numeric address (InetPton recommended).
    // Alternatively, use getaddrinfo to handle DNS or IPv6.
    if (InetPton(AF_INET, wHost.c_str(), &m_serverAddr.sin_addr) <= 0) {
        std::cerr << "[CLIENT] Invalid host or InetPton failed.\n";
        return false;
    }

    // We can connect with the standard blocking `connect()` call,
    // OR do an overlapped connect using `ConnectEx`.
    // We'll do ConnectEx for demonstration of purely async operation.
    LPFN_CONNECTEX lpfnConnectEx = nullptr;
    if (!getConnectExPtr(m_socket, lpfnConnectEx)) {
        std::cerr << "[CLIENT] Failed to retrieve ConnectEx pointer.\n";
        return false;
    }

    // Bind the socket to any local IP/port, required before using ConnectEx.
    if (!bindAnyAddress(m_socket)) {
        std::cerr << "[CLIENT] bindAnyAddress() failed.\n";
        return false;
    }

    // Associate the socket with the IOCP
    if (!associateSocketToIOCP(m_socket)) {
        std::cerr << "[CLIENT] Failed to associate socket with IOCP.\n";
        return false;
    }

    // Create an IO context for this connection
    m_ioContext = new CLIENT_IO_CONTEXT{};
    m_ioContext->wsabuf.buf = m_ioContext->buffer;
    m_ioContext->wsabuf.len = sizeof(m_ioContext->buffer);

    // Use ConnectEx in an overlapped manner
    ZeroMemory(&m_ioContext->overlapped, sizeof(OVERLAPPED));
    BOOL bRet = lpfnConnectEx(
        m_socket,
        reinterpret_cast<sockaddr*>(&m_serverAddr),
        sizeof(m_serverAddr),
        nullptr,
        0,
        nullptr,
        &m_ioContext->overlapped
    );
    if (!bRet) {
        int err = WSAGetLastError();
        if (err != ERROR_IO_PENDING) {
            std::cerr << "[CLIENT] ConnectEx failed: " << err << "\n";
            return false;
        }
        // If it's ERROR_IO_PENDING, the connection is in progress
    }

    // The actual completion (success/failure) will be signaled in the workerThread via IOCP.
    // Once connected, we can post a first WSARecv (done in handleIO).

    std::cout << "[CLIENT] Connection initiated (async) to "
        << m_host << ":" << m_port << "\n";
    return true;
}

bool AsyncIOCPClient::Send(const std::string& data)
{
    if (!m_running || m_socket == INVALID_SOCKET) {
        std::cerr << "[CLIENT] Socket not connected or client not started.\n";
        return false;
    }

    // Allocate a fresh IO context for sending
    CLIENT_IO_CONTEXT* sendCtx = new CLIENT_IO_CONTEXT{};
    ZeroMemory(&sendCtx->overlapped, sizeof(OVERLAPPED));

    // Copy data into the buffer
    size_t len = data.size();
    if (len > sizeof(sendCtx->buffer)) {
        std::cerr << "[CLIENT] Data too large for buffer.\n";
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
    if (ret == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
        std::cerr << "[CLIENT] WSASend failed: " << WSAGetLastError() << "\n";
        closesocket(m_socket);
        delete sendCtx;
        return false;
    }

    // The completion of this send will be picked up by the worker thread via IOCP.
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
    if (m_hWorkerThread) {
        WaitForSingleObject(m_hWorkerThread, INFINITE);
        CloseHandle(m_hWorkerThread);
        m_hWorkerThread = NULL;
    }

    // Close socket
    if (m_socket != INVALID_SOCKET) {
        closesocket(m_socket);
        m_socket = INVALID_SOCKET;
    }

    // Cleanup the main IO context if allocated
    if (m_ioContext) {
        delete m_ioContext;
        m_ioContext = nullptr;
    }

    // Cleanup Winsock
    WSACleanup();

    std::cout << "[CLIENT] Stopped.\n";
}

// ---------------------------------------------------------
// Private Helpers
// ---------------------------------------------------------
bool AsyncIOCPClient::initWinsock()
{
    WSADATA wsaData;
    int ret = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (ret != 0) {
        std::cerr << "[CLIENT] WSAStartup failed: " << ret << "\n";
        return false;
    }
    return true;
}

bool AsyncIOCPClient::createSocket()
{
    m_socket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
    if (m_socket == INVALID_SOCKET) {
        std::cerr << "[CLIENT] WSASocket failed: " << WSAGetLastError() << "\n";
        return false;
    }
    return true;
}

bool AsyncIOCPClient::createIOCP()
{
    m_hIOCP = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
    if (m_hIOCP == nullptr) {
        std::cerr << "[CLIENT] CreateIoCompletionPort failed: " << GetLastError() << "\n";
        return false;
    }
    return true;
}

bool AsyncIOCPClient::associateSocketToIOCP(SOCKET s, ULONG_PTR completionKey)
{
    HANDLE h = CreateIoCompletionPort(reinterpret_cast<HANDLE>(s), m_hIOCP, completionKey, 1);
    if (h == nullptr) {
        std::cerr << "[CLIENT] associateSocketToIOCP failed: " << GetLastError() << "\n";
        return false;
    }
    return true;
}

bool AsyncIOCPClient::bindAnyAddress(SOCKET s)
{
    // For ConnectEx, the socket must be bound (can be ephemeral port).
    sockaddr_in anyAddr{};
    anyAddr.sin_family = AF_INET;
    anyAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    anyAddr.sin_port = 0; // let OS choose a random port
    if (bind(s, reinterpret_cast<sockaddr*>(&anyAddr), sizeof(anyAddr)) == SOCKET_ERROR) {
        std::cerr << "[CLIENT] bind failed: " << WSAGetLastError() << "\n";
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
    if (result == SOCKET_ERROR) {
        std::cerr << "[CLIENT] WSAIoctl for ConnectEx failed: " << WSAGetLastError() << "\n";
        return false;
    }
    return true;
}

bool AsyncIOCPClient::postRecv(CLIENT_IO_CONTEXT* ctx)
{
    ZeroMemory(&ctx->overlapped, sizeof(OVERLAPPED));
    ctx->wsabuf.buf = ctx->buffer;
    ctx->wsabuf.len = sizeof(ctx->buffer);

    DWORD flags = 0;
    DWORD bytesRecv = 0;
    int ret = WSARecv(m_socket, &ctx->wsabuf, 1, &bytesRecv, &flags, &ctx->overlapped, nullptr);
    if (ret == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
        std::cerr << "[CLIENT] WSARecv failed: " << WSAGetLastError() << "\n";
        return false;
    }
    return true;
}

// ---------------------------------------------------------
// Static Worker Thread Entry
// ---------------------------------------------------------
DWORD WINAPI AsyncIOCPClient::workerThread(LPVOID lpParam)
{
    AsyncIOCPClient* client = reinterpret_cast<AsyncIOCPClient*>(lpParam);
    DWORD bytesTransferred = 0;
    ULONG_PTR completionKey = 0;
    OVERLAPPED* pOverlapped = nullptr;

    while (true) {
        BOOL success = GetQueuedCompletionStatus(
            client->m_hIOCP,
            &bytesTransferred,
            &completionKey,
            &pOverlapped,
            INFINITE
        );
        if (!success && pOverlapped == nullptr) {
            // IOCP closed or serious error
            // This likely means the client is shutting down
            break;
        }
        if (!pOverlapped) {
            break;
        }

        CLIENT_IO_CONTEXT* ioCtx = reinterpret_cast<CLIENT_IO_CONTEXT*>(pOverlapped);

        if (bytesTransferred == 0) {
            // This usually means the remote side closed the connection
            std::cerr << "[CLIENT] Connection closed by server or zero-byte I/O.\n";
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

// ---------------------------------------------------------
// Handle Completed I/O
// ---------------------------------------------------------
void AsyncIOCPClient::handleIO(DWORD bytesTransferred, CLIENT_IO_CONTEXT* ioCtx)
{
    // We need to figure out if this is the result of a ConnectEx, a Send, or a Recv
    // In a production design, you'd keep track of the "operation type" in the context.
    // For simplicity, let's do a small logic test:

    // 1) If we haven't posted a recv yet, let's assume we've just connected
    //    so we can post the first recv now.
    if (ioCtx == m_ioContext && m_ioContext->buffer[0] == '\0') {
        // This might be the completion of ConnectEx if no data was in the buffer.
        std::cout << "[CLIENT] Connection established!\n";

        int result = setsockopt(m_socket, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, NULL, 0);
        if (result == SOCKET_ERROR) {
            std::cerr << "[CLIENT] setsockopt(SO_UPDATE_CONNECT_CONTEXT) failed: "
                << WSAGetLastError() << "\n";
        }
        else {
            std::cout << "[CLIENT] setsockopt(SO_UPDATE_CONNECT_CONTEXT) succeeded.\n";
        }

        // Post a read so we can receive data from the server
        if (!postRecv(ioCtx)) {
            std::cerr << "[CLIENT] postRecv failed after connect.\n";
        }
        return;
    }

    // 2) Otherwise, if we have data in buffer, interpret it as received data
    //    and maybe post another recv.
    // For demonstration, let's just print it:
    if (bytesTransferred > 0) {
        std::string received(ioCtx->buffer, bytesTransferred);
        std::cout << "[CLIENT] Received: " << received << "\n";
    }

    // Post another recv for continuous reading
    if (!postRecv(ioCtx)) {
        std::cerr << "[CLIENT] postRecv failed.\n";
    }
}