#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>    // For AcceptEx, GetAcceptExSockaddrs, etc.
#include <iostream>
#include <string>
#include <vector>
#include "ConsoleLogger.h"

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Mswsock.lib")
