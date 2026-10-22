#pragma once
// Bond-persistence receipt vocabulary (Arduino-free, host-tested).
//
// Proof that the bond hit NVS before the ACK. Move-only; default-constructs
// INVALID; only ConfigStore (friend) can mark one valid via saveBond /
// saveRekey. Neutral home: transport/connection_manager.h (the funnel that
// consumes it) and services/config_store.h (the minter) both include this
// header, so neither layer names the other.

// Firmware minter of PairingReceipt (services/config_store.h). Forward
// declaration only — this header stays dependency-free.
class ConfigStore;

namespace connection {

// PairingReceipt: proof that the bond hit NVS before the ACK. Move-only.
// Default-constructs INVALID; only ConfigStore (friend) can mark one valid.
class PairingReceipt {
 public:
  PairingReceipt() : valid_(false) {}
  PairingReceipt(PairingReceipt&& other) : valid_(other.valid_) { other.valid_ = false; }
  PairingReceipt& operator=(PairingReceipt&& other) {
    if (this != &other) {
      valid_ = other.valid_;
      other.valid_ = false;
    }
    return *this;
  }
  PairingReceipt(const PairingReceipt&) = delete;
  PairingReceipt& operator=(const PairingReceipt&) = delete;

  bool valid() const { return valid_; }

 private:
  // NOTE: the global qualifier matters — an unqualified `friend class
  // ConfigStore` would befriend a never-defined connection::ConfigStore.
  friend class ::ConfigStore;
  void mark(bool valid) { valid_ = valid; }

  bool valid_;
};

}  // namespace connection
