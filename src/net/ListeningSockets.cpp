#include "ListeningSockets.h"

void CheckedWiFiServer::end() {
    if (sock) {
        sock->close();
        delete sock;
        sock = nullptr;
    }
}

nsapi_error_t CheckedWiFiServer::beginAnyAddress(NetworkInterface* network) {
    end();
    sock = new TCPSocket();
    if (sock == nullptr) {
        errorStage = "allocate";
        return NSAPI_ERROR_NO_MEMORY;
    }

    nsapi_error_t result = sock->open(network);
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
