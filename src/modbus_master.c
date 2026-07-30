#include "modbus_master.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static uint16_t crc16_modbus(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : (crc >> 1);
        }
    }
    return crc;
}

void mbm_begin(modbus_master_t *m, uart_port_t uart, uint8_t slave, uint32_t timeout_ms)
{
    m->uart = uart;
    m->slave = slave;
    m->timeout_ms = timeout_ms;
    memset(m->response, 0, sizeof(m->response));
}

uint8_t mbm_read_holding_registers(modbus_master_t *m, uint16_t addr, uint16_t qty)
{
    if (qty == 0 || qty > 8) return MBM_ERR_BAD_FRAME;

    // Build request: [slave][0x03][addr_hi][addr_lo][qty_hi][qty_lo][crc_lo][crc_hi]
    uint8_t req[8];
    req[0] = m->slave;
    req[1] = 0x03;
    req[2] = addr >> 8;
    req[3] = addr & 0xFF;
    req[4] = qty >> 8;
    req[5] = qty & 0xFF;
    uint16_t crc = crc16_modbus(req, 6);
    req[6] = crc & 0xFF;
    req[7] = crc >> 8;

    uart_flush_input(m->uart);
    uart_write_bytes(m->uart, (const char *)req, sizeof(req));
    uart_wait_tx_done(m->uart, pdMS_TO_TICKS(100));

    // Expected reply: [slave][0x03][byte_count][data...][crc_lo][crc_hi]
    int expected = 3 + (qty * 2) + 2;
    uint8_t resp[3 + 16 + 2];
    int n = uart_read_bytes(m->uart, resp, expected, pdMS_TO_TICKS(m->timeout_ms));
    if (n < 0) n = 0;

    // Modbus exception reply is 5 bytes: [slave][0x83][code][crc_lo][crc_hi]
    if (n >= 2 && (resp[1] & 0x80)) return MBM_ERR_EXCEPTION;
    if (n != expected) return MBM_ERR_TIMEOUT;
    if (resp[0] != m->slave || resp[1] != 0x03 || resp[2] != qty * 2)
        return MBM_ERR_BAD_FRAME;

    uint16_t crc_calc = crc16_modbus(resp, 3 + qty * 2);
    uint16_t crc_recv = resp[3 + qty * 2] | (resp[4 + qty * 2] << 8);
    if (crc_calc != crc_recv) return MBM_ERR_CRC;

    for (uint16_t i = 0; i < qty; i++) {
        m->response[i] = (resp[3 + i * 2] << 8) | resp[4 + i * 2];
    }
    return MBM_SUCCESS;
}

uint16_t mbm_get_response(const modbus_master_t *m, uint8_t index)
{
    return (index < 8) ? m->response[index] : 0;
}
