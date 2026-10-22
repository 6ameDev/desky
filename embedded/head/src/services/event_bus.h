#pragma once
// Thin alias binding the shared event bus to head's event types.

#include "common/event_bus.h"
#include "head_context.h"

using EventBus = EventBusT<HeadEvent, HeadEventType, headEventName>;
