// modbus_master.h - tiny Modbus-RTU master over UART.
// Mirrors the bits of Arduino's ModbusMaster we use:
//   begin(), readHoldingRegisters(), getResponseBuffer().
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "driver/uart.h"

// Status codes, same spirit as ModbusMaster's ku8MB* constants.
#define MBM_SUCCESS            0x00  // like ku8MBSuccess
#define MBM_ERR_TIMEOUT        0xE0  // no / short reply
#define MBM_ERR_CRC            0xE1  // CRC mismatch
#define MBM_ERR_BAD_FRAME      0xE2  // wrong addr / function / length
#define MBM_ERR_EXCEPTION      0xE3  // slave returned a Modbus exception

typedef struct {
    uart_port_t uart;
    uint8_t     slave;
    uint32_t    timeout_ms;
    uint16_t    response[8];   // last read registers, like getResponseBuffer()
} modbus_master_t;

// Bind a master to an already-configured UART port and slave address.
void mbm_begin(modbus_master_t *m, uart_port_t uart, uint8_t slave, uint32_t timeout_ms);

// Read `qty` holding registers starting at `addr` (function 0x03).
// On MBM_SUCCESS the values land in m->response[0..qty-1].
uint8_t mbm_read_holding_registers(modbus_master_t *m, uint16_t addr, uint16_t qty);

// Convenience accessor, like ModbusMaster::getResponseBuffer(i).
uint16_t mbm_get_response(const modbus_master_t *m, uint8_t index);
