#pragma once
// desky v2 event bus — header-only, direct fan-out pub/sub.
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
// Rules: no String/heap in the publish/receive path (queues are created
// once in subscribe()), never call publish() (the logging variant) from
// ISR, subscribe()/unsubscribe() are task-context only and must have
// settled before any ISR publishing starts (the ISR path takes no mutex
// and snapshots the slot array as-is).

#include <Arduino.h>

#include "services/logger.h"
#include "system_context.h"

class EventBus {
 public:
  static constexpr uint8_t kMaxSubscribers = 8;
  static constexpr UBaseType_t kDefaultDepth = 16;
  static constexpr uint8_t kInvalidSlot = 0xFF;
  static constexpr uint32_t kWaitForever = 0xFFFFFFFFUL;

  // Handle to one subscriber's private queue. Copyable; copies alias the
  // same queue slot (droppedCount() stays coherent across copies).
  class Subscription {
   public:
    Subscription() : queue_(nullptr), slot_(kInvalidSlot) {}

    bool valid() const { return queue_ != nullptr; }

    // Blocking take. Returns true with `out` filled on success, false on
    // timeout (or if the subscription is invalid). timeoutMs defaults to
    // forever; pdMS_TO_TICKS overflow is avoided by special-casing it.
    bool receive(SystemEvent& out, uint32_t timeoutMs = kWaitForever) {
      if (queue_ == nullptr) {
        return false;
      }
      TickType_t ticks = (timeoutMs == kWaitForever) ? portMAX_DELAY : pdMS_TO_TICKS(timeoutMs);
      return xQueueReceive(queue_, &out, ticks) == pdPASS;
    }

    // Lifetime drops for this slot (queue full at publish time). Monotonic
    // per slot; reset only by unsubscribe() + re-subscribe.
    uint32_t droppedCount() const {
      if (slot_ >= kMaxSubscribers) {
        return 0;
      }
      return __atomic_load_n(&s_dropped[slot_], __ATOMIC_RELAXED);
    }

   private:
    friend class EventBus;
    QueueHandle_t queue_;
    uint8_t slot_;
  };

  // Idempotent; creates the subscriber-list mutex on first use.
  static void begin() { ensureMutex(); }

  // Registers a private queue of `depth` events. Returns an invalid
  // Subscription when the table is full (kMaxSubscribers) or the queue
  // allocation fails. Task context only.
  static Subscription subscribe(UBaseType_t depth = kDefaultDepth) {
    ensureMutex();
    if (depth == 0) {
      depth = kDefaultDepth;
    }
    Subscription sub;
    lock();
    for (uint8_t i = 0; i < kMaxSubscribers; ++i) {
      if (s_queues[i] != nullptr) {
        continue;
      }
      QueueHandle_t q = xQueueCreate(depth, sizeof(SystemEvent));
      if (q != nullptr) {
        s_queues[i] = q;
        s_dropped[i] = 0;
        sub.queue_ = q;
        sub.slot_ = i;
      }
      break;
    }
    unlock();
    return sub;
  }

  static void unsubscribe(Subscription* sub) {
    if (sub == nullptr || !sub->valid()) {
      return;
    }
    ensureMutex();
    lock();
    uint8_t s = sub->slot_;
    if (s < kMaxSubscribers && s_queues[s] == sub->queue_) {
      vQueueDelete(s_queues[s]);
      s_queues[s] = nullptr;
      s_dropped[s] = 0;
    }
    unlock();
    sub->queue_ = nullptr;
    sub->slot_ = kInvalidSlot;
  }

  // Fan-out copy to every subscriber. Stamps millis(), logs at DEBUG,
  // never blocks (zero-timeout sends). Full queues drop + count + WARN.
  // Task context only — never call from ISR (logging takes a mutex).
  static void publish(EventType type, uint32_t payload = 0) {
    SystemEvent ev;
    ev.type = type;
    ev.payload = payload;
    ev.timestampMs = millis();

    // Snapshot handles under the mutex, then send without holding it so a
    // slow consumer (or LOG_W below) cannot stall other publishers.
    struct Target {
      QueueHandle_t queue;
      uint8_t slot;
    };
    Target targets[kMaxSubscribers];
    uint8_t n = 0;
    lock();
    for (uint8_t i = 0; i < kMaxSubscribers; ++i) {
      if (s_queues[i] != nullptr) {
        targets[n].queue = s_queues[i];
        targets[n].slot = i;
        ++n;
      }
    }
    unlock();

    LOG_D("BUS", "pub %s payload=%lu subs=%u", eventName(type), (unsigned long)payload, (unsigned)n);
    for (uint8_t i = 0; i < n; ++i) {
      if (xQueueSend(targets[i].queue, &ev, 0) != pdPASS) {
        uint32_t total = __atomic_add_fetch(&s_dropped[targets[i].slot], 1, __ATOMIC_RELAXED);
        LOG_W("BUS", "drop %s sub=%u dropped=%lu", eventName(type), (unsigned)targets[i].slot, (unsigned long)total);
      }
    }
  }

  // ISR-safe fan-out: FromISR queue calls, no mutex, NO logging (the log
  // path takes a mutex and vsnprintf stack — both forbidden in ISR).
  // Passes `woken` through to xQueueSendFromISR; when omitted (nullptr) and
  // a higher-priority task was woken, yields here. Timestamp uses the ISR
  // tick count, not millis().
  static void publishFromISR(EventType type, uint32_t payload = 0, BaseType_t* woken = nullptr) {
    SystemEvent ev;
    ev.type = type;
    ev.payload = payload;
    ev.timestampMs = (uint32_t)(xTaskGetTickCountFromISR() * (TickType_t)portTICK_PERIOD_MS);

    BaseType_t localWoken = pdFALSE;
    BaseType_t* pw = (woken != nullptr) ? woken : &localWoken;
    for (uint8_t i = 0; i < kMaxSubscribers; ++i) {
      QueueHandle_t q = s_queues[i];
      if (q == nullptr) {
        continue;
      }
      if (xQueueSendFromISR(q, &ev, pw) != pdPASS) {
        __atomic_add_fetch(&s_dropped[i], 1, __ATOMIC_RELAXED);
      }
    }
    if (woken == nullptr && localWoken != pdFALSE) {
      portYIELD_FROM_ISR();
    }
  }

  static const char* eventName(EventType type) {
    switch (type) {
      case EVENT_CLIFF_DETECTED:
        return "CLIFF_DETECTED";
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
      default:
        return "UNKNOWN";
    }
  }

 private:
  static void ensureMutex() {
    if (s_mutex == nullptr) {
#if defined(portTICK_PERIOD_MS)
      // Safe to call repeatedly; first writer wins.
      SemaphoreHandle_t m = xSemaphoreCreateMutex();
      if (m != nullptr) {
        // Avoid clobbering if two tasks race here.
        if (s_mutex == nullptr) {
          s_mutex = m;
        } else {
          vSemaphoreDelete(m);
        }
      }
#endif
    }
  }

  static void lock() {
    if (s_mutex != nullptr) {
      xSemaphoreTake(s_mutex, portMAX_DELAY);
    }
  }

  static void unlock() {
    if (s_mutex != nullptr) {
      xSemaphoreGive(s_mutex);
    }
  }

  static QueueHandle_t s_queues[kMaxSubscribers];
  static uint32_t s_dropped[kMaxSubscribers];
  static SemaphoreHandle_t s_mutex;
};

inline QueueHandle_t EventBus::s_queues[EventBus::kMaxSubscribers] = {nullptr};
inline uint32_t EventBus::s_dropped[EventBus::kMaxSubscribers] = {0};
inline SemaphoreHandle_t EventBus::s_mutex = nullptr;
