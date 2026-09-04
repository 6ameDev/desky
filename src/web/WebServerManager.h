#pragma once
#include <ESPAsyncWebServer.h>
#include "core/RobotState.h"

class WebServerManager {
public:
    WebServerManager(RobotStateStore& stateStore);
    void begin();
    void pushTelemetry();
    void cleanupClients();

private:
    AsyncWebServer _server;
    AsyncWebSocket _ws;
    RobotStateStore& _stateStore;

    void onEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len);
    void handleBinaryMessage(void *arg, uint8_t *data, size_t len);
    void sendConfig(AsyncWebSocketClient *client = nullptr);
};
