#pragma once
// Shared event-bus mechanics — header-only, direct fan-out pub/sub.
// No broker task: publish() fan-outs into each subscriber's private queue,
// so one slow consumer fills only its own and never blocks publishers.
// Never publish() from ISR (it logs); subscribe/unsubscribe are task-context only.

#include <Arduino.h>

#include "common/logger.h"

template <typename EventT, typename TypeT, const char* (*NameFn)(TypeT)>
class EventBusT {
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

    bool receive(EventT& out, uint32_t timeoutMs = kWaitForever) {
      if (queue_ == nullptr) {
        return false;
      }
      TickType_t ticks = (timeoutMs == kWaitForever) ? portMAX_DELAY : pdMS_TO_TICKS(timeoutMs);
      return xQueueReceive(queue_, &out, ticks) == pdPASS;
    }

   private:
    friend class EventBusT;
    QueueHandle_t queue_;
    uint8_t slot_;
  };

  static void begin() { ensureMutex(); }

  // Returns an invalid Subscription when the table is full or alloc fails.
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
      QueueHandle_t q = xQueueCreate(depth, sizeof(EventT));
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

  // Fan-out copy; never blocks; full queues drop + WARN. Task context only, never ISR.
  static void publish(TypeT type, uint32_t payload = 0) {
    EventT ev;
    ev.type = type;
    ev.payload = payload;
    ev.timestampMs = millis();

    // Snapshot under the mutex, send without holding it.
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

    LOG_D("BUS", "pub %s payload=%lu subs=%u", NameFn(type), (unsigned long)payload, (unsigned)n);
    for (uint8_t i = 0; i < n; ++i) {
      if (xQueueSend(targets[i].queue, &ev, 0) != pdPASS) {
        uint32_t total = __atomic_add_fetch(&s_dropped[targets[i].slot], 1, __ATOMIC_RELAXED);
        LOG_W("BUS", "drop %s sub=%u dropped=%lu", NameFn(type), (unsigned)targets[i].slot, (unsigned long)total);
      }
    }
  }

  // ISR-safe fan-out: FromISR calls, no mutex, no logging.
  static void publishFromISR(TypeT type, uint32_t payload = 0, BaseType_t* woken = nullptr) {
    EventT ev;
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

 private:
  static void ensureMutex() {
    if (s_mutex == nullptr) {
#if defined(portTICK_PERIOD_MS)
      SemaphoreHandle_t m = xSemaphoreCreateMutex();
      if (m != nullptr) {
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

  static inline QueueHandle_t s_queues[kMaxSubscribers] = {nullptr};
  static inline uint32_t s_dropped[kMaxSubscribers] = {0};
  static inline SemaphoreHandle_t s_mutex = nullptr;
};
