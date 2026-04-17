/*
 * MCP2515 standalone CAN controller driver (SPI).
 *
 * The MCP2515 is a Microchip CAN 2.0A/B controller with an SPI interface.
 * It contains three TX buffers and two RX buffers, programmable acceptance
 * filters, and supports bit rates from 10 kbps to 1 Mbps depending on the
 * oscillator frequency.
 *
 * SPI mode: CPOL=0, CPHA=0 (mode 0) or CPOL=1, CPHA=1 (mode 3).
 * Maximum SPI clock: 10 MHz.
 *
 * Usage:
 *   Mcp2515 can(spi_device_cap, Mcp2515::Osc_freq::MHz8);
 *   can.init(Mcp2515::Bit_rate::kbps_500);
 *   Mcp2515::Can_frame frame = { .id=0x123, .dlc=2, .data={0xDE,0xAD} };
 *   can.send(frame);
 *   if (can.rx_pending()) { Mcp2515::Can_frame rx; can.recv(rx); }
 */
#pragma once

#include <l4/sys/types.h>
#include <l4/sys/capability>
#include <l4/spi-driver/spi_device_if.h>

class Mcp2515
{
public:
  /** Crystal oscillator frequency fitted to the MCP2515 XTAL pins. */
  enum class Osc_freq { MHz8, MHz16 };

  /**
   * CAN bus bit rate.
   *
   * Available combinations with sample point at 75%:
   *
   *   Osc      125k   250k   500k   1000k
   *   8 MHz     yes    yes    yes     yes*
   *   16 MHz    yes    yes    yes     yes
   *
   * (*) 8 MHz / 1000 kbps uses a 4-TQ bit time (sample at 75%).
   */
  enum class Bit_rate
  {
    kbps_125  = 0,
    kbps_250  = 1,
    kbps_500  = 2,
    kbps_1000 = 3,
  };

  /** A CAN 2.0A/B frame. */
  struct Can_frame
  {
    l4_uint32_t id;       /**< CAN ID (11-bit standard or 29-bit extended). */
    bool        extended; /**< true = extended 29-bit frame. */
    bool        rtr;      /**< Remote Transmission Request. */
    l4_uint8_t  dlc;      /**< Data length code (0–8). */
    l4_uint8_t  data[8];  /**< Payload. */
  };

  /**
   * Construct an MCP2515 driver.
   *
   * \param spi  SPI device capability from the spi-driver factory.
   *             Must be configured for SPI mode 0 (CPOL=CPHA=0) and
   *             the desired clock speed (≤ 10 MHz).
   * \param osc  Crystal frequency soldered onto the MCP2515 board.
   */
  explicit Mcp2515(L4::Cap<Spi_device_ops> spi,
                   Osc_freq osc = Osc_freq::MHz8);

  /**
   * Initialise the MCP2515 and switch to normal operating mode.
   *
   * Resets the chip, programs the bit timing for \a rate, accepts all
   * incoming frames (no filtering), and enables RXB0→RXB1 rollover.
   * The device is left in normal mode upon success.
   *
   * \param rate  Desired CAN bus bit rate.
   * \retval L4_EOK  Success.
   * \retval <0      I/O error or mode-switch timeout.
   */
  long init(Bit_rate rate = Bit_rate::kbps_500);

  /** Switch to normal (on-bus) mode. */
  long set_mode_normal();

  /**
   * Switch to internal loopback mode.
   *
   * In loopback mode the TX and RX paths are connected internally and
   * no frames appear on the CAN bus pins.  Useful for self-tests.
   */
  long set_mode_loopback();

  /** Switch to listen-only mode (receive only, no ACK transmitted). */
  long set_mode_listen_only();

  /**
   * Transmit a CAN frame using TX buffer 0.
   *
   * Loads the frame, requests transmission, and polls until the hardware
   * reports completion (TXREQ bit cleared).
   *
   * \param frame  Frame to send; \c dlc must be 0–8.
   * \retval L4_EOK     Frame transmitted successfully.
   * \retval -L4_EINVAL dlc > 8.
   * \retval -L4_EIO    SPI error or TX timeout.
   */
  long send(Can_frame const &frame);

  /**
   * Receive one CAN frame from RXB0 or RXB1.
   *
   * Checks the RX status register and reads from whichever buffer holds
   * a message (RXB0 first, then RXB1).  Clears the corresponding RX
   * interrupt flag so the buffer is freed for the next message.
   *
   * \param[out] frame  Filled with the received frame on success.
   * \retval L4_EOK     Frame received; \a frame is valid.
   * \retval -L4_ENOENT No message pending.
   * \retval <0         SPI error.
   */
  long recv(Can_frame &frame);

  /**
   * Return true if at least one received frame is waiting in RXB0 or RXB1.
   *
   * This is a quick poll that reads the RX_STATUS SPI instruction.
   * On SPI error it returns false.
   */
  bool rx_pending();

  /**
   * Read the transmit and receive error counters from TEC/REC registers.
   *
   * Error counters > 127 indicate error-passive state; > 255 bus-off.
   *
   * \param[out] tec  Transmit Error Counter.
   * \param[out] rec  Receive Error Counter.
   * \retval L4_EOK  Success.
   * \retval <0      SPI error.
   */
  long get_error_counters(l4_uint8_t *tec, l4_uint8_t *rec);

private:
  long reset();
  long read_reg(l4_uint8_t addr, l4_uint8_t *val);
  long write_reg(l4_uint8_t addr, l4_uint8_t val);
  long bit_modify(l4_uint8_t addr, l4_uint8_t mask, l4_uint8_t data);
  long read_status(l4_uint8_t *status);
  long read_rx_status(l4_uint8_t *status);
  long set_mode(l4_uint8_t reqop);
  long load_tx_buf(Can_frame const &frame);
  long read_rx_buf(l4_uint8_t buf_idx, Can_frame &frame);

  L4::Cap<Spi_device_ops> _spi;
  Osc_freq                _osc;
};
