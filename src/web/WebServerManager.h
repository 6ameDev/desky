#pragma once
#include <ESPAsyncWebServer.h>
#include "core/RobotState.h"

class WebServerManager {
public:
    WebServerManager(RobotStateStore& stateStore);
    void begin();
    void pushTelemetry();
    void pushTelemetryIfNeeded();
    void cleanupClients();

private:
    AsyncWebServer _server;
    AsyncWebSocket _ws;
    RobotStateStore& _stateStore;

    unsigned long _lastPushMs = 0;
    bool _lastSentCliff = false;
    bool _lastSentFault = false;
    bool _lastSentEBrake = false;
    String _lastSentStatus = "";
    int _lastSentDistance = 0;

    void onEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len);
    void handleBinaryMessage(void *arg, uint8_t *data, size_t len);
    void sendConfig(AsyncWebSocketClient *client = nullptr);
};
