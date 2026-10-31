package com.desky.shared.core

import com.desky.shared.core.UdpSocketManager
import kotlinx.coroutines.CoroutineScope

/**
 * Platform UDP socket manager factory.
 *
 * Each platform actual MUST honor this contract:
 * - the constructor binds an ephemeral-port UDP socket eagerly (so the local
 *   port is fixed before the first `send`);
 * - the receive loop is launched in `init` into [scope] on an IO dispatcher,
 *   feeding a `MutableSharedFlow` with `replay = 0` and
 *   `extraBufferCapacity = 64`;
 * - [UdpSocketManager.send] transmits [ByteArray] to `host:port`;
 * - [UdpSocketManager.close] cancels the loop and closes the socket.
 *
 * Single-collector assumption: [UdpSocketManager.incoming] is a hot
 * [kotlinx.coroutines.flow.SharedFlow] with no replay. Multiple collectors
 * each receive every datagram (fan-out), so at most one consumer should
 * collect it. Delivery follows documented SharedFlow semantics: with no
 * collector subscribed, arriving datagrams are dropped (replay 0 retains
 * nothing, so subscribe promptly after creating the manager); with a slow
 * subscribed collector, bursts beyond the 64-slot buffer suspend the receive
 * loop until the collector drains.
 */
expect fun createUdpSocketManager(scope: CoroutineScope): UdpSocketManager
