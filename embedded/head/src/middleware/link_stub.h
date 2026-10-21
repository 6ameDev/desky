#pragma once
// desky-head link stub — MIDDLEWARE compatibility shim (Task 2 split).
//
// The UART2 stub (class LinkStub) + line mapping (namespace linkstub) now
// live in transport/link_stub.h as a dumb pipe (business decisions arrive
// there as an injected onCommand callback bound in main.cpp). This shim
// re-exports that transport header so existing host tests including this
// path keep passing with zero test changes.

#include "transport/link_stub.h"
