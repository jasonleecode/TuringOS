/*
 * MCP2515 CAN controller driver implementation.
 *
 * All communication uses the L4Re SPI device RPC interface:
 *   write()    – for commands that require no response (WRITE, BIT_MODIFY, RTS)
 *   transfer() – for full-duplex exchanges (READ, READ_STATUS, READ_RX_BUFFER)
 *
 * Polling is used throughout (no interrupt line required).
 */

#include <l4/mcp2515/mcp2515.h>
#include <l4/util/util.h>
#include <l4/sys/err.h>
#include <cstring>

// ---- SPI instruction set (MCP2515 datasheet table 12-1) ----

static constexpr l4_uint8_t CMD_WRITE       = 0x02;
static constexpr l4_uint8_t CMD_READ        = 0x03;
static constexpr l4_uint8_t CMD_BITMOD      = 0x05;
static constexpr l4_uint8_t CMD_LOAD_TX0    = 0x40; // Load TXB0 from SIDH
static constexpr l4_uint8_t CMD_RTS_TX0     = 0x81; // Request-to-Send TXB0
static constexpr l4_uint8_t CMD_READ_RX0    = 0x90; // Read RXB0 from SIDH
static constexpr l4_uint8_t CMD_READ_RX1    = 0x94; // Read RXB1 from SIDH
static constexpr l4_uint8_t CMD_READ_STATUS = 0xA0;
static constexpr l4_uint8_t CMD_RX_STATUS   = 0xB0;
static constexpr l4_uint8_t CMD_RESET       = 0xC0;

// ---- Register addresses ----

static constexpr l4_uint8_t REG_TEC      = 0x1C;
static constexpr l4_uint8_t REG_REC      = 0x1D;
static constexpr l4_uint8_t REG_CNF3     = 0x28;
static constexpr l4_uint8_t REG_CNF2     = 0x29;
static constexpr l4_uint8_t REG_CNF1     = 0x2A;
static constexpr l4_uint8_t REG_CANINTE  = 0x2B;
static constexpr l4_uint8_t REG_CANINTF  = 0x2C;
static constexpr l4_uint8_t REG_CANSTAT  = 0x0E;
static constexpr l4_uint8_t REG_CANCTRL  = 0x0F;
static constexpr l4_uint8_t REG_TXB0CTRL = 0x30;
static constexpr l4_uint8_t REG_RXB0CTRL = 0x60;
static constexpr l4_uint8_t REG_RXB1CTRL = 0x70;

// ---- CANCTRL / CANSTAT operation mode masks ----

static constexpr l4_uint8_t REQOP_MASK      = 0xE0;
static constexpr l4_uint8_t REQOP_NORMAL    = 0x00;
static constexpr l4_uint8_t REQOP_SLEEP     = 0x20;
static constexpr l4_uint8_t REQOP_LOOPBACK  = 0x40;
static constexpr l4_uint8_t REQOP_LISTENONLY = 0x60;
static constexpr l4_uint8_t REQOP_CONFIG    = 0x80;

// ---- TXBnCTRL ----

static constexpr l4_uint8_t TXREQ          = 0x08; // Transmission request

// ---- CANINTF ----

static constexpr l4_uint8_t INTF_RX0IF     = 0x01;
static constexpr l4_uint8_t INTF_RX1IF     = 0x02;

// ---- RXBnCTRL ----

// Accept all frames regardless of filters
static constexpr l4_uint8_t RXB_RXM_ANY    = 0x60;
// Roll over from RXB0 to RXB1 when RXB0 is full (RXB0CTRL only)
static constexpr l4_uint8_t RXB0_BUKT      = 0x04;

// ---- TXBnSIDL / RXBnSIDL bit flags ----

static constexpr l4_uint8_t SIDL_EXIDE     = 0x08; // Extended frame
static constexpr l4_uint8_t SIDL_SRTR      = 0x10; // Standard remote frame

// ---- Bit timing tables ----
//
// All configurations target a 75% sample point using an 8-TQ bit time
// (Sync=1, Prop=2, PS1=3, PS2=2).  The only exception is the 8 MHz /
// 1 Mbps entry which uses a 4-TQ bit time (Sync=1, Prop=1, PS1=1, PS2=1).
//
//   CNF2 for 8-TQ: BTLMODE=1, SAM=0, PHSEG1=2(3 TQ), PRSEG=1(2 TQ) = 0x91
//   CNF3 for 8-TQ: PHSEG2=1(2 TQ)                                   = 0x01
//   CNF2 for 4-TQ: BTLMODE=1, SAM=0, PHSEG1=0(1 TQ), PRSEG=0(1 TQ) = 0x80
//   CNF3 for 4-TQ: PHSEG2=0(1 TQ)                                   = 0x00

struct Bit_timing { l4_uint8_t cnf1, cnf2, cnf3; };

// 8 MHz crystal: TQ = 2*(BRP+1) / 8 MHz
static constexpr Bit_timing bt_8mhz[4] = {
  { 0x03, 0x91, 0x01 }, // 125 kbps  BRP=3, TQ=1.0 µs,  8 TQ
  { 0x01, 0x91, 0x01 }, // 250 kbps  BRP=1, TQ=0.5 µs,  8 TQ
  { 0x00, 0x91, 0x01 }, // 500 kbps  BRP=0, TQ=0.25 µs, 8 TQ
  { 0x00, 0x80, 0x00 }, // 1000 kbps BRP=0, TQ=0.25 µs, 4 TQ
};

// 16 MHz crystal: TQ = 2*(BRP+1) / 16 MHz
static constexpr Bit_timing bt_16mhz[4] = {
  { 0x07, 0x91, 0x01 }, // 125 kbps  BRP=7, TQ=1.0 µs,  8 TQ
  { 0x03, 0x91, 0x01 }, // 250 kbps  BRP=3, TQ=0.5 µs,  8 TQ
  { 0x01, 0x91, 0x01 }, // 500 kbps  BRP=1, TQ=0.25 µs, 8 TQ
  { 0x00, 0x91, 0x01 }, // 1000 kbps BRP=0, TQ=0.125 µs,8 TQ
};

// Maximum poll iterations before declaring a timeout
static constexpr unsigned Poll_max = 1000;

// ---- Constructor ----

Mcp2515::Mcp2515(L4::Cap<Spi_device_ops> spi, Osc_freq osc)
: _spi(spi), _osc(osc)
{}

// ---- Private SPI helpers ----

long Mcp2515::reset()
{
  l4_uint8_t tx = CMD_RESET;
  long r = _spi->write(L4::Ipc::Array<l4_uint8_t const>(1, &tx));
  if (r < 0)
    return r;
  l4_usleep(128); // ≥ 128 µs for OST (oscillator start-up timer)
  return L4_EOK;
}

long Mcp2515::read_reg(l4_uint8_t addr, l4_uint8_t *val)
{
  l4_uint8_t tx[3] = { CMD_READ, addr, 0x00 };
  l4_uint8_t rx[3] = {};
  L4::Ipc::Array<l4_uint8_t> rx_arr(3, rx);
  long r = _spi->transfer(L4::Ipc::Array<l4_uint8_t const>(3, tx), 3, rx_arr);
  if (r == L4_EOK)
    *val = rx[2];
  return r;
}

long Mcp2515::write_reg(l4_uint8_t addr, l4_uint8_t val)
{
  l4_uint8_t tx[3] = { CMD_WRITE, addr, val };
  return _spi->write(L4::Ipc::Array<l4_uint8_t const>(3, tx));
}

long Mcp2515::bit_modify(l4_uint8_t addr, l4_uint8_t mask, l4_uint8_t data)
{
  l4_uint8_t tx[4] = { CMD_BITMOD, addr, mask, data };
  return _spi->write(L4::Ipc::Array<l4_uint8_t const>(4, tx));
}

long Mcp2515::read_status(l4_uint8_t *status)
{
  l4_uint8_t tx[2] = { CMD_READ_STATUS, 0x00 };
  l4_uint8_t rx[2] = {};
  L4::Ipc::Array<l4_uint8_t> rx_arr(2, rx);
  long r = _spi->transfer(L4::Ipc::Array<l4_uint8_t const>(2, tx), 2, rx_arr);
  if (r == L4_EOK)
    *status = rx[1];
  return r;
}

long Mcp2515::read_rx_status(l4_uint8_t *status)
{
  l4_uint8_t tx[2] = { CMD_RX_STATUS, 0x00 };
  l4_uint8_t rx[2] = {};
  L4::Ipc::Array<l4_uint8_t> rx_arr(2, rx);
  long r = _spi->transfer(L4::Ipc::Array<l4_uint8_t const>(2, tx), 2, rx_arr);
  if (r == L4_EOK)
    *status = rx[1];
  return r;
}

long Mcp2515::set_mode(l4_uint8_t reqop)
{
  long r = bit_modify(REG_CANCTRL, REQOP_MASK, reqop);
  if (r < 0)
    return r;

  // CANSTAT.OPMOD[7:5] mirrors CANCTRL.REQOP[7:5] once the switch completes.
  for (unsigned i = 0; i < Poll_max; ++i)
    {
      l4_uint8_t stat = 0;
      r = read_reg(REG_CANSTAT, &stat);
      if (r < 0)
        return r;
      if ((stat & REQOP_MASK) == (reqop & REQOP_MASK))
        return L4_EOK;
      l4_usleep(10);
    }
  return -L4_EIO; // mode-switch timeout
}

long Mcp2515::load_tx_buf(Can_frame const &frame)
{
  l4_uint8_t sidh, sidl, eid8, eid0, dlc;

  if (!frame.extended)
    {
      // Standard 11-bit ID: ID[10:3] → SIDH, ID[2:0] → SIDL[7:5]
      sidh = static_cast<l4_uint8_t>((frame.id >> 3) & 0xFF);
      sidl = static_cast<l4_uint8_t>(((frame.id & 0x07) << 5)
                                     | (frame.rtr ? SIDL_SRTR : 0x00));
      eid8 = 0x00;
      eid0 = 0x00;
    }
  else
    {
      // Extended 29-bit ID: ID[28:21]→SIDH, ID[20:18]→SIDL[7:5],
      //                      EXIDE=1, ID[17:16]→SIDL[1:0],
      //                      ID[15:8]→EID8, ID[7:0]→EID0
      sidh = static_cast<l4_uint8_t>((frame.id >> 21) & 0xFF);
      sidl = static_cast<l4_uint8_t>(
               (((frame.id >> 18) & 0x07) << 5)
               | SIDL_EXIDE
               | ((frame.id >> 16) & 0x03));
      eid8 = static_cast<l4_uint8_t>((frame.id >> 8) & 0xFF);
      eid0 = static_cast<l4_uint8_t>(frame.id & 0xFF);
    }

  // RTR for extended frames lives in TXBnDLC[6]; for standard frames in SIDL[4]
  dlc = (frame.extended && frame.rtr ? 0x40 : 0x00) | (frame.dlc & 0x0F);

  // Command: [CMD_LOAD_TX0, SIDH, SIDL, EID8, EID0, DLC, D0..D(dlc-1)]
  l4_uint8_t tx[14]; // 1 + 5 header + 8 data (max)
  tx[0] = CMD_LOAD_TX0;
  tx[1] = sidh;
  tx[2] = sidl;
  tx[3] = eid8;
  tx[4] = eid0;
  tx[5] = dlc;
  unsigned n = 6;
  for (unsigned i = 0; i < frame.dlc; ++i)
    tx[n++] = frame.data[i];

  return _spi->write(L4::Ipc::Array<l4_uint8_t const>(n, tx));
}

long Mcp2515::read_rx_buf(l4_uint8_t buf_idx, Can_frame &frame)
{
  // READ_RX clocks out: SIDH, SIDL, EID8, EID0, DLC, D0..D7  (13 bytes)
  // Total transfer: 1 command byte + 13 payload bytes = 14 bytes
  l4_uint8_t tx[14] = {};
  tx[0] = (buf_idx == 0) ? CMD_READ_RX0 : CMD_READ_RX1;
  l4_uint8_t rx[14] = {};
  L4::Ipc::Array<l4_uint8_t> rx_arr(14, rx);
  long r = _spi->transfer(L4::Ipc::Array<l4_uint8_t const>(14, tx), 14, rx_arr);
  if (r < 0)
    return r;

  // rx[0] = don't-care response to the command byte
  l4_uint8_t sidh = rx[1];
  l4_uint8_t sidl = rx[2];
  l4_uint8_t eid8 = rx[3];
  l4_uint8_t eid0 = rx[4];
  l4_uint8_t dlc  = rx[5];

  frame.extended = (sidl & SIDL_EXIDE) != 0;
  frame.rtr      = (dlc & 0x40) != 0;
  frame.dlc      = dlc & 0x0F;
  if (frame.dlc > 8)
    frame.dlc = 8;

  if (!frame.extended)
    {
      frame.id = (static_cast<l4_uint32_t>(sidh) << 3)
                 | ((sidl >> 5) & 0x07);
    }
  else
    {
      frame.id = (static_cast<l4_uint32_t>(sidh) << 21)
                 | (static_cast<l4_uint32_t>((sidl >> 5) & 0x07) << 18)
                 | (static_cast<l4_uint32_t>(sidl & 0x03) << 16)
                 | (static_cast<l4_uint32_t>(eid8) << 8)
                 | eid0;
    }

  for (unsigned i = 0; i < frame.dlc; ++i)
    frame.data[i] = rx[6 + i];

  return L4_EOK;
}

// ---- Public API ----

long Mcp2515::init(Bit_rate rate)
{
  long r = reset();
  if (r < 0)
    return r;

  // After RESET the chip must be in configuration mode
  l4_uint8_t stat = 0;
  r = read_reg(REG_CANSTAT, &stat);
  if (r < 0)
    return r;
  if ((stat & REQOP_MASK) != REQOP_CONFIG)
    return -L4_EIO;

  // Program bit timing
  Bit_timing const &bt = (_osc == Osc_freq::MHz16)
                           ? bt_16mhz[static_cast<int>(rate)]
                           : bt_8mhz[static_cast<int>(rate)];
  r = write_reg(REG_CNF1, bt.cnf1);  if (r < 0) return r;
  r = write_reg(REG_CNF2, bt.cnf2);  if (r < 0) return r;
  r = write_reg(REG_CNF3, bt.cnf3);  if (r < 0) return r;

  // Accept all frames; enable RXB0→RXB1 rollover on overflow
  r = write_reg(REG_RXB0CTRL, RXB_RXM_ANY | RXB0_BUKT);
  if (r < 0) return r;
  r = write_reg(REG_RXB1CTRL, RXB_RXM_ANY);
  if (r < 0) return r;

  // Disable all pin interrupts – we use polling
  r = write_reg(REG_CANINTE, 0x00);
  if (r < 0) return r;

  return set_mode(REQOP_NORMAL);
}

long Mcp2515::set_mode_normal()
{
  return set_mode(REQOP_NORMAL);
}

long Mcp2515::set_mode_loopback()
{
  return set_mode(REQOP_LOOPBACK);
}

long Mcp2515::set_mode_listen_only()
{
  return set_mode(REQOP_LISTENONLY);
}

long Mcp2515::send(Can_frame const &frame)
{
  if (frame.dlc > 8)
    return -L4_EINVAL;

  long r = load_tx_buf(frame);
  if (r < 0)
    return r;

  l4_uint8_t rts = CMD_RTS_TX0;
  r = _spi->write(L4::Ipc::Array<l4_uint8_t const>(1, &rts));
  if (r < 0)
    return r;

  // Wait for TXREQ to clear (hardware clears it when the frame is sent)
  for (unsigned i = 0; i < Poll_max; ++i)
    {
      l4_uint8_t ctrl = 0;
      r = read_reg(REG_TXB0CTRL, &ctrl);
      if (r < 0)
        return r;
      if (!(ctrl & TXREQ))
        return L4_EOK;
      l4_usleep(10);
    }
  return -L4_EIO; // TX timeout
}

long Mcp2515::recv(Can_frame &frame)
{
  l4_uint8_t rxst = 0;
  long r = read_rx_status(&rxst);
  if (r < 0)
    return r;

  // RX_STATUS bits 7:6 – bit 6 = RXB0, bit 7 = RXB1
  if (rxst & 0x40)
    {
      r = read_rx_buf(0, frame);
      if (r < 0) return r;
      return bit_modify(REG_CANINTF, INTF_RX0IF, 0x00);
    }
  if (rxst & 0x80)
    {
      r = read_rx_buf(1, frame);
      if (r < 0) return r;
      return bit_modify(REG_CANINTF, INTF_RX1IF, 0x00);
    }

  return -L4_ENOENT; // no message pending
}

bool Mcp2515::rx_pending()
{
  l4_uint8_t rxst = 0;
  if (read_rx_status(&rxst) < 0)
    return false;
  return (rxst & 0xC0) != 0;
}

long Mcp2515::get_error_counters(l4_uint8_t *tec, l4_uint8_t *rec)
{
  long r = read_reg(REG_TEC, tec);
  if (r < 0) return r;
  return read_reg(REG_REC, rec);
}
