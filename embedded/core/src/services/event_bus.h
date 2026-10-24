#pragma once
// Event bus — thin alias over the shared generic mechanism. The fan-out
// implementation lives once in shared/common/event_bus.h; this header only
// binds it to core's event types (system_context.h).
//
// Topology: NO broker task. publish() copies the event into every
// subscriber's private FreeRTOS queue, so one slow consumer only fills its
// own queue and never blocks publishers or other subscribers.
//
// Usage:
//   EventBus::begin();                                  // setup(), after Logger::begin()
//   EventBus::Subscription sub = EventBus::subscribe();  // setup-time; NOT ISR-safe
//   EventBus::publish(EVENT_USER_TOUCH, 1);             // task context (logs)
//   EventBus::publishFromISR(type, payload);            // ISR context (never logs)
//   SystemEvent ev;                                     // consumer task:
//   while (sub.receive(ev)) { ... }
//
// Rules: no String/heap in the publish/receive path, never call publish()
// (the logging variant) from ISR, subscribe()/unsubscribe() are task-context
// only and must have settled before any ISR publishing starts.

#include "common/event_bus.h"
#include "system_context.h"

inline const char* deskyEventName(EventType type) {
  switch (type) {
    case EVENT_CLIFF_DETECTED:
      return "CLIFF_DETECTED";
    case EVENT_GROUND_CHANGED:
      return "GROUND_CHANGED";
    case EVENT_PICKED_UP:
      return "PICKED_UP";
    case EVENT_UDP_COMMAND_RECEIVED:
      return "UDP_COMMAND_RECEIVED";
    case EVENT_BATTERY_LOW:
      return "BATTERY_LOW";
    case EVENT_USER_TOUCH:
      return "USER_TOUCH";
    case EVENT_FACE_RECOGNIZED:
      return "FACE_RECOGNIZED";
    case EVENT_BEHAVIOR_STARTED:
      return "BEHAVIOR_STARTED";
    case EVENT_BEHAVIOR_DONE:
      return "BEHAVIOR_DONE";
    case EVENT_OBSTACLE_DETECTED:
      return "OBSTACLE_DETECTED";
    case EVENT_OBSTACLE_CLEARED:
      return "OBSTACLE_CLEARED";
    case EVENT_BEEP_DONE:
      return "BEEP_DONE";
    default:
      return "UNKNOWN";
  }
}

using EventBus = EventBusT<SystemEvent, EventType, deskyEventName>;
