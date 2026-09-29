#pragma once
// desky-head event bus — thin alias over the shared generic mechanism.
// The fan-out implementation lives once in shared/common/event_bus.h
// (EventBusT, never forked); this header only binds it to head's event
// types (include/head_context.h). Mirrors core's src/services/event_bus.h.

#include "common/event_bus.h"
#include "head_context.h"

using EventBus = EventBusT<HeadEvent, HeadEventType, headEventName>;
