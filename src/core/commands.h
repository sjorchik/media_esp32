#pragma once
#include <stdint.h>

#include "proto/proto_table.h"

enum class Origin : uint8_t { Link, Console };

// Команда після валідації роутером. arg: SrcId (SRC), 0/1 (SHUF), 0..2 (REP),
// бітова маска sd|usb<<1 (DbgMedia).
struct Cmd {
  proto::CmdId id;
  int16_t arg;
  Origin origin;
};