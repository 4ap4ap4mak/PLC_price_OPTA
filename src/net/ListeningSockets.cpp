#include "ListeningSockets.h"

void CheckedWiFiServer::end() {
    if (sock) {
        sock->close();
        delete sock;
        sock = nullptr;
    }
}

nsapi_error_t CheckedWiFiServer::beginOnAddress(const IPAddress& ipAddress) {
    end();
    sock = new TCPSocket();
    if (sock == nullptr) {
        return NSAPI_ERROR_NO_MEMORY;
    }

    nsapi_error_t result = sock->open(WiFi.getNetwork());
    if (result != NSAPI_ERROR_OK) {
        end();
        return result;
    }

    int reuseAddress = 1;
    sock->setsockopt(NSAPI_SOCKET, NSAPI_REUSEADDR,
        &reuseAddress, sizeof(reuseAddress));
    String addressText = ipAddress.toString();
    SocketAddress address(addressText.c_str(), _port);
    result = sock->bind(address);
    if (result == NSAPI_ERROR_OK) {
        result = sock->listen(5);
    }
    if (result == NSAPI_ERROR_OK) {
        sock->set_blocking(false);
    } else {
        end();
    }
    return result;
}

void CheckedEthernetServer::end() {
    if (sock) {
        sock->close();
        delete sock;
        sock = nullptr;
    }
}

nsapi_error_t CheckedEthernetServer::beginListening() {
    end();
    sock = new TCPSocket();
    if (sock == nullptr) {
        errorStage = "allocate";
        return NSAPI_ERROR_NO_MEMORY;
    }

    nsapi_error_t result = sock->open(Ethernet.getNetwork());
    if (result != NSAPI_ERROR_OK) {
        errorStage = "open";
        end();
        return result;
    }

    int reuseAddress = 1;
    sock->setsockopt(NSAPI_SOCKET, NSAPI_REUSEADDR,
        &reuseAddress, sizeof(reuseAddress));
    errorStage = "bind";
    result = sock->bind(_port);
    if (result == NSAPI_ERROR_OK) {
        errorStage = "listen";
        result = sock->listen(5);
    }
    if (result == NSAPI_ERROR_OK) {
        sock->set_blocking(false);
    } else {
        end();
    }
    return result;
}
