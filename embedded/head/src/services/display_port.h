#pragma once
// Display port (services-owned): narrow rail contract for the OLED face panel.
//
// PowerManager drives the display rail through this interface — never the
// ability class. DisplayRenderer (abilities/display) implements it. Covers exactly
// the display-rail verbs PowerManager needs (present/setDim/repaint) plus the
// IActuator power edge.

#include "common/iactuator.h"

class IDisplayPort : public IActuator {
 public:
  ~IDisplayPort() override = default;
  virtual bool present() const = 0;
  virtual void setDim(bool dim) = 0;
  virtual void repaint() = 0;
};
