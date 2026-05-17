// Copyright (C) 2024, Mark Qvist

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

// CBA Reticulum includes must come before local to avoid collision with local defines
#ifdef HAS_RNS
#include <Transport.h>
#include <Reticulum.h>
#include <Interface.h>
#include <Log.h>
#include <Bytes.h>
#endif
#if defined(UDP_TRANSPORT)
#include "UDPInterface.h"
#endif
#if defined(TCP_TRANSPORT)
#include "TCPTransport.h"
#endif
#if defined(HAS_LXMF_GATEWAY)
#include "LXMF/LXMFGateway.h"
#include "LXMF/AnnounceLog.h"
#include "LXMF/RatchetBridge.h"
#include "Web/WebUI.h"
#include "Web/RtcPCF8563.h"
#include "Web/Gps.h"
#include "Web/Ntp.h"
#include "Web/SDCard.h"
#include "Web/Bme280.h"
#include "Web/QmcMag.h"
#include "Web/QmiImu.h"
#include "Web/SensorConfig.h"
#endif

#include <Arduino.h>
#include <SPI.h>
#include "Utilities.h"

// CBA SD
#if HAS_SDCARD
#include <SD.h>
SPIClass SDSPI(HSPI);
#endif

#if MCU_VARIANT == MCU_ESP32
  #include <esp_task_wdt.h>
#endif

// WDT timeout
#define WDT_TIMEOUT 60  // seconds

FIFOBuffer serialFIFO;
uint8_t serialBuffer[CONFIG_UART_BUFFER_SIZE+1];

FIFOBuffer16 packet_starts;
uint16_t packet_starts_buf[CONFIG_QUEUE_MAX_LENGTH+1];

FIFOBuffer16 packet_lengths;
uint16_t packet_lengths_buf[CONFIG_QUEUE_MAX_LENGTH+1];

uint8_t packet_queue[CONFIG_QUEUE_SIZE];

#if defined(HAS_LXMF_GATEWAY)
// Diagnostic: per-burst BLE-in byte counter, drained either when 64 bytes
// have accumulated (see buffer_serial) or when a 50ms idle gap is detected
// (see main loop).  Updated from buffer_serial; flushed from loop().
uint32_t ble_in_burst_bytes = 0;
uint32_t ble_in_last_byte_ms = 0;
#endif

volatile uint8_t queue_height = 0;
volatile uint16_t queued_bytes = 0;
volatile uint16_t queue_cursor = 0;
volatile uint16_t current_packet_start = 0;
volatile bool serial_buffering = false;
#if HAS_BLUETOOTH || HAS_BLE == true
  bool bt_init_ran = false;
#endif

#if HAS_CONSOLE
  #include "Console.h"
#endif

#if PLATFORM == PLATFORM_ESP32 || PLATFORM == PLATFORM_NRF52
  #define MODEM_QUEUE_SIZE 8
  typedef struct {
          size_t len;
          int rssi;
          int snr_raw;
          uint8_t data[];
  } modem_packet_t;
  static xQueueHandle modem_packet_queue = NULL;
#endif

char sbuf[128];

#if MCU_VARIANT == MCU_ESP32 || MCU_VARIANT == MCU_NRF52
  bool packet_ready = false;
#endif

#if MCU_VARIANT == MCU_ESP32 || MCU_VARIANT == MCU_NRF52
void update_csma_parameters();
#endif

#ifdef HAS_RNS
// CBA LoRa interface
class LoRaInterface : public RNS::InterfaceImpl {
public:
	LoRaInterface(const char *name) : RNS::InterfaceImpl(name) {
		_IN = true;
		_OUT = true;
		_HW_MTU = 508;
	}
	LoRaInterface() : LoRaInterface("LoRaInterface") {}
	virtual ~LoRaInterface() {
		_name = "deleted";
	}
protected:
	virtual void handle_incoming(const RNS::Bytes& data) {
    TRACEF("LoRaInterface.handle_incoming: (%u bytes) data: %s", data.size(), data.toHex().c_str());
    TRACE("LoRaInterface.handle_incoming: sending packet to rns...");
    try {
      InterfaceImpl::handle_incoming(data);
    }
    catch (const std::bad_alloc&) {
      ERROR("LoRaInterface::handle_incoming: bad_alloc - out of memory");
    }
    catch (std::exception& e) {
      ERRORF("LoRaInterface::handle_incoming: %s", e.what());
    }
  }
	virtual void send_outgoing(const RNS::Bytes& data) {
    // CBA NOTE header will be addded later by transmit function
    TRACEF("LoRaInterface.send_outgoing: (%u bytes) data: %s", data.size(), data.toHex().c_str());
    try {
      TRACE("LoRaInterface.send_outgoing: adding packet to outgoing queue...");
      for (size_t i = 0; i < data.size(); i++) {
          if (queue_height < CONFIG_QUEUE_MAX_LENGTH && queued_bytes < CONFIG_QUEUE_SIZE) {
              queued_bytes++;
              packet_queue[queue_cursor++] = data.data()[i];
              if (queue_cursor == CONFIG_QUEUE_SIZE) queue_cursor = 0;
          }
      }
      if (!fifo16_isfull(&packet_starts) && queued_bytes < CONFIG_QUEUE_SIZE) {
          uint16_t s = current_packet_start;
          int16_t e = queue_cursor-1; if (e == -1) e = CONFIG_QUEUE_SIZE-1;
          uint16_t l;

          if (s != e) {
              l = (s < e) ? e - s + 1 : CONFIG_QUEUE_SIZE - s + e + 1;
          } else {
              l = 1;
          }

          if (l >= MIN_L) {
              queue_height++;

              fifo16_push(&packet_starts, s);
              fifo16_push(&packet_lengths, l);

              current_packet_start = queue_cursor;
          }

      }
      // Perform post-send housekeeping
      InterfaceImpl::handle_outgoing(data);
    }
    catch (const std::bad_alloc&) {
      ERROR("LoRaInterface::send_outgoing: bad_alloc - out of memory");
    }
    catch (std::exception& e) {
      ERRORF("LoRaInterface::send_outgoing: %s", e.what());
    }
  }
};

// CBA logger callback
void on_log(const char* msg, RNS::LogLevel level) {
  // KISS-mode hosts (reticulum-meshchat, rnsd, anything talking to us as
  // a serial RNode) cannot tolerate plain-text bytes interleaved with
  // CMD_DATA frames. Once we've flipped into MODE_TNC (host has sent
  // CMD_DETECT and the radio is up) wrap log lines in CMD_LOG KISS
  // frames so a KISS decoder either displays them on its own pane or
  // silently discards them — but the radio byte stream stays clean.
  //
  // Exception: the kiss_serial_output toggle is the user's
  // override for "I want a clean text monitor on USB, KISS host be
  // damned". When OFF, force the plain-text path regardless of
  // op_mode so `pio device monitor` actually shows diagnostics.
  if (op_mode == MODE_TNC && kiss_serial_output) {
    kiss_indicate_log((uint8_t)level, msg);
    return;
  }
  // MODE_HOST (development / LXMF gateway), or KISS suppressed: plain
  // text for pio monitor.
	Serial.print(RNS::getTimeString());
	Serial.print(" [");
	Serial.print(RNS::getLevelName(level));
	Serial.print("] ");
	Serial.println(msg);
	Serial.flush();
/*
  String line = RNS::getTimeString() + String(" [") + RNS::getLevelName(level) + "] " + msg + "\n";
	Serial.print(line);
	Serial.flush();
*/

#ifdef HAS_SDCARD
	File file = SD.open("/logfile.txt", FILE_APPEND);
	if (file) {
    file.write((uint8_t*)msg, strlen(msg));
    file.close();
  }
#endif  // HAS_SDCARD
}

// CBA receive packet callback
void on_receive_packet(const RNS::Bytes& raw, const RNS::Interface& interface) {
#ifdef HAS_SDCARD
  TRACE("Logging receive packet to SD");
  String line = RNS::getTimeString() + String(" recv: ") + String(raw.toHex().c_str()) + "\n";
	File file = SD.open("/tracefile.txt", FILE_APPEND);
	if (file) {
    file.write((uint8_t*)line.c_str(), line.length());
    file.close();
  }
	RNS::Packet packet(raw);
	if (packet.unpack()) {
    String line = RNS::getTimeString() + String(" recv: ") + String(packet.dumpString().c_str()) + "\n";
    File file = SD.open("/tracedetails.txt", FILE_APPEND);
    if (file) {
      file.write((uint8_t*)line.c_str(), line.length());
      file.close();
    }
	}
#endif  // HAS_SDCARD

#if defined(HAS_LXMF_GATEWAY)
  // Diagnostic: log every inbound packet at the raw-bytes level before
  // attempting an unpack, so we see packets that may be in a format we
  // can't parse. Then attempt unpack and classify by packet_type.
  NOTICEF("RX RAW %u bytes (head: %02x %02x %02x %02x)",
          (unsigned)raw.size(),
          raw.size() > 0 ? raw.data()[0] : 0,
          raw.size() > 1 ? raw.data()[1] : 0,
          raw.size() > 2 ? raw.data()[2] : 0,
          raw.size() > 3 ? raw.data()[3] : 0);
  RNS::Packet pkt(raw);
  if (pkt.unpack()) {
    if (pkt.packet_type() == RNS::Type::Packet::ANNOUNCE) {
      NOTICEF("RX ANNOUNCE dest=%s hops=%u data=%u bytes",
              pkt.destination_hash().toHex().c_str(),
              (unsigned)pkt.hops(),
              (unsigned)pkt.data().size());
    } else if (pkt.packet_type() == RNS::Type::Packet::DATA) {
      // Log all DATA packets for our destinations AND any not-clearly-routing
      // packet so we can see if anything Columba sends is reaching us at all.
      const bool ours = LXMF::LXMFGateway::is_own_destination(pkt.destination_hash());
      NOTICEF("RX DATA dest=%s hops=%u data=%u bytes ctx=%u%s",
              pkt.destination_hash().toHex().c_str(),
              (unsigned)pkt.hops(),
              (unsigned)pkt.data().size(),
              (unsigned)pkt.context(),
              ours ? " (OURS)" : "");
    } else if (pkt.packet_type() == RNS::Type::Packet::LINKREQUEST) {
      NOTICEF("RX LINKREQUEST dest=%s hops=%u data=%u bytes",
              pkt.destination_hash().toHex().c_str(),
              (unsigned)pkt.hops(),
              (unsigned)pkt.data().size());
    } else if (pkt.packet_type() == RNS::Type::Packet::PROOF) {
      NOTICEF("RX PROOF dest=%s hops=%u data=%u bytes",
              pkt.destination_hash().toHex().c_str(),
              (unsigned)pkt.hops(),
              (unsigned)pkt.data().size());
    }
  }
#endif
}

// CBA transmit packet callback
void on_transmit_packet(const RNS::Bytes& raw, const RNS::Interface& interface) {
#ifdef HAS_SDCARD
  TRACE("Logging transmit packet to SD");
  String line = RNS::getTimeString() + String(" send: ") + String(raw.toHex().c_str()) + "\n";
	File file = SD.open("/tracefile.txt", FILE_APPEND);
	if (file) {
    file.write((uint8_t*)line.c_str(), line.length());
    file.close();
  }
	RNS::Packet packet(raw);
	if (packet.unpack()) {
    String line = RNS::getTimeString() + String(" send: ") + String(packet.dumpString().c_str()) + "\n";
    File file = SD.open("/tracedetails.txt", FILE_APPEND);
    if (file) {
      file.write((uint8_t*)line.c_str(), line.length());
      file.close();
    }
	}
#endif  // HAS_SDCARD

#if defined(HAS_LXMF_GATEWAY)
  NOTICEF("TX RAW %u bytes (head: %02x %02x %02x %02x)",
          (unsigned)raw.size(),
          raw.size() > 0 ? raw.data()[0] : 0,
          raw.size() > 1 ? raw.data()[1] : 0,
          raw.size() > 2 ? raw.data()[2] : 0,
          raw.size() > 3 ? raw.data()[3] : 0);
  RNS::Packet pkt(raw);
  if (pkt.unpack()) {
    const uint8_t flag = (raw.size() > 0) ? ((raw.data()[0] >> 5) & 0x01) : 0;
    if (pkt.packet_type() == RNS::Type::Packet::ANNOUNCE) {
      NOTICEF("TX ANNOUNCE dest=%s ctx_flag=%u data=%u bytes",
              pkt.destination_hash().toHex().c_str(),
              (unsigned)flag,
              (unsigned)pkt.data().size());
    } else if (pkt.packet_type() == RNS::Type::Packet::DATA) {
      NOTICEF("TX DATA dest=%s ctx=%u data=%u bytes",
              pkt.destination_hash().toHex().c_str(),
              (unsigned)pkt.context(),
              (unsigned)pkt.data().size());
    } else if (pkt.packet_type() == RNS::Type::Packet::LINKREQUEST) {
      NOTICEF("TX LINKREQUEST dest=%s data=%u bytes",
              pkt.destination_hash().toHex().c_str(),
              (unsigned)pkt.data().size());
    } else if (pkt.packet_type() == RNS::Type::Packet::PROOF) {
      NOTICEF("TX PROOF dest=%s data=%u bytes",
              pkt.destination_hash().toHex().c_str(),
              (unsigned)pkt.data().size());
    }
  }
#endif
}

// CBA RNS
RNS::Reticulum reticulum(RNS::Type::NONE);
RNS::Interface lora_interface(RNS::Type::NONE);
#if defined(RNS_USE_FS)
  // CBA microStore
  #if MCU_VARIANT == MCU_ESP32
    #if defined(USTORE_USE_SD)
      #include <microStore/Adapters/SDFileSystem.h>
      microStore::FileSystem filesystem{microStore::Adapters::SDFileSystem(SDCARD_SCLK, SDCARD_MISO, SDCARD_MOSI, SDCARD_CS)};
    #else
      //#include <microStore/Adapters/SPIFFSFileSystem.h>
      //microStore::FileSystem filesystem{microStore::Adapters::SPIFFSFileSystem()};
      //#include <microStore/Adapters/LittleFSFileSystem.h>
      //microStore::FileSystem filesystem{microStore::Adapters::LittleFSFileSystem()};
      #include <microStore/Adapters/PosixFileSystem.h>
      microStore::FileSystem filesystem{microStore::Adapters::PosixFileSystem()};
    #endif
  #elif MCU_VARIANT == MCU_NRF52
    #include <microStore/Adapters/InternalFSFileSystem.h>
    #include <microStore/Adapters/FlashFSFileSystem.h>
    microStore::FileSystem filesystem;
  #else
    #include <microStore/Adapters/PosixFileSystem.h>
    microStore::FileSystem filesystem{microStore::Adapters::PosixFileSystem()};
  #endif
  #else // RNS_USE_FS
    microStore::FileSystem filesystem{microStore::Adapters::NoopFileSystem()};
  #endif // RNS_USE_FS
#endif  // HAS_RNS

// CBA For printf
int _write(int file, char *ptr, int len) {
    size_t wrote = Serial.write(ptr, len);
    Serial.flush();
    return wrote;
}

#if HAS_DISPLAY && MCU_VARIANT == MCU_ESP32
// FreeRTOS task that pulls the periodic OLED refresh off the main loop
// (CPU 1, where reticulum.loop() and the LoRa modem ISR live) and onto
// CPU 0. Without this, any radio burst that holds the main loop for
// 100+ ms also freezes the display (and looks like a crash to the user).
// update_display() is internally idempotent and self-throttled, so we
// can poll it cheaply.
static void display_refresh_task(void* /*arg*/) {
  NOTICE("Display: refresh task started on core 0");
  uint32_t last_heartbeat = 0;
  uint32_t tick_count     = 0;
  while (true) {
    if (disp_ready && !display_updating) {
      // Deliberately NOT acquiring the rns_lock here. The whole point
      // of the display being on its own task is that the user can see
      // what's going on EVEN when the radio core / WebUI handlers are
      // stuck holding the lock. Single-word reads of RSSI, packet
      // counts, queue depth etc are atomic on ESP32-S3; the worst case
      // is the very occasional torn read of a multi-word field, which
      // is a cosmetic glitch, not a crash. update_stat_area /
      // update_disp_area read shared state best-effort; if it ever
      // matters that those reads are coherent, push a published
      // snapshot from the main loop instead.
      update_display();
    }
    tick_count++;
    // Heartbeat once a second so we can tell from serial output whether
    // the task is alive even when the screen visibly froze (which would
    // then mean it's the screen driver, not the task).
    const uint32_t now = millis();
    if (now - last_heartbeat > 1000) {
      last_heartbeat = now;
      DEBUGF("Display: heartbeat ticks=%u disp_ready=%d updating=%d",
             (unsigned)tick_count, (int)disp_ready, (int)display_updating);
    }
    // 33 ms ≈ 30 Hz cap. update_display() self-paces beyond this via
    // disp_update_interval; the delay is just to keep the task from
    // hot-spinning when the radio is idle.
    vTaskDelay(pdMS_TO_TICKS(33));
  }
}

static void start_display_refresh_task() {
  static TaskHandle_t handle = nullptr;
  if (handle) return;
  xTaskCreatePinnedToCore(
    display_refresh_task,
    "display",
    4096,
    nullptr,
    1,
    &handle,
    0  // core 0 (PRO_CPU)
  );
}
#endif

void setup() {

  // Initialise serial communication
  memset(serialBuffer, 0, sizeof(serialBuffer));
  fifo_init(&serialFIFO, serialBuffer, CONFIG_UART_BUFFER_SIZE);

  Serial.begin(serial_baudrate);

  // CBA Safely wait for serial initialization
  while (!Serial) {
    if (millis() > 2000) {
      break;
    }
    delay(10);
  }
  // CBA Test
  delay(2000);

  printf("Total SRAM:  %7u bytes\n", RNS::Utilities::Memory::heap_size());
  printf("Free SRAM:   %7u bytes\n", RNS::Utilities::Memory::heap_available());
#if defined(ESP32)
	printf("Total PSRAM: %7u bytes\n", ESP.getPsramSize());
#endif
	//printf("Total flash: %zu bytes\n", RNS::Utilities::OS::storage_size());

  // Configure WDT
  #if MCU_VARIANT == MCU_ESP32
    #if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
      esp_task_wdt_config_t wdt_config = {
          .timeout_ms     = WDT_TIMEOUT * 1000,
          .idle_core_mask = 0,
          .trigger_panic  = true,
      };
      // In IDF 5.x, the framework initializes TWDT before setup(); reconfigure
      // it with our timeout rather than calling init() (which would fail with
      // "TWDT already initialized").  Fall back to init() if not yet started.
      if (esp_task_wdt_reconfigure(&wdt_config) == ESP_ERR_INVALID_STATE) {
          esp_task_wdt_init(&wdt_config);
      }
    #else
      esp_task_wdt_init(WDT_TIMEOUT, true); // enable panic so ESP32 restarts
    #endif
    esp_task_wdt_add(NULL);               // add current thread to WDT watch
  #elif MCU_VARIANT == MCU_NRF52
    NRF_WDT->CONFIG         = 0x01;           // Configure WDT to run when CPU is asleep
    NRF_WDT->CRV            = WDT_TIMEOUT * 32768 + 1; // set timeout
    NRF_WDT->RREN           = 0x01;           // Enable the RR[0] reload register
    NRF_WDT->TASKS_START    = 1;              // Start WDT
  #endif

  #if MCU_VARIANT == MCU_ESP32
    boot_seq();
    EEPROM.begin(EEPROM_SIZE);
    Serial.setRxBufferSize(CONFIG_UART_BUFFER_SIZE);

    #if BOARD_MODEL == BOARD_TDECK
      pinMode(pin_poweron, OUTPUT);
      digitalWrite(pin_poweron, HIGH);

      pinMode(SD_CS, OUTPUT);
      pinMode(DISPLAY_CS, OUTPUT);
      digitalWrite(SD_CS, HIGH);
      digitalWrite(DISPLAY_CS, HIGH);

      pinMode(DISPLAY_BL_PIN, OUTPUT);
    #endif
  #endif

  #if MCU_VARIANT == MCU_NRF52
    #if BOARD_MODEL == BOARD_TECHO
      delay(200);
      pinMode(PIN_VEXT_EN, OUTPUT);
      digitalWrite(PIN_VEXT_EN, HIGH);
      pinMode(pin_btn_usr1, INPUT_PULLUP);
      pinMode(pin_btn_touch, INPUT_PULLUP);
      pinMode(PIN_LED_RED, OUTPUT);
      pinMode(PIN_LED_GREEN, OUTPUT);
      pinMode(PIN_LED_BLUE, OUTPUT);
      delay(200);
    #endif

    if (!eeprom_begin()) { Serial.write("EEPROM initialisation failed.\r\n"); }
  #endif

  // Seed the PRNG for CSMA R-value selection
  #if MCU_VARIANT == MCU_ESP32
    // On ESP32, get the seed value from the
    // hardware RNG
    unsigned long seed_val = (unsigned long)esp_random();
  #elif MCU_VARIANT == MCU_NRF52
    // On nRF, get the seed value from the
    // hardware RNG
    unsigned long seed_val = get_rng_seed();
  #else
    // Otherwise, get a pseudo-random seed
    // value from an unconnected analog pin
    //
    // CAUTION! If you are implementing the
    // firmware on a platform that does not
    // have a hardware RNG, you MUST take
    // care to get a seed value with enough
    // entropy at each device reset!
    unsigned long seed_val = analogRead(0);
  #endif
  randomSeed(seed_val);

  #if HAS_NP
    led_init();
  #endif

  #if MCU_VARIANT == MCU_NRF52 && HAS_NP == true
    boot_seq();
  #endif

  #if BOARD_MODEL != BOARD_RAK4631 && BOARD_MODEL != BOARD_HELTEC_T114 && BOARD_MODEL != BOARD_TECHO && BOARD_MODEL != BOARD_T3S3 && BOARD_MODEL != BOARD_TBEAM_S_V1 && BOARD_MODEL != BOARD_TBEAM_S_LR_V1 && BOARD_MODEL != BOARD_HELTEC32_V4
    // Some boards need to wait until the hardware UART is set up before booting
    // the full firmware. In the case of the RAK4631 and Heltec T114, the line below will wait
    // until a serial connection is actually established with a master. Thus, it
    // is disabled on this platform.
    while (!Serial);
  #endif

  serial_interrupt_init();

  // Configure input and output pins
  #if HAS_INPUT
    input_init();
  #endif

  #if HAS_NP == false
    pinMode(pin_led_rx, OUTPUT);
    pinMode(pin_led_tx, OUTPUT);
  #endif

  #if HAS_TCXO == true
    if (pin_tcxo_enable != -1) {
        pinMode(pin_tcxo_enable, OUTPUT);
        digitalWrite(pin_tcxo_enable, HIGH);
    }
  #endif

  // Initialise buffers
  memset(pbuf, 0, sizeof(pbuf));
  memset(cmdbuf, 0, sizeof(cmdbuf));
  
  memset(packet_queue, 0, sizeof(packet_queue));

  memset(packet_starts_buf, 0, sizeof(packet_starts_buf));
  fifo16_init(&packet_starts, packet_starts_buf, CONFIG_QUEUE_MAX_LENGTH);
  
  memset(packet_lengths_buf, 0, sizeof(packet_starts_buf));
  fifo16_init(&packet_lengths, packet_lengths_buf, CONFIG_QUEUE_MAX_LENGTH);

  #if PLATFORM == PLATFORM_ESP32 || PLATFORM == PLATFORM_NRF52
    modem_packet_queue = xQueueCreate(MODEM_QUEUE_SIZE, sizeof(modem_packet_t*));
  #endif

  // Set chip select, reset and interrupt
  // pins for the LoRa module
  #if MODEM == SX1276 || MODEM == SX1278
  LoRa->setPins(pin_cs, pin_reset, pin_dio, pin_busy);
  #elif MODEM == SX1262 || MODEM == LR11XX
  LoRa->setPins(pin_cs, pin_reset, pin_dio, pin_busy, pin_rxen);
  #elif MODEM == SX1280
  LoRa->setPins(pin_cs, pin_reset, pin_dio, pin_busy, pin_rxen, pin_txen);
  #endif
  
  #if MCU_VARIANT == MCU_ESP32 || MCU_VARIANT == MCU_NRF52
    init_channel_stats();

    #if BOARD_MODEL == BOARD_T3S3
      #if MODEM == SX1280
        delay(300);
        LoRa->reset();
        delay(100);
      #endif
    #endif

    #if BOARD_MODEL == BOARD_XIAO_S3
      // Improve wakeup from sleep
      delay(300);
      LoRa->reset();
      delay(100);
    #endif

    // Check installed transceiver chip and
    // probe boot parameters.
    if (LoRa->preInit()) {
      modem_installed = true;
      
      #if HAS_INPUT
        // Skip quick-reset console activation
      #else
        uint32_t lfr = LoRa->getFrequency();
        if (lfr == 0) {
          // Normal boot
        } else if (lfr == M_FRQ_R) {
          // Quick reboot
          #if HAS_CONSOLE
            if (rtc_get_reset_reason(0) == POWERON_RESET) {
              console_active = true;
            }
          #endif
        } else {
          // Unknown boot
        }
        LoRa->setFrequency(M_FRQ_S);
      #endif

    } else {
      modem_installed = false;
    }
  #else
    // Older variants only came with SX1276/78 chips,
    // so assume that to be the case for now.
    modem_installed = true;
  #endif

  #if HAS_DISPLAY
    #if HAS_EEPROM
    if (EEPROM.read(eeprom_addr(ADDR_CONF_DSET)) != CONF_OK_BYTE) {
    #elif MCU_VARIANT == MCU_NRF52
    if (eeprom_read(eeprom_addr(ADDR_CONF_DSET)) != CONF_OK_BYTE) {
    #endif
      eeprom_update(eeprom_addr(ADDR_CONF_DSET), CONF_OK_BYTE);
      #if BOARD_MODEL == BOARD_TECHO
        eeprom_update(eeprom_addr(ADDR_CONF_DINT), 0x03);
      #else
        eeprom_update(eeprom_addr(ADDR_CONF_DINT), 0xFF);
      #endif
    }
    #if BOARD_MODEL == BOARD_TECHO
      display_add_callback(work_while_waiting);
    #endif

    display_unblank();
    disp_ready = display_init();
    update_display();
    #if MCU_VARIANT == MCU_ESP32
      // Take periodic display refresh off the main loop so radio-busy
      // periods can't stall the OLED.
      if (disp_ready) start_display_refresh_task();
    #endif
  #endif

  #if MCU_VARIANT == MCU_ESP32 || MCU_VARIANT == MCU_NRF52
    #if HAS_PMU == true
      pmu_ready = init_pmu();
    #endif

    #if HAS_BLUETOOTH || HAS_BLE == true
      bt_init();
      bt_init_ran = true;
    #endif

    if (console_active) {
      #if HAS_CONSOLE
        console_start();
      #else
        kiss_indicate_reset();
      #endif
    } else {
      #if HAS_WIFI
        wifi_mode = EEPROM.read(eeprom_addr(ADDR_CONF_WIFI));
        if (wifi_mode == WR_WIFI_STA || wifi_mode == WR_WIFI_AP) { wifi_remote_init(); }
        #if defined(HAS_LXMF_GATEWAY)
          // Bootstrap fallback: if WiFi is not configured at all, bring up a
          // softAP using the device's BT name so the user can reach the web
          // UI for first-time setup. RAM-only flag, EEPROM is untouched so
          // a configured device stays in its configured mode.
          if (!wifi_initialized) {
            wifi_mode = WR_WIFI_AP;
            wifi_remote_init();
            if (wifi_initialized) {
              Web::WebUI::bootstrap_mode = true;
              NOTICE("WebUI: WiFi unconfigured — entered bootstrap softAP mode");
            }
          }
        #endif
      #endif
      kiss_indicate_reset();
    }
  #endif

  #if MCU_VARIANT == MCU_ESP32 || MCU_VARIANT == MCU_NRF52
    #if MODEM == SX1280
      avoid_interference = false;
    #else
      #if HAS_EEPROM
        uint8_t ia_conf = EEPROM.read(eeprom_addr(ADDR_CONF_DIA));
        if (ia_conf == 0x00) { avoid_interference = true; }
        else                 { avoid_interference = false; }
      #elif MCU_VARIANT == MCU_NRF52
        uint8_t ia_conf = eeprom_read(eeprom_addr(ADDR_CONF_DIA));
        if (ia_conf == 0x00) { avoid_interference = true; }
        else                 { avoid_interference = false; }
      #endif
    #endif
  #endif

  // Validate board health, EEPROM and config
  validate_status();

  if (op_mode != MODE_TNC) LoRa->setFrequency(0);

// CBA SD
#ifdef HAS_SDCARD
  pinMode(SDCARD_MISO, INPUT_PULLUP);
  SDSPI.begin(SDCARD_SCLK, SDCARD_MISO, SDCARD_MOSI, SDCARD_CS);
  if (!SD.begin(SDCARD_CS, SDSPI)) {
      Serial.println("setupSDCard FAIL");
  } else {
      uint32_t cardSize = SD.cardSize() / (1024 * 1024);
      Serial.print("setupSDCard PASS . SIZE = ");
      Serial.print(cardSize / 1024.0);
      Serial.println(" GB");
      SD.remove("/logfile");
      SD.remove("/logfile.txt");
      SD.remove("/tracefile");
      SD.remove("/tracedetails");
      SD.remove("/tracefile.txt");
      SD.remove("/tracedetails.txt");
      Serial.println("DIR: /");
      File root = SD.open("/");
      File file = root.openNextFile();
      while(file){
          Serial.print("  FILE: ");
          Serial.println(file.name());
          file = root.openNextFile();
      }
  }
  delay(3000);
#endif

#ifdef HAS_RNS

  // Set sane default memory limits based on hardware-specific availability
  // (note these may be adjusted dynamically based on detected hardware below)
  RNS::Transport::path_table_maxsize(URTN_PATH_TABLE_MAX_RECS);
  RNS::Transport::announce_table_maxsize(50);
  RNS::Transport::hashlist_maxsize(50);
  RNS::Identity::known_destinations_maxsize(50);
  RNS::Transport::max_pr_tags(50);
  RNS::Reticulum::clean_interval(60*15); // 60 minutes
  //RNS::Reticulum::clean_interval(60*15); // 15 minutes
  RNS::Reticulum::persist_interval(60*60); // 60 minutes
  //RNS::Reticulum::persist_interval(60*10); // 10 minutes
  //RNS::Reticulum::persist_interval(60); // 1 minute

  try {
    // CBA Init filesystem
    HEAD("Initializing filesystem...", RNS::LOG_TRACE);
#if MCU_VARIANT == MCU_NRF52
    // First attempt to initialize RAK15001 flash
    TRACE("Looking for RAK15001 flash...");
    static const SPIFlash_Device_t device_rak15001 = RAK15001;
    filesystem = microStore::Adapters::FlashFSFileSystem(&device_rak15001);
    if (filesystem.init()) {
      TRACE("Initialized RAK15001 flash");
      // Raise path store limits to account for larger external flash size
      RNS::Transport::path_table_maxsize(500);
      RNS::Transport::path_store_segment_size(24576);
      RNS::Transport::path_store_segment_count(8);
    }
    else {
      // Finaly attempt to initialize internl flash
      TRACE("Using internal flash...");
      filesystem = microStore::Adapters::InternalFSFileSystem();
      filesystem.init();
      TRACE("Initialized internal flash");
    }
#else
    filesystem.init();
#endif

    // Seed the wall clock from the on-board hardware RTC.
    // The PCF8563 on the T-Beam Supreme keeps time across reboots
    // and shutdowns via its coin-cell backup. If it has a valid
    // value we feed it to TimeManager so outbound LXMF timestamps
    // are sensible even before any live source (GPS/NTP/Browser)
    // reports. Once a live source adopts, TimeManager fires the
    // on_adopt hook below which writes the new time back to the RTC.
#if BOARD_MODEL == BOARD_TBEAM_S_V1 || BOARD_MODEL == BOARD_TBEAM_S_LR_V1
    {
      // T-Beam Supreme: PCF8563 RTC sits on the PMU I2C bus (Wire1),
      // SDA=42 / SCL=41, addr 0x51. The "main" Wire bus on 17/18
      // hosts the OLED + BME280 + magnetometer (Bus 0); the RTC is
      // on Bus 1 alongside the AXP2101 PMU. See LilyGo's docs:
      // https://wiki.lilygo.cc/get_started/en/LoRa_GPS/T-Beam-SUPREME
      Web::RtcPCF8563::Pins pins{ /*sda=*/42, /*scl=*/41, /*hz=*/100000 };
      Web::RtcPCF8563::init_and_seed(Wire1, pins);
      // RTC write-through whenever a higher-trust source adopts.
      Web::TimeManager::set_on_adopt([](Web::TimeManager::Source src, double epoch) {
        if (!Web::RtcPCF8563::available()) return;
        if (Web::RtcPCF8563::write_epoch(epoch)) {
          NOTICEF("RtcPCF8563: write-through epoch %.0f (from %s)",
                  epoch, Web::TimeManager::source_name(src));
        }
      });
    }
    // GPS — L76K on UART1, pins 8/9, EN on 7. Pumps NMEA into
    // the parser; valid RMC fixes call TimeManager::report_time
    // (Source::GPS) per the user-configured interval.
    {
      Web::Gps::Pins pins{ /*rx=*/9, /*tx=*/8, /*en=*/7, /*baud=*/9600 };
      Web::Gps::begin(Serial1, pins);
    }
    // NTP — non-blocking SNTP against pool.ntp.org. Sync
    // happens when WiFi STA becomes ready; pump() handles adoption.
    Web::Ntp::begin();
    // SD card — mount the microSD slot if a card is inserted.
    // The card's power rails (BLDO1 + BLDO2 on the AXP2101) are
    // disabled by default in Power.h. Bring them up here, and do a
    // full power-cycle (off → wait → on) matching LilyGo's factory
    // reference — some cards need the off-edge before they'll
    // initialise cleanly.
    if (PMU && PMU->getChipModel() == XPOWERS_AXP2101) {
      NOTICE("SDCard: power-cycling BLDO1/BLDO2 (AXP2101 SD rails)");
      PMU->disablePowerOutput(XPOWERS_BLDO1);
      PMU->disablePowerOutput(XPOWERS_BLDO2);
      delay(250);
      PMU->setPowerChannelVoltage(XPOWERS_BLDO1, 3300);
      PMU->enablePowerOutput(XPOWERS_BLDO1);
      PMU->setPowerChannelVoltage(XPOWERS_BLDO2, 3300);
      PMU->enablePowerOutput(XPOWERS_BLDO2);
      delay(100);
      const bool b1_on = PMU->isPowerChannelEnable(XPOWERS_BLDO1);
      const int  b1_mV = PMU->getPowerChannelVoltage(XPOWERS_BLDO1);
      const bool b2_on = PMU->isPowerChannelEnable(XPOWERS_BLDO2);
      const int  b2_mV = PMU->getPowerChannelVoltage(XPOWERS_BLDO2);
      NOTICEF("SDCard: BLDO1 on=%d %dmV  BLDO2 on=%d %dmV",
              (int)b1_on, b1_mV, (int)b2_on, b2_mV);
      Web::SDCard::set_rail_state(b1_on, b1_mV, b2_on, b2_mV);
    } else {
      NOTICEF("SDCard: PMU=%p chip=%d — not AXP2101, leaving rails alone",
              (void*)PMU, PMU ? (int)PMU->getChipModel() : -1);
    }
    Web::SDCard::begin();
    // Bring up the user/sensor I2C bus (Wire) at SDA=17 SCL=18
    // — this is where BME280 lives (and where future QMC6310
    // magnetometer + any other Wire-side sensors will sit). PMU /
    // RTC are on Wire1 (42/41), untouched by this.
    Wire.begin(17, 18);
    Web::Bme280::begin(Wire);
    // QMC6310 magnetometer — also on Wire (0x1C or 0x3C).
    Web::QmcMag::begin(Wire);
    // QMI8658 IMU — on the HSPI bus shared with the SD slot.
    // Its begin() reuses SDCard::ensure_shared_bus() so we don't
    // double-init the bus.
    Web::QmiImu::begin();
    // Restore user-configured enable/interval overrides on
    // top of the driver defaults. No-op if /lxmf/sensors.json doesn't
    // exist yet (factory state).
    Web::SensorConfig::load(filesystem);
#endif

    // Remove legacy files
    if (filesystem.exists("/destination_table")) filesystem.remove("/destination_table");
    if (filesystem.isDirectory("/cache")) {
      filesystem.listDirectory("/cache", [&](const char* path) -> void {
        char rmpath[64];
        snprintf(rmpath, 64, "/cache/%s", path);
        filesystem.remove(rmpath);
      });
      filesystem.rmdir("/cache");
    }

    // If filesystem is essentially full then clear all path store files
    if (filesystem.storageAvailable() < 1024) {
      WARNING("FileSystem is full, clearing space...");
      // CBA Delete the path store index file to force a rebuild
      filesystem.remove("/path_store_index.dat");
      // CBA Remove all path store data files
      filesystem.remove("/path_store_0.dat");
      filesystem.remove("/path_store_1.dat");
      filesystem.remove("/path_store_2.dat");
      filesystem.remove("/path_store_3.dat");
      filesystem.remove("/path_store_4.dat");
      filesystem.remove("/path_store_5.dat");
      filesystem.remove("/path_store_6.dat");
      filesystem.remove("/path_store_7.dat");
    }

    TRACE("Registering filesystem...");
    RNS::Utilities::OS::register_filesystem(filesystem);

#if !defined(NDEBUG) && defined(RNS_USE_FS)
#if 0
    filesystem.format();
#endif
#if 1
    Serial.println("Listing filesystem /:");
    filesystem.listDirectory("/", [&](const char* path) -> void {
      Serial.print("  ");
      Serial.println(path);
    });
#endif
#endif // !NDEBUG && RNS_USE_FS

    // CBA Start RNS
    //if (hw_ready) {
    if (true) {

      //reticulum.clear_caches();

      // Configure callbacks
      RNS::set_log_callback(&on_log);
      RNS::Transport::set_receive_packet_callback(on_receive_packet);
      RNS::Transport::set_transmit_packet_callback(on_transmit_packet);

      HEAD("Starting RNS...\r\n", RNS::LOG_VERBOSE);
#if defined(RNS_MEM_LOG)
      RNS::loglevel(RNS::LOG_MEM);
#else
      RNS::loglevel(RNS::LOG_TRACE);
#endif

      HEAD("Registering LoRA Interface...", RNS::LOG_TRACE);
      lora_interface = new LoRaInterface();
      lora_interface.mode(RNS::Type::Interface::MODE_GATEWAY);
      RNS::Transport::register_interface(lora_interface);
      TRACEF("LoRaInterface hash: %s", lora_interface.get_hash().toHex().c_str());

#if HAS_WIFI && defined(UDP_TRANSPORT)
      HEAD("Registering UDP Interface...", RNS::LOG_TRACE);
      udp_interface = new UDPInterface();
      udp_interface.mode(RNS::Type::Interface::MODE_GATEWAY);
      RNS::Transport::register_interface(udp_interface);
      TRACEF("UDPInterface hash: %s", udp_interface.get_hash().toHex().c_str());
#endif

#if HAS_WIFI && defined(TCP_TRANSPORT)
      HEAD("Registering TCP Interfaces...", RNS::LOG_TRACE);
      TCPTransport::setup();
#endif

      HEAD("Creating Reticulum instance...", RNS::LOG_TRACE);
      reticulum = RNS::Reticulum();
      reticulum.transport_enabled(op_mode == MODE_TNC);
#if defined(HAS_LXMF_GATEWAY)
      // User-controllable override from the WebUI. /lxmf/transport.json
      // is a tiny JSON file with {"enabled": bool} written by the
      // POST /api/system/transport handler. Overrides the op_mode-
      // derived default above when present.
      if (filesystem.exists("/lxmf/transport.json")) {
        std::vector<uint8_t> data;
        if (filesystem.readFile("/lxmf/transport.json", data) > 0) {
          JsonDocument tdoc;
          if (deserializeJson(tdoc, data.data(), data.size()) == DeserializationError::Ok) {
            bool want = tdoc["enabled"] | false;
            reticulum.transport_enabled(want);
            NOTICEF("WebUI: persisted transport_enabled=%s applied", want ? "true" : "false");
          }
        }
      }
#endif
      reticulum.probe_destination_enabled(true);
      reticulum.start();

      // Set loop callback only after the Reticulum instance is started
      // (to avoid looping without a completely initialized instance)
      RNS::Utilities::OS::set_loop_callback(&loop);

      // CBA load/create local destination for admin node
#if 0
      RNS::Identity identity = {RNS::Type::NONE};
      std::string local_identity_path = RNS::Reticulum::_storagepath + "/local_identity";
      if (RNS::Utilities::OS::file_exists(local_identity_path.c_str())) {
        identity = RNS::Identity::from_file(local_identity_path.c_str());
      }
      if (!identity) {
        RNS::verbose("No valid local identity in storage, creating...");
        identity = RNS::Identity();
        identity.to_file(local_identity_path.c_str());
      }
      else {
        RNS::verbose("Loaded local identity from storage");
      }
      RNS::Destination destination(identity, RNS::Type::Destination::IN, RNS::Type::Destination::SINGLE, "rnstransport", "local");
#endif
      RNS::Destination destination(RNS::Transport::identity(), RNS::Type::Destination::IN, RNS::Type::Destination::SINGLE, "rnstransport", "local");

#if defined(HAS_LXMF_GATEWAY)
      HEAD("Initializing LXMF gateway...", RNS::LOG_TRACE);
      // Inbox cap + TTL config. Load before LXMFGateway::setup
      // so identity activation picks up the user's settings rather
      // than the compiled default. The TTL prune needs a wall-clock
      // — wire TimeManager::now_epoch as the inbox clock source.
      LXMF::LXMFInbox::set_now_epoch_provider(&Web::TimeManager::now_epoch);
      LXMF::InboxConfig::load(filesystem);
      LXMF::LXMFGateway::setup();
      LXMF::AnnounceLog::setup();
      // Wire the microReticulum ratchet patches to our gateway-backed
      // providers. Must run AFTER setup() so identities (and their ratchet
      // rings) are loaded before the first announce / decrypt fires.
      LXMF::register_ratchet_providers();
#endif

      HEAD("RNS is READY!", RNS::LOG_TRACE);
      if (op_mode == MODE_TNC) {
        HEAD("RNS transport mode is ENABLED", RNS::LOG_TRACE);
        TRACEF("Frequency: %d Hz", lora_freq);
        TRACEF("Bandwidth: %d Hz", lora_bw);
        TRACEF("TX Power: %d dBm", lora_txp);
        TRACEF("Spreading Factor: %d", lora_sf);
        TRACEF("Coding Rate: %d", lora_cr);
        HEAD("RNS Transport is READY!", RNS::LOG_TRACE);
      }
      else {
        HEAD("RNS transport mode is DISABLED", RNS::LOG_INFO);
        HEAD("Configure TNC mode with radio configuration to enable RNS transport", RNS::LOG_INFO);
      }
      //RNS::loglevel(RNS::LOG_NONE);
    }
    else {
      HEAD("RNS is inoperable because hardware is not ready!", RNS::LOG_ERROR);
      HEAD("Check firmware signature and eeprom provisioning", RNS::LOG_ERROR);
      // CBA Clear cached files just in case cached files are responsible for failure
  		//reticulum.clear_caches();
    }
  }
  catch (const std::bad_alloc&) {
    ERROR("RNS startup failed: bad_alloc - out of memory");
  }
  catch (std::exception& e) {
    ERRORF("RNS startup failed: %s", e.what());
  }
#endif  // HAS_RNS
}

void lora_receive() {
  if (!implicit) {
    LoRa->receive();
  } else {
    LoRa->receive(implicit_l);
  }
}

inline void kiss_write_packet() {

#ifdef HAS_RNS
  TRACEF("Received %d byte packet", host_write_len);
  // CBA send packet received over LoRa to RNS in addition to connected client
  // CBA RESERVE
  //RNS::Bytes data();
  RNS::Bytes data(512);
  for (uint16_t i = 0; i < host_write_len; i++) {
    #if MCU_VARIANT == MCU_NRF52
      portENTER_CRITICAL();
      uint8_t byte = pbuf[i];
      portEXIT_CRITICAL();
    #else
      uint8_t byte = pbuf[i];
    #endif
    data << byte;
  }
  lora_interface.handle_incoming(data);
#endif

  serial_write(FEND);
  serial_write(CMD_DATA);
  
  for (uint16_t i = 0; i < host_write_len; i++) {
    #if MCU_VARIANT == MCU_NRF52
      portENTER_CRITICAL();
      uint8_t byte = pbuf[i];
      portEXIT_CRITICAL();
    #else
      uint8_t byte = pbuf[i];
    #endif

    if (byte == FEND) { serial_write(FESC); byte = TFEND; }
    if (byte == FESC) { serial_write(FESC); byte = TFESC; }
    serial_write(byte);
  }

  serial_write(FEND);
  host_write_len = 0;

  #if MCU_VARIANT == MCU_ESP32 || MCU_VARIANT == MCU_NRF52
    packet_ready = false;
  #endif

  #if MCU_VARIANT == MCU_ESP32
    #if HAS_BLE
      bt_flush();
    #endif
  #endif
}

inline void getPacketData(uint16_t len) {
  #if MCU_VARIANT != MCU_NRF52
    while (len-- && read_len < MTU) {
      pbuf[read_len++] = LoRa->read();
    }  
  #else
    BaseType_t int_mask = taskENTER_CRITICAL_FROM_ISR();
    while (len-- && read_len < MTU) {
      pbuf[read_len++] = LoRa->read();
    }
    taskEXIT_CRITICAL_FROM_ISR(int_mask);
  #endif
}

void ISR_VECT receive_callback(int packet_size) {
  #if MCU_VARIANT == MCU_ESP32 || MCU_VARIANT == MCU_NRF52
    BaseType_t int_mask;
  #endif

  bool    ready    = false;
  if (!promisc) { // Not in promiscuous mode
    // The standard operating mode allows large
    // packets with a payload up to 500 bytes,
    // by combining two raw LoRa packets.
    // We read the 1-byte header and extract
    // packet sequence number and split flags
    uint8_t header   = LoRa->read(); packet_size--;
    uint8_t sequence = packetSequence(header);

    if (isSplitPacket(header) && seq == SEQ_UNSET) {
      // This is the first part of a split
      // packet, so we set the seq variable
      // and add the data to the buffer
      #if MCU_VARIANT == MCU_NRF52
        int_mask = taskENTER_CRITICAL_FROM_ISR(); read_len = 0; taskEXIT_CRITICAL_FROM_ISR(int_mask);
      #else
        read_len = 0;
      #endif
      
      seq = sequence;

      #if MCU_VARIANT != MCU_ESP32 && MCU_VARIANT != MCU_NRF52
        last_rssi = LoRa->packetRssi();
        last_snr_raw = LoRa->packetSnrRaw();
      #endif

      getPacketData(packet_size);

    } else if (isSplitPacket(header) && seq == sequence) {
      // This is the second part of a split
      // packet, so we add it to the buffer
      // and set the ready flag.
      #if MCU_VARIANT != MCU_ESP32 && MCU_VARIANT != MCU_NRF52
        last_rssi = (last_rssi+LoRa->packetRssi())/2;
        last_snr_raw = (last_snr_raw+LoRa->packetSnrRaw())/2;
      #endif

      getPacketData(packet_size);
      seq = SEQ_UNSET;
      ready = true;

    } else if (isSplitPacket(header) && seq != sequence) {
      // This split packet does not carry the
      // same sequence id, so we must assume
      // that we are seeing the first part of
      // a new split packet.
      #if MCU_VARIANT == MCU_NRF52
        int_mask = taskENTER_CRITICAL_FROM_ISR(); read_len = 0; taskEXIT_CRITICAL_FROM_ISR(int_mask);
      #else
        read_len = 0;
      #endif
      seq = sequence;

      #if MCU_VARIANT != MCU_ESP32 && MCU_VARIANT != MCU_NRF52
        last_rssi = LoRa->packetRssi();
        last_snr_raw = LoRa->packetSnrRaw();
      #endif

      getPacketData(packet_size);

    } else if (!isSplitPacket(header)) {
      // This is not a split packet, so we
      // just read it and set the ready
      // flag to true.

      if (seq != SEQ_UNSET) {
        // If we already had part of a split
        // packet in the buffer, we clear it.
        #if MCU_VARIANT == MCU_NRF52
          int_mask = taskENTER_CRITICAL_FROM_ISR(); read_len = 0; taskEXIT_CRITICAL_FROM_ISR(int_mask);
        #else
          read_len = 0;
        #endif
        seq = SEQ_UNSET;
      }

      #if MCU_VARIANT != MCU_ESP32 && MCU_VARIANT != MCU_NRF52
        last_rssi = LoRa->packetRssi();
        last_snr_raw = LoRa->packetSnrRaw();
      #endif

      getPacketData(packet_size);
      ready = true;
    }
  } else { // In promiscuous mode
    // In promiscuous mode, raw packets are
    // output directly to the host
    read_len = 0;

    #if MCU_VARIANT != MCU_ESP32 && MCU_VARIANT != MCU_NRF52
      last_rssi = LoRa->packetRssi();
      last_snr_raw = LoRa->packetSnrRaw();
      getPacketData(packet_size);

      // We first signal the RSSI of the
      // recieved packet to the host.
      kiss_indicate_stat_rssi();
      kiss_indicate_stat_snr();

      // And then write the entire packet
      kiss_write_packet();

    #else
      getPacketData(packet_size);
      packet_ready = true;
    #endif
  }

  if (ready) {
    #if MCU_VARIANT != MCU_ESP32 && MCU_VARIANT != MCU_NRF52
      // We first signal the RSSI of the
      // recieved packet to the host.
      kiss_indicate_stat_rssi();
      kiss_indicate_stat_snr();

      // And then write the entire packet
      host_write_len = read_len;
      kiss_write_packet(); read_len = 0;

    #else
      // Allocate packet struct, but abort if there
      // is not enough memory available.
      modem_packet_t *modem_packet = (modem_packet_t*)malloc(sizeof(modem_packet_t) + read_len);
      if(!modem_packet) { memory_low = true; return; }

      // Get packet RSSI and SNR
      #if MCU_VARIANT == MCU_ESP32
        modem_packet->snr_raw = LoRa->packetSnrRaw();
        modem_packet->rssi = LoRa->packetRssi(modem_packet->snr_raw);
      #endif

      // Send packet to event queue, but free the
      // allocated memory again if the queue is
      // unable to receive the packet.
      modem_packet->len = read_len;
      memcpy(modem_packet->data, pbuf, read_len); read_len = 0;
      if (!modem_packet_queue || xQueueSendFromISR(modem_packet_queue, &modem_packet, NULL) != pdPASS) {
          free(modem_packet);
      }
    #endif
  }
}

bool startRadio() {
  update_radio_lock();
  if (!radio_online && !console_active) {
    if (!radio_locked && hw_ready) {
      if (!LoRa->begin(lora_freq)) {
        // The radio could not be started.
        // Indicate this failure over both the
        // serial port and with the onboard LEDs
        radio_error = true;
        // Surface a CRITICAL log line + machine-readable hint over /api/info
        // (radio.error_reason). The most common cause is a wrong-variant
        // flash (e.g. SX1262 firmware on an LR1121 board, or vice versa) —
        // the SPI commands the compiled driver sends don't get the expected
        // responses from the actual chip, begin() returns false, and the
        // firmware otherwise comes up looking healthy while the radio
        // never radiates. The compile-time MODEM define identifies which
        // chip THIS build expects.
        #if MODEM == SX1262
          radio_error_expected_chip = "SX1262";
        #elif MODEM == LR11XX
          radio_error_expected_chip = "LR1121 / LR1120 (LR11xx family)";
        #elif MODEM == SX1276
          radio_error_expected_chip = "SX1276";
        #elif MODEM == SX1278
          radio_error_expected_chip = "SX1278";
        #elif MODEM == SX1280
          radio_error_expected_chip = "SX1280";
        #else
          radio_error_expected_chip = "(unknown — MODEM define not recognised)";
        #endif
        radio_error_reason =
            "Radio chip not responding to driver. This firmware was built for "
            "the chip listed in radio.expected_chip; if the actual chip on the "
            "board is different (a wrong-variant flash), re-flash with the env "
            "that matches the board's radio chip. WiFi / web UI stay up so "
            "recovery is possible without a power cycle.";
        CRITICALF("Radio init failed — expected %s, chip not responding. "
                  "Likely wrong-variant flash. See radio.error_reason in /api/info.",
                  radio_error_expected_chip);
        kiss_indicate_error(ERROR_INITRADIO);
        led_indicate_error(0);
        return false;
      } else {
        radio_online = true;

        init_channel_stats();

        setTXPower();
        setBandwidth();
        setSpreadingFactor();
        setCodingRate();
        getFrequency();

        LoRa->enableCrc();
        LoRa->onReceive(receive_callback);
        lora_receive();

        // Flash an info pattern to indicate
        // that the radio is now on
        kiss_indicate_radiostate();
        led_indicate_info(3);
        return true;
      }

    } else {
      // Flash a warning pattern to indicate
      // that the radio was locked, and thus
      // not started
      radio_online = false;
      kiss_indicate_radiostate();
      led_indicate_warning(3);
      return false;
    }
  } else {
    // If radio is already on, we silently
    // ignore the request.
    kiss_indicate_radiostate();
    return true;
  }
}

void stopRadio() {
  LoRa->end();
  radio_online = false;
}

void update_radio_lock() {
  if (lora_freq != 0 && lora_bw != 0 && lora_txp != 0xFF && lora_sf != 0) {
    radio_locked = false;
  } else {
    radio_locked = true;
  }
}

bool queue_full() { return (queue_height >= CONFIG_QUEUE_MAX_LENGTH || queued_bytes >= CONFIG_QUEUE_SIZE); }

volatile bool queue_flushing = false;
void flush_queue(void) {
  if (!queue_flushing) {
    queue_flushing = true;
    led_tx_on();

    #if MCU_VARIANT == MCU_ESP32 || MCU_VARIANT == MCU_NRF52
    while (!fifo16_isempty(&packet_starts)) {
    #else
    while (!fifo16_isempty_locked(&packet_starts)) {
    #endif

      uint16_t start = fifo16_pop(&packet_starts);
      uint16_t length = fifo16_pop(&packet_lengths);

      if (length >= MIN_L && length <= MTU) {
        for (uint16_t i = 0; i < length; i++) {
          uint16_t pos = (start+i)%CONFIG_QUEUE_SIZE;
          tbuf[i] = packet_queue[pos];
        }

        transmit(length);
      }
    }

    lora_receive(); led_tx_off();
  }

  queue_height = 0;
  queued_bytes = 0;

  #if MCU_VARIANT == MCU_ESP32 || MCU_VARIANT == MCU_NRF52
    update_airtime();
  #endif

  queue_flushing = false;

  #if HAS_DISPLAY
    display_tx = true;
  #endif
}

void pop_queue() {
  if (!queue_flushing) {
    queue_flushing = true; led_tx_on();

    #if MCU_VARIANT == MCU_ESP32 || MCU_VARIANT == MCU_NRF52
    if (!fifo16_isempty(&packet_starts)) {
    #else
    if (!fifo16_isempty_locked(&packet_starts)) {
    #endif

      uint16_t start = fifo16_pop(&packet_starts);
      uint16_t length = fifo16_pop(&packet_lengths);
      if (length >= MIN_L && length <= MTU) {
        for (uint16_t i = 0; i < length; i++) {
          uint16_t pos = (start+i)%CONFIG_QUEUE_SIZE;
          tbuf[i] = packet_queue[pos];
        }

        transmit(length);
      }
      queue_height -= 1;
      queued_bytes -= length;
    }

    lora_receive(); led_tx_off();
  }

  #if MCU_VARIANT == MCU_ESP32 || MCU_VARIANT == MCU_NRF52
    update_airtime();
  #endif

  queue_flushing = false;

  #if HAS_DISPLAY
    display_tx = true;
  #endif
}

void add_airtime(uint16_t written) {
  #if MCU_VARIANT == MCU_ESP32 || MCU_VARIANT == MCU_NRF52
    float lora_symbols = 0;
    float packet_cost_ms = 0.0;
    int ldr_opt = 0; if (lora_low_datarate) ldr_opt = 1;

    #if MODEM == SX1276 || MODEM == SX1278
      lora_symbols += (8*written + PHY_CRC_LORA_BITS - 4*lora_sf + 8 + PHY_HEADER_LORA_SYMBOLS);
      lora_symbols /=                          4*(lora_sf-2*ldr_opt);
      lora_symbols *= lora_cr;
      lora_symbols += lora_preamble_symbols + 0.25 + 8;
      packet_cost_ms += lora_symbols * lora_symbol_time_ms;
      
    #elif MODEM == SX1262 || MODEM == SX1280 || MODEM == LR11XX
      if (lora_sf < 7) {
        lora_symbols += (8*written + PHY_CRC_LORA_BITS - 4*lora_sf + PHY_HEADER_LORA_SYMBOLS);
        lora_symbols /=                              4*lora_sf;
        lora_symbols *= lora_cr;
        lora_symbols += lora_preamble_symbols + 2.25 + 8;
        packet_cost_ms += lora_symbols * lora_symbol_time_ms;

      } else {
        lora_symbols += (8*written + PHY_CRC_LORA_BITS - 4*lora_sf + 8 + PHY_HEADER_LORA_SYMBOLS);
        lora_symbols /=                         4*(lora_sf-2*ldr_opt);
        lora_symbols *= lora_cr;
        lora_symbols += lora_preamble_symbols + 0.25 + 8;
        packet_cost_ms += lora_symbols * lora_symbol_time_ms;
      }
    
    #endif

    uint16_t cb = current_airtime_bin();
    uint16_t nb = cb+1; if (nb == AIRTIME_BINS) { nb = 0; }
    airtime_bins[cb] += packet_cost_ms;
    airtime_bins[nb] = 0;

  #endif
}

void update_airtime() {
  #if MCU_VARIANT == MCU_ESP32 || MCU_VARIANT == MCU_NRF52
    uint16_t cb = current_airtime_bin();
    uint16_t pb = cb-1; if (cb-1 < 0) { pb = AIRTIME_BINS-1; }
    uint16_t nb = cb+1; if (nb == AIRTIME_BINS) { nb = 0; }
    airtime_bins[nb] = 0; airtime = (float)(airtime_bins[cb]+airtime_bins[pb])/(2.0*AIRTIME_BINLEN_MS);

    uint32_t longterm_airtime_sum = 0;
    for (uint16_t bin = 0; bin < AIRTIME_BINS; bin++) { longterm_airtime_sum += airtime_bins[bin]; }
    longterm_airtime = (float)longterm_airtime_sum/(float)AIRTIME_LONGTERM_MS;

    float longterm_channel_util_sum = 0.0;
    for (uint16_t bin = 0; bin < AIRTIME_BINS; bin++) { longterm_channel_util_sum += longterm_bins[bin]; }
    longterm_channel_util = (float)longterm_channel_util_sum/(float)AIRTIME_BINS;

    #if MCU_VARIANT == MCU_ESP32 || MCU_VARIANT == MCU_NRF52
      update_csma_parameters();
    #endif

    kiss_indicate_channel_stats();
  #endif
}

void transmit(uint16_t size) {
  if (!radio_online) {
#if defined(HAS_LXMF_GATEWAY)
    NOTICEF("LoRa TX REJECTED: radio_online=false size=%u", (unsigned)size);
#endif
  }
  if (radio_online) {
#if defined(HAS_LXMF_GATEWAY)
    NOTICEF("LoRa TX from queue: size=%u (head: %02x %02x %02x %02x) promisc=%d",
            (unsigned)size,
            size > 0 ? tbuf[0] : 0, size > 1 ? tbuf[1] : 0,
            size > 2 ? tbuf[2] : 0, size > 3 ? tbuf[3] : 0,
            (int)promisc);
#endif
    if (!promisc) {
      uint16_t  written = 0;
      uint8_t header  = random(256) & 0xF0;
      if (size > SINGLE_MTU - HEADER_L) { header = header | FLAG_SPLIT; }

      LoRa->beginPacket();
      LoRa->write(header); written++;

      for (uint16_t i=0; i < size; i++) {
        LoRa->write(tbuf[i]); written++;

        if (written == 255 && isSplitPacket(header)) {
          if (!LoRa->endPacket()) {
            kiss_indicate_error(ERROR_MODEM_TIMEOUT);
            kiss_indicate_error(ERROR_TXFAILED);
            led_indicate_error(5);
            hard_reset();
          }

          add_airtime(written);
          LoRa->beginPacket();
          LoRa->write(header);
          written = 1;
        }
      }

      if (!LoRa->endPacket()) {
        kiss_indicate_error(ERROR_MODEM_TIMEOUT);
        kiss_indicate_error(ERROR_TXFAILED);
        led_indicate_error(5);
        hard_reset();
      }

      add_airtime(written);

    } else {
      led_tx_on(); uint16_t written = 0;
      if (size > SINGLE_MTU) { size = SINGLE_MTU; }
      if (!implicit) { LoRa->beginPacket(); }
      else           { LoRa->beginPacket(size); }
      for (uint16_t i=0; i < size; i++) { LoRa->write(tbuf[i]); written++; }
      LoRa->endPacket(); add_airtime(written);
    }

  } else { kiss_indicate_error(ERROR_TXFAILED); led_indicate_error(5); }
}

void serial_callback(uint8_t sbyte) {
  if (IN_FRAME && sbyte == FEND && command == CMD_DATA) {
    IN_FRAME = false;

    if (!fifo16_isfull(&packet_starts) && queued_bytes < CONFIG_QUEUE_SIZE) {
        uint16_t s = current_packet_start;
        int16_t e = queue_cursor-1; if (e == -1) e = CONFIG_QUEUE_SIZE-1;
        uint16_t l;

        if (s != e) { l = (s < e) ? e - s + 1 : CONFIG_QUEUE_SIZE - s + e + 1; }
        else        { l = 1; }

        if (l >= MIN_L) {
            queue_height++;
            fifo16_push(&packet_starts, s);
            fifo16_push(&packet_lengths, l);
            current_packet_start = queue_cursor;
#if defined(HAS_LXMF_GATEWAY)
            {
              uint8_t b0 = packet_queue[s % CONFIG_QUEUE_SIZE];
              uint8_t b1 = packet_queue[(s+1) % CONFIG_QUEUE_SIZE];
              uint8_t b2 = packet_queue[(s+2) % CONFIG_QUEUE_SIZE];
              uint8_t b3 = packet_queue[(s+3) % CONFIG_QUEUE_SIZE];
              NOTICEF("HOST FRAME queued for LoRa TX: len=%u (head: %02x %02x %02x %02x) via=%s",
                      (unsigned)l, b0, b1, b2, b3,
                      bt_state == BT_STATE_CONNECTED ? "BLE" : "serial");
            }
#endif
        }
#if defined(HAS_LXMF_GATEWAY)
        else {
          NOTICEF("HOST FRAME REJECTED: len=%u < MIN_L=%u", (unsigned)l, (unsigned)MIN_L);
        }
#endif
    }
#if defined(HAS_LXMF_GATEWAY)
    else {
      NOTICEF("HOST FRAME DROPPED: packet_starts_full=%d queued_bytes=%lu (cap=%lu)",
              (int)fifo16_isfull(&packet_starts),
              (unsigned long)queued_bytes, (unsigned long)CONFIG_QUEUE_SIZE);
    }
#endif

  } else if (sbyte == FEND) {
    IN_FRAME = true;
    command = CMD_UNKNOWN;
    frame_len = 0;
  } else if (IN_FRAME && frame_len < MTU) {
    // Have a look at the command byte first
    if (frame_len == 0 && command == CMD_UNKNOWN) {
        command = sbyte;
#if defined(HAS_LXMF_GATEWAY)
        if (command != CMD_DATA) {
          NOTICEF("HOST KISS command byte=0x%02x (not CMD_DATA=0x%02x) via=%s",
                  (unsigned)command, (unsigned)CMD_DATA,
                  bt_state == BT_STATE_CONNECTED ? "BLE" : "serial");
        }
#endif
    } else if (command == CMD_DATA) {
        if (bt_state != BT_STATE_CONNECTED) {
          cable_state = CABLE_STATE_CONNECTED;
        }
        if (sbyte == FESC) {
            ESCAPE = true;
        } else {
            if (ESCAPE) {
                if (sbyte == TFEND) sbyte = FEND;
                if (sbyte == TFESC) sbyte = FESC;
                ESCAPE = false;
            }
            if (queue_height < CONFIG_QUEUE_MAX_LENGTH && queued_bytes < CONFIG_QUEUE_SIZE) {
              queued_bytes++;
              packet_queue[queue_cursor++] = sbyte;
              if (queue_cursor == CONFIG_QUEUE_SIZE) queue_cursor = 0;
            }
        }
    } else if (command == CMD_FREQUENCY) {
      if (sbyte == FESC) {
            ESCAPE = true;
        } else {
            if (ESCAPE) {
                if (sbyte == TFEND) sbyte = FEND;
                if (sbyte == TFESC) sbyte = FESC;
                ESCAPE = false;
            }
            if (frame_len < CMD_L) cmdbuf[frame_len++] = sbyte;
        }

        if (frame_len == 4) {
          uint32_t freq = (uint32_t)cmdbuf[0] << 24 | (uint32_t)cmdbuf[1] << 16 | (uint32_t)cmdbuf[2] << 8 | (uint32_t)cmdbuf[3];

          if (freq == 0) {
            kiss_indicate_frequency();
          } else {
            lora_freq = freq;
            if (op_mode == MODE_HOST) setFrequency();
            kiss_indicate_frequency();
          }
        }
    } else if (command == CMD_BANDWIDTH) {
      if (sbyte == FESC) {
            ESCAPE = true;
        } else {
            if (ESCAPE) {
                if (sbyte == TFEND) sbyte = FEND;
                if (sbyte == TFESC) sbyte = FESC;
                ESCAPE = false;
            }
            if (frame_len < CMD_L) cmdbuf[frame_len++] = sbyte;
        }

        if (frame_len == 4) {
          uint32_t bw = (uint32_t)cmdbuf[0] << 24 | (uint32_t)cmdbuf[1] << 16 | (uint32_t)cmdbuf[2] << 8 | (uint32_t)cmdbuf[3];

          if (bw == 0) {
            kiss_indicate_bandwidth();
          } else {
            lora_bw = bw;
            if (op_mode == MODE_HOST) setBandwidth();
            kiss_indicate_bandwidth();
          }
        }
    } else if (command == CMD_TXPOWER) {
      if (sbyte == 0xFF) {
        kiss_indicate_txpower();
      } else {
        int txp = sbyte;
        #if MODEM == SX1262 || MODEM == LR11XX
          #if HAS_LORA_PA
            if (txp > PA_MAX_OUTPUT) txp = PA_MAX_OUTPUT;
          #else
            if (txp > 22) txp = 22;
          #endif
        #elif MODEM == SX1280
          #if HAS_PA
            if (txp > 20) txp = 20;
          #else
            if (txp > 13) txp = 13;
          #endif
        #else
          if (txp > 17) txp = 17;
        #endif

        lora_txp = txp;
        if (op_mode == MODE_HOST) setTXPower();
        kiss_indicate_txpower();
      }
    } else if (command == CMD_SF) {
      if (sbyte == 0xFF) {
        kiss_indicate_spreadingfactor();
      } else {
        int sf = sbyte;
        if (sf < 5) sf = 5;
        if (sf > 12) sf = 12;

        lora_sf = sf;
        if (op_mode == MODE_HOST) setSpreadingFactor();
        kiss_indicate_spreadingfactor();
      }
    } else if (command == CMD_CR) {
      if (sbyte == 0xFF) {
        kiss_indicate_codingrate();
      } else {
        int cr = sbyte;
        if (cr < 5) cr = 5;
        if (cr > 8) cr = 8;

        lora_cr = cr;
        if (op_mode == MODE_HOST) setCodingRate();
        kiss_indicate_codingrate();
      }
    } else if (command == CMD_IMPLICIT) {
      set_implicit_length(sbyte);
      kiss_indicate_implicit_length();
    } else if (command == CMD_LEAVE) {
      if (sbyte == 0xFF) {
        display_unblank();
        cable_state   = CABLE_STATE_DISCONNECTED;
        current_rssi  = -292;
        last_rssi     = -292;
        last_rssi_raw = 0x00;
        last_snr_raw  = 0x80;
      }
    } else if (command == CMD_RADIO_STATE) {
      if (bt_state != BT_STATE_CONNECTED) {
        cable_state = CABLE_STATE_CONNECTED;
        display_unblank();
      }
      if (sbyte == 0xFF) {
        kiss_indicate_radiostate();
      } else if (sbyte == 0x00) {
        stopRadio();
        kiss_indicate_radiostate();
      } else if (sbyte == 0x01) {
        startRadio();
        kiss_indicate_radiostate();
      }
    } else if (command == CMD_ST_ALOCK) {
      if (sbyte == FESC) {
            ESCAPE = true;
        } else {
            if (ESCAPE) {
                if (sbyte == TFEND) sbyte = FEND;
                if (sbyte == TFESC) sbyte = FESC;
                ESCAPE = false;
            }
            if (frame_len < CMD_L) cmdbuf[frame_len++] = sbyte;
        }

        if (frame_len == 2) {
          uint16_t at = (uint16_t)cmdbuf[0] << 8 | (uint16_t)cmdbuf[1];

          if (at == 0) {
            st_airtime_limit = 0.0;
          } else {
            st_airtime_limit = (float)at/(100.0*100.0);
            if (st_airtime_limit >= 1.0) { st_airtime_limit = 0.0; }
          }
          kiss_indicate_st_alock();
        }
    } else if (command == CMD_LT_ALOCK) {
      if (sbyte == FESC) {
            ESCAPE = true;
        } else {
            if (ESCAPE) {
                if (sbyte == TFEND) sbyte = FEND;
                if (sbyte == TFESC) sbyte = FESC;
                ESCAPE = false;
            }
            if (frame_len < CMD_L) cmdbuf[frame_len++] = sbyte;
        }

        if (frame_len == 2) {
          uint16_t at = (uint16_t)cmdbuf[0] << 8 | (uint16_t)cmdbuf[1];

          if (at == 0) {
            lt_airtime_limit = 0.0;
          } else {
            lt_airtime_limit = (float)at/(100.0*100.0);
            if (lt_airtime_limit >= 1.0) { lt_airtime_limit = 0.0; }
          }
          kiss_indicate_lt_alock();
        }
    } else if (command == CMD_STAT_RX) {
      kiss_indicate_stat_rx();
    } else if (command == CMD_STAT_TX) {
      kiss_indicate_stat_tx();
    } else if (command == CMD_STAT_RSSI) {
      kiss_indicate_stat_rssi();
    } else if (command == CMD_RADIO_LOCK) {
      update_radio_lock();
      kiss_indicate_radio_lock();
    } else if (command == CMD_BLINK) {
      led_indicate_info(sbyte);
    } else if (command == CMD_RANDOM) {
      kiss_indicate_random(getRandom());
    } else if (command == CMD_DETECT) {
      if (sbyte == DETECT_REQ) {
        if (bt_state != BT_STATE_CONNECTED) cable_state = CABLE_STATE_CONNECTED;
        kiss_indicate_detect();
      }
    } else if (command == CMD_PROMISC) {
      if (sbyte == 0x01) {
        promisc_enable();
      } else if (sbyte == 0x00) {
        promisc_disable();
      }
      kiss_indicate_promisc();
    } else if (command == CMD_READY) {
      if (!queue_full()) {
        kiss_indicate_ready();
      } else {
        kiss_indicate_not_ready();
      }
    } else if (command == CMD_UNLOCK_ROM) {
      if (sbyte == ROM_UNLOCK_BYTE) {
        unlock_rom();
      }
    } else if (command == CMD_RESET) {
      if (sbyte == CMD_RESET_BYTE) {
        hard_reset();
      }
    } else if (command == CMD_ROM_READ) {
      kiss_dump_eeprom();
    } else if (command == CMD_CFG_READ) {
      kiss_dump_config();
    } else if (command == CMD_ROM_WRITE) {
      if (sbyte == FESC) {
            ESCAPE = true;
        } else {
            if (ESCAPE) {
                if (sbyte == TFEND) sbyte = FEND;
                if (sbyte == TFESC) sbyte = FESC;
                ESCAPE = false;
            }
            if (frame_len < CMD_L) cmdbuf[frame_len++] = sbyte;
        }

        if (frame_len == 2) {
          eeprom_write(cmdbuf[0], cmdbuf[1]);
        }
    } else if (command == CMD_FW_VERSION) {
      kiss_indicate_version();
    } else if (command == CMD_PLATFORM) {
      kiss_indicate_platform();
    } else if (command == CMD_MCU) {
      kiss_indicate_mcu();
    } else if (command == CMD_BOARD) {
      kiss_indicate_board();
    } else if (command == CMD_CONF_SAVE) {
      eeprom_conf_save();
    } else if (command == CMD_CONF_DELETE) {
      eeprom_conf_delete();
    } else if (command == CMD_FB_EXT) {
      #if HAS_DISPLAY == true
        if (sbyte == 0xFF) {
          kiss_indicate_fbstate();
        } else if (sbyte == 0x00) {
          ext_fb_disable();
          kiss_indicate_fbstate();
        } else if (sbyte == 0x01) {
          ext_fb_enable();
          kiss_indicate_fbstate();
        }
      #endif
    } else if (command == CMD_FB_WRITE) {
      if (sbyte == FESC) {
            ESCAPE = true;
        } else {
            if (ESCAPE) {
                if (sbyte == TFEND) sbyte = FEND;
                if (sbyte == TFESC) sbyte = FESC;
                ESCAPE = false;
            }
            if (frame_len < CMD_L) cmdbuf[frame_len++] = sbyte;
        }
        #if HAS_DISPLAY
          if (frame_len == 9) {
            uint8_t line = cmdbuf[0];
            if (line > 63) line = 63;
            int fb_o = line*8; 
            memcpy(fb+fb_o, cmdbuf+1, 8);
          }
        #endif
    } else if (command == CMD_FB_READ) {
      if (sbyte != 0x00) { kiss_indicate_fb(); }
    } else if (command == CMD_DISP_READ) {
      if (sbyte != 0x00) { kiss_indicate_disp(); }
    } else if (command == CMD_DEV_HASH) {
      #if MCU_VARIANT == MCU_ESP32 || MCU_VARIANT == MCU_NRF52
        if (sbyte != 0x00) {
          kiss_indicate_device_hash();
        }
      #endif
    } else if (command == CMD_DEV_SIG) {
      #if MCU_VARIANT == MCU_ESP32 || MCU_VARIANT == MCU_NRF52
        if (sbyte == FESC) {
              ESCAPE = true;
          } else {
              if (ESCAPE) {
                  if (sbyte == TFEND) sbyte = FEND;
                  if (sbyte == TFESC) sbyte = FESC;
                  ESCAPE = false;
              }
              if (frame_len < CMD_L) cmdbuf[frame_len++] = sbyte;
          }

          if (frame_len == DEV_SIG_LEN) {
            memcpy(dev_sig, cmdbuf, DEV_SIG_LEN);
            device_save_signature();
          }
      #endif
    } else if (command == CMD_FW_UPD) {
      if (sbyte == 0x01) {
        firmware_update_mode = true;
      } else {
        firmware_update_mode = false;
      }
    } else if (command == CMD_HASHES) {
      #if MCU_VARIANT == MCU_ESP32 || MCU_VARIANT == MCU_NRF52
        if (sbyte == 0x01) {
          kiss_indicate_target_fw_hash();
        } else if (sbyte == 0x02) {
          kiss_indicate_fw_hash();
        } else if (sbyte == 0x03) {
          kiss_indicate_bootloader_hash();
        } else if (sbyte == 0x04) {
          kiss_indicate_partition_table_hash();
        }
      #endif
    } else if (command == CMD_FW_HASH) {
      #if MCU_VARIANT == MCU_ESP32 || MCU_VARIANT == MCU_NRF52
        if (sbyte == FESC) {
              ESCAPE = true;
          } else {
              if (ESCAPE) {
                  if (sbyte == TFEND) sbyte = FEND;
                  if (sbyte == TFESC) sbyte = FESC;
                  ESCAPE = false;
              }
              if (frame_len < CMD_L) cmdbuf[frame_len++] = sbyte;
          }

          if (frame_len == DEV_HASH_LEN) {
            memcpy(dev_firmware_hash_target, cmdbuf, DEV_HASH_LEN);
            device_save_firmware_hash();
          }
      #endif
    } else if (command == CMD_WIFI_CHN) {
      #if HAS_WIFI
        if (sbyte > 0 && sbyte < 14) { eeprom_update(eeprom_addr(ADDR_CONF_WCHN), sbyte); }
      #endif
    } else if (command == CMD_WIFI_MODE) {
      #if HAS_WIFI
        if (sbyte == WR_WIFI_OFF || sbyte == WR_WIFI_STA || sbyte == WR_WIFI_AP) {
          wr_conf_save(sbyte);
          wifi_mode = sbyte;
          wifi_remote_init();
        }
      #endif
    } else if (command == CMD_WIFI_SSID) {
      #if HAS_WIFI
        if (sbyte == FESC) { ESCAPE = true; }
        else {
          if (ESCAPE) {
            if (sbyte == TFEND) sbyte = FEND;
            if (sbyte == TFESC) sbyte = FESC;
            ESCAPE = false;
          }
          if (frame_len < CMD_L) cmdbuf[frame_len++] = sbyte;
        }

        if (sbyte == 0x00) {
          for (uint8_t i = 0; i<33; i++) {
            if (i<frame_len && i<32) { eeprom_update(config_addr(ADDR_CONF_SSID+i), cmdbuf[i]); }
            else                     { eeprom_update(config_addr(ADDR_CONF_SSID+i), 0x00); }
          }
        }
      #endif
    } else if (command == CMD_WIFI_PSK) {
      #if HAS_WIFI
        if (sbyte == FESC) { ESCAPE = true; }
        else {
          if (ESCAPE) {
            if (sbyte == TFEND) sbyte = FEND;
            if (sbyte == TFESC) sbyte = FESC;
            ESCAPE = false;
          }
          if (frame_len < CMD_L) cmdbuf[frame_len++] = sbyte;
        }

        if (sbyte == 0x00) {
          for (uint8_t i = 0; i<33; i++) {
            if (i<frame_len && i<32) { eeprom_update(config_addr(ADDR_CONF_PSK+i), cmdbuf[i]); }
            else                     { eeprom_update(config_addr(ADDR_CONF_PSK+i), 0x00); }
          }
        }
      #endif
    } else if (command == CMD_WIFI_IP) {
      #if HAS_WIFI
        if (sbyte == FESC) { ESCAPE = true; }
        else {
          if (ESCAPE) {
            if (sbyte == TFEND) sbyte = FEND;
            if (sbyte == TFESC) sbyte = FESC;
            ESCAPE = false;
          }
          if (frame_len < CMD_L) cmdbuf[frame_len++] = sbyte;
        }

        if (frame_len == 4) { for (uint8_t i = 0; i<4; i++) { eeprom_update(config_addr(ADDR_CONF_IP+i), cmdbuf[i]); } }
      #endif
    } else if (command == CMD_WIFI_NM) {
      #if HAS_WIFI
        if (sbyte == FESC) { ESCAPE = true; }
        else {
          if (ESCAPE) {
            if (sbyte == TFEND) sbyte = FEND;
            if (sbyte == TFESC) sbyte = FESC;
            ESCAPE = false;
          }
          if (frame_len < CMD_L) cmdbuf[frame_len++] = sbyte;
        }

        if (frame_len == 4) { for (uint8_t i = 0; i<4; i++) { eeprom_update(config_addr(ADDR_CONF_NM+i), cmdbuf[i]); } }
      #endif
    } else if (command == CMD_BT_CTRL) {
      #if HAS_BLUETOOTH || HAS_BLE
        if (sbyte == 0x00) {
          bt_stop();
          bt_conf_save(false);
        } else if (sbyte == 0x01) {
          bt_start();
          bt_conf_save(true);
        } else if (sbyte == 0x02) {
          if (bt_state == BT_STATE_OFF) {
            bt_start();
            bt_conf_save(true);
          }
          if (bt_state != BT_STATE_CONNECTED) {
            bt_enable_pairing();
          }
        }
      #endif
    } else if (command == CMD_BT_UNPAIR) {
      #if HAS_BLE
        if (sbyte == 0x01) { bt_debond_all(); }
      #endif
    } else if (command == CMD_DISP_INT) {
      #if HAS_DISPLAY
        if (sbyte == FESC) {
            ESCAPE = true;
        } else {
            if (ESCAPE) {
                if (sbyte == TFEND) sbyte = FEND;
                if (sbyte == TFESC) sbyte = FESC;
                ESCAPE = false;
            }
            display_intensity = sbyte;
            di_conf_save(display_intensity);
            display_unblank();
        }
      #endif
    } else if (command == CMD_DISP_ADDR) {
      #if HAS_DISPLAY
        if (sbyte == FESC) {
            ESCAPE = true;
        } else {
            if (ESCAPE) {
                if (sbyte == TFEND) sbyte = FEND;
                if (sbyte == TFESC) sbyte = FESC;
                ESCAPE = false;
            }
            display_addr = sbyte;
            da_conf_save(display_addr);
        }

      #endif
    } else if (command == CMD_DISP_BLNK) {
      #if HAS_DISPLAY
        if (sbyte == FESC) {
            ESCAPE = true;
        } else {
            if (ESCAPE) {
                if (sbyte == TFEND) sbyte = FEND;
                if (sbyte == TFESC) sbyte = FESC;
                ESCAPE = false;
            }
            db_conf_save(sbyte);
            display_unblank();
        }
      #endif
    } else if (command == CMD_DISP_ROT) {
      #if HAS_DISPLAY
        if (sbyte == FESC) {
            ESCAPE = true;
        } else {
            if (ESCAPE) {
                if (sbyte == TFEND) sbyte = FEND;
                if (sbyte == TFESC) sbyte = FESC;
                ESCAPE = false;
            }
            drot_conf_save(sbyte);
            display_unblank();
        }
      #endif
    } else if (command == CMD_DIS_IA) {
      if (sbyte == FESC) {
          ESCAPE = true;
      } else {
          if (ESCAPE) {
              if (sbyte == TFEND) sbyte = FEND;
              if (sbyte == TFESC) sbyte = FESC;
              ESCAPE = false;
          }
          dia_conf_save(sbyte);
      }
    } else if (command == CMD_DISP_RCND) {
      #if HAS_DISPLAY
        if (sbyte == FESC) {
            ESCAPE = true;
        } else {
            if (ESCAPE) {
                if (sbyte == TFEND) sbyte = FEND;
                if (sbyte == TFESC) sbyte = FESC;
                ESCAPE = false;
            }
            if (sbyte > 0x00) recondition_display = true;
        }
      #endif
    } else if (command == CMD_NP_INT) {
      #if HAS_NP
        if (sbyte == FESC) {
            ESCAPE = true;
        } else {
            if (ESCAPE) {
                if (sbyte == TFEND) sbyte = FEND;
                if (sbyte == TFESC) sbyte = FESC;
                ESCAPE = false;
            }
            sbyte;
            led_set_intensity(sbyte);
            np_int_conf_save(sbyte);
        }

      #endif
    }
  }
}

#if MCU_VARIANT == MCU_ESP32
  portMUX_TYPE update_lock = portMUX_INITIALIZER_UNLOCKED;
#endif

bool medium_free() {
  update_modem_status();
  if (avoid_interference && interference_detected) { return false; }
  return !dcd;
}

bool noise_floor_sampled = false;
int  noise_floor_sample  = 0;
int  noise_floor_buffer[NOISE_FLOOR_SAMPLES] = {0};
void update_noise_floor() {
  #if MCU_VARIANT == MCU_ESP32 || MCU_VARIANT == MCU_NRF52
    if (!dcd) {
      #if BOARD_MODEL != BOARD_HELTEC32_V4
      if (!noise_floor_sampled || current_rssi < noise_floor + CSMA_INFR_THRESHOLD_DB) {
      #else
      if ((!noise_floor_sampled || current_rssi < noise_floor + CSMA_INFR_THRESHOLD_DB) || (noise_floor_sampled && (noise_floor < LNA_GD_THRSHLD && current_rssi <= LNA_GD_LIMIT))) {
      #endif
        #if HAS_LORA_LNA
          // Discard invalid samples due to gain variance
          // during LoRa LNA re-calibration
          if (current_rssi < noise_floor-LORA_LNA_GVT) { return; }
        #endif
        bool sum_noise_floor = false;
        noise_floor_buffer[noise_floor_sample] = current_rssi;
        noise_floor_sample = noise_floor_sample+1;
        if (noise_floor_sample >= NOISE_FLOOR_SAMPLES) {
          noise_floor_sample %= NOISE_FLOOR_SAMPLES;
          noise_floor_sampled = true;
          sum_noise_floor = true;
        }

        if (noise_floor_sampled && sum_noise_floor) {
          noise_floor = 0;
          for (int ni = 0; ni < NOISE_FLOOR_SAMPLES; ni++) { noise_floor += noise_floor_buffer[ni]; }
          noise_floor /= NOISE_FLOOR_SAMPLES;
        }
      }
    }
  #endif
}

#define LED_ID_TRIG 16
uint8_t led_id_filter = 0;
uint32_t interference_start = 0;
bool interference_persists = false;
void update_modem_status() {
  #if MCU_VARIANT == MCU_ESP32
    portENTER_CRITICAL(&update_lock);
  #elif MCU_VARIANT == MCU_NRF52
    portENTER_CRITICAL();
  #endif

  bool carrier_detected = LoRa->dcd();
  current_rssi = LoRa->currentRssi();
  last_status_update = millis();

  #if MCU_VARIANT == MCU_ESP32
    portEXIT_CRITICAL(&update_lock);
  #elif MCU_VARIANT == MCU_NRF52
    portEXIT_CRITICAL();
  #endif

  #if BOARD_MODEL == BOARD_HELTEC32_V4
    if (noise_floor > LNA_GD_THRSHLD)  { interference_detected = !carrier_detected && (current_rssi > (noise_floor+CSMA_INFR_THRESHOLD_DB)); }
    else                               { interference_detected = !carrier_detected && (current_rssi > LNA_GD_LIMIT); }
  #else
    interference_detected = !carrier_detected && (current_rssi > (noise_floor+CSMA_INFR_THRESHOLD_DB));
  #endif

  if (interference_detected) { if (led_id_filter < LED_ID_TRIG) { led_id_filter += 1; } }
  else                       { if (led_id_filter > 0) {led_id_filter -= 1; } }

  // Handle potential false interference detection due to
  // LNA recalibration, antenna swap, moving into new RF
  // environment or similar.
  if (interference_detected && current_rssi < CSMA_RFENV_RECAL_LIMIT_DB) {
    if (!interference_persists) { interference_persists = true; interference_start = millis(); }
    else {
      if (millis()-interference_start >= CSMA_RFENV_RECAL_MS) { noise_floor_sampled = false; interference_persists = false; }
    }
  } else { interference_persists = false; }

  if (carrier_detected) { dcd = true; } else { dcd = false; }

  dcd_led = dcd;
  if (dcd_led) { led_rx_on(); }
  else {
    if (interference_detected) {
      if (led_id_filter >= LED_ID_TRIG && noise_floor_sampled) { led_id_on(); }
    } else {
      if (airtime_lock) { led_indicate_airtime_lock(); }
      else              { led_rx_off(); led_id_off(); }
    }
  }
}

void check_modem_status() {
  if (millis()-last_status_update >= status_interval_ms) {
    update_modem_status();
    update_noise_floor();

    #if MCU_VARIANT == MCU_ESP32 || MCU_VARIANT == MCU_NRF52
      util_samples[dcd_sample] = dcd;
      dcd_sample = (dcd_sample+1)%DCD_SAMPLES;
      if (dcd_sample % UTIL_UPDATE_INTERVAL == 0) {
        int util_count = 0;
        for (int ui = 0; ui < DCD_SAMPLES; ui++) {
          if (util_samples[ui]) util_count++;
        }
        local_channel_util = (float)util_count / (float)DCD_SAMPLES;
        total_channel_util = local_channel_util + airtime;
        if (total_channel_util > 1.0) total_channel_util = 1.0;

        int16_t cb = current_airtime_bin();
        uint16_t nb = cb+1; if (nb == AIRTIME_BINS) { nb = 0; }
        if (total_channel_util > longterm_bins[cb]) longterm_bins[cb] = total_channel_util;
        longterm_bins[nb] = 0.0;

        update_airtime();
      }
    #endif
  }
}

void validate_status() {
  #if MCU_VARIANT == MCU_1284P
      uint8_t boot_flags = OPTIBOOT_MCUSR;
      uint8_t F_POR = PORF;
      uint8_t F_BOR = BORF;
      uint8_t F_WDR = WDRF;
  #elif MCU_VARIANT == MCU_2560
      uint8_t boot_flags = OPTIBOOT_MCUSR;
      if (boot_flags == 0x00) boot_flags = 0x03;
      uint8_t F_POR = PORF;
      uint8_t F_BOR = BORF;
      uint8_t F_WDR = WDRF;
  #elif MCU_VARIANT == MCU_ESP32
      // TODO: Get ESP32 boot flags
      uint8_t boot_flags = 0x02;
      uint8_t F_POR = 0x00;
      uint8_t F_BOR = 0x00;
      uint8_t F_WDR = 0x01;
  #elif MCU_VARIANT == MCU_NRF52
      // TODO: Get NRF52 boot flags
      uint8_t boot_flags = 0x02;
      uint8_t F_POR = 0x00;
      uint8_t F_BOR = 0x00;
      uint8_t F_WDR = 0x01;
  #endif

  if (hw_ready || device_init_done) {
    hw_ready = false;
    Serial.write("Error, invalid hardware check state\r\n");
    #if HAS_DISPLAY
      if (disp_ready) {
        device_init_done = true;
        update_display();
      }
    #endif
    led_indicate_boot_error();
  }

  if (boot_flags & (1<<F_POR)) {
    boot_vector = START_FROM_POWERON;
  } else if (boot_flags & (1<<F_BOR)) {
    boot_vector = START_FROM_BROWNOUT;
  } else if (boot_flags & (1<<F_WDR)) {
    boot_vector = START_FROM_BOOTLOADER;
  } else {
      Serial.write("Error, indeterminate boot vector\r\n");
      #if HAS_DISPLAY
        if (disp_ready) {
          device_init_done = true;
          update_display();
        }
      #endif
      led_indicate_boot_error();
  }

  if (boot_vector == START_FROM_BOOTLOADER || boot_vector == START_FROM_POWERON) {
    if (eeprom_lock_set()) {
      if (eeprom_product_valid() && eeprom_model_valid() && eeprom_hwrev_valid()) {
        if (eeprom_checksum_valid()) {
          eeprom_ok = true;
          if (modem_installed) {
            #if PLATFORM == PLATFORM_ESP32 || PLATFORM == PLATFORM_NRF52
              if (device_init()) {
                hw_ready = true;
              } else {
                hw_ready = false;
              }
            #else
              hw_ready = true;
            #endif
          } else {
            hw_ready = false;
            Serial.write("No radio module found\r\n");
            #if HAS_DISPLAY
              if (disp_ready) {
                device_init_done = true;
                update_display();
              }
            #endif
          }
          
          if (hw_ready && eeprom_have_conf()) {
            eeprom_conf_load();
            op_mode = MODE_TNC;
            startRadio();
          }
        } else {
          hw_ready = false;
          Serial.write("Invalid EEPROM checksum\r\n");
          #if HAS_DISPLAY
            if (disp_ready) {
              device_init_done = true;
              update_display();
            }
          #endif
        }
      } else {
        hw_ready = false;
        Serial.write("Invalid EEPROM configuration\r\n");
        #if HAS_DISPLAY
          if (disp_ready) {
            device_init_done = true;
            update_display();
          }
        #endif
      }
    } else {
      hw_ready = false;
      Serial.write("Device unprovisioned, no device configuration found in EEPROM\r\n");
      #if HAS_DISPLAY
        if (disp_ready) {
          device_init_done = true;
          update_display();
        }
      #endif
    }
  } else {
    hw_ready = false;
    Serial.write("Error, incorrect boot vector\r\n");
    #if HAS_DISPLAY
      if (disp_ready) {
        device_init_done = true;
        update_display();
      }
    #endif
    led_indicate_boot_error();
  }
}

#if MCU_VARIANT == MCU_ESP32 || MCU_VARIANT == MCU_NRF52
  void update_csma_parameters() {
    int airtime_pct = (int)(airtime*100);
    int new_cw_band = cw_band;

    if (airtime_pct <= CSMA_BAND_1_MAX_AIRTIME) { new_cw_band = 1; }
    else {
      int at = airtime_pct + CSMA_BAND_1_MAX_AIRTIME;
      new_cw_band = map(at, CSMA_BAND_1_MAX_AIRTIME, CSMA_BAND_N_MIN_AIRTIME, 2, CSMA_CW_BANDS);
    }

    if (new_cw_band > CSMA_CW_BANDS) { new_cw_band = CSMA_CW_BANDS; }
    if (new_cw_band != cw_band) { 
      cw_band = (uint8_t)(new_cw_band);
      cw_min  = (cw_band-1) * CSMA_CW_PER_BAND_WINDOWS;
      cw_max  = (cw_band) * CSMA_CW_PER_BAND_WINDOWS - 1;
      kiss_indicate_csma_stats();
    }
  }
#endif

void tx_queue_handler() {
  if (!airtime_lock && queue_height > 0) {
    if (csma_cw == -1) {
      csma_cw = random(cw_min, cw_max);
      cw_wait_target = csma_cw * csma_slot_ms;
    }

    if (difs_wait_start == -1) {                                                  // DIFS wait not yet started
      if (medium_free()) { difs_wait_start = millis(); return; }                  // Set DIFS wait start time
      else               { return; } }                                            // Medium not yet free, continue waiting
    
    else {                                                                        // We are waiting for DIFS or CW to pass
      if (!medium_free()) { difs_wait_start = -1; cw_wait_start = -1; return; }   // Medium became occupied while in DIFS wait, restart waiting when free again
      else {                                                                      // Medium is free, so continue waiting
        if (millis() < difs_wait_start+difs_ms) { return; }                       // DIFS has not yet passed, continue waiting
        else {                                                                    // DIFS has passed, and we are now in CW wait
          if (cw_wait_start == -1) { cw_wait_start = millis(); return; }          // If we haven't started counting CW wait time, do it from now
          else {                                                                  // If we are already counting CW wait time, add it to the counter
            cw_wait_passed += millis()-cw_wait_start; cw_wait_start   = millis();
            if (cw_wait_passed < cw_wait_target) { return; }                      // Contention window wait time has not yet passed, continue waiting
            else {                                                                // Wait time has passed, flush the queue
              bool should_flush = !lora_limit_rate && !lora_guard_rate;
              if (should_flush) { flush_queue(); } else { pop_queue(); }
              cw_wait_passed = 0; csma_cw = -1; difs_wait_start = -1; }
          }
        }
      }
    }
  }
}

void work_while_waiting() { loop(); }

void loop() {

#if defined(HAS_LXMF_GATEWAY)
  // Flush any pending BLE-in burst counter after a 50ms idle gap so short
  // bursts (a single 227-byte LXMF message) surface as their own log line.
  if (ble_in_burst_bytes > 0 && (millis() - ble_in_last_byte_ms) > 50) {
    NOTICEF("BLE IN burst end: %lu bytes", (unsigned long)ble_in_burst_bytes);
    ble_in_burst_bytes = 0;
  }
  // Connection-state edge logging so BLE link drops show up explicitly.
  {
    static uint8_t prev_bt_state = 0xFF;
    if (bt_state != prev_bt_state) {
      NOTICEF("BLE state -> %u (was %u)", (unsigned)bt_state, (unsigned)prev_bt_state);
      prev_bt_state = bt_state;
    }
  }
#endif

#ifdef HAS_RNS
  // CBA
  if (reticulum) {
    // Take the rns_lock so the WebServer task (which also accesses
    // RNS state from its core-0 task) doesn't race against state
    // mutations inside reticulum.loop(). The lock is released as
    // soon as the loop returns so the WebServer task can run during
    // the radio/serial/display work that follows. The lock only
    // exists when the WebUI is compiled in — on builds without
    // HAS_LXMF_GATEWAY there's no second accessor and no lock.
#if defined(HAS_LXMF_GATEWAY)
    Web::WebUI::RnsLockGuard guard;
#endif
    try {
      reticulum.loop();
    }
    catch (const std::bad_alloc&) {
      ERROR("RNS loop failed: bad_alloc - out of memory");
    }
    catch (std::exception& e) {
      ERRORF("RNS loop failed: %s", e.what());
    }
  }
  // After a long reticulum.loop() (e.g. mid-Resource assembly that
  // ran SHA + decrypt + flash writes), reset the task watchdog so we
  // don't reboot just because one tick processed a lot. The lock is
  // released here so the web_task gets a window before we re-enter
  // RNS work below.
  esp_task_wdt_reset();
#endif

  // Drain whatever NMEA the GPS module shoved at us this
  // tick. Cheap if no bytes pending. No rns_lock needed — the GPS
  // parser only touches its own static state and TimeManager (whose
  // adopt path is reentrant-safe).
#if BOARD_MODEL == BOARD_TBEAM_S_V1 || BOARD_MODEL == BOARD_TBEAM_S_LR_V1
  Web::Gps::pump();
  // NTP — cheap when no transition; checks SNTP sync status
  // and forwards to TimeManager when a fresh epoch lands. Gated on
  // WiFi STA connection internally.
  Web::Ntp::pump();
  // BME280 — periodic temp/humidity/pressure poll, gated by
  // the driver's own interval. No-op if the chip wasn't detected.
  Web::Bme280::pump();
  // QMC6310 magnetometer + QMI8658 IMU — same pattern.
  Web::QmcMag::pump();
  Web::QmiImu::pump();
#endif

  if (radio_online) {
    #if MCU_VARIANT == MCU_ESP32
      LoRa->handleDio0IfPending();
      modem_packet_t *modem_packet = NULL;
      if(modem_packet_queue && xQueueReceive(modem_packet_queue, &modem_packet, 0) == pdTRUE && modem_packet) {
        host_write_len = modem_packet->len;
        last_rssi      = modem_packet->rssi;
        last_snr_raw   = modem_packet->snr_raw;
        memcpy(&pbuf, modem_packet->data, modem_packet->len);
        free(modem_packet);
        modem_packet = NULL;

        kiss_indicate_stat_rssi();
        kiss_indicate_stat_snr();
        kiss_write_packet();
      }

      airtime_lock = false;
      if (st_airtime_limit != 0.0 && airtime >= st_airtime_limit) airtime_lock = true;
      if (lt_airtime_limit != 0.0 && longterm_airtime >= lt_airtime_limit) airtime_lock = true;

    #elif MCU_VARIANT == MCU_NRF52
      LoRa->handleDio0IfPending();
      modem_packet_t *modem_packet = NULL;
      if(modem_packet_queue && xQueueReceive(modem_packet_queue, &modem_packet, 0) == pdTRUE && modem_packet) {
        memcpy(&pbuf, modem_packet->data, modem_packet->len);
        host_write_len = modem_packet->len;
        free(modem_packet);
        modem_packet = NULL;

        portENTER_CRITICAL();
        last_rssi = LoRa->packetRssi();
        last_snr_raw = LoRa->packetSnrRaw();
        portEXIT_CRITICAL();
        kiss_indicate_stat_rssi();
        kiss_indicate_stat_snr();
        kiss_write_packet();
      }

      airtime_lock = false;
      if (st_airtime_limit != 0.0 && airtime >= st_airtime_limit) airtime_lock = true;
      if (lt_airtime_limit != 0.0 && longterm_airtime >= lt_airtime_limit) airtime_lock = true;

    #endif

    tx_queue_handler();
    check_modem_status();
  
  } else {
    if (hw_ready) {
      if (console_active) {
        #if HAS_CONSOLE
          console_loop();
        #endif
      } else {
        led_indicate_standby();
      }
    } else {

      led_indicate_not_ready();
      stopRadio();
    }
  }

  #if MCU_VARIANT == MCU_ESP32 || MCU_VARIANT == MCU_NRF52
      buffer_serial();
      if (!fifo_isempty(&serialFIFO)) serial_poll();
  #else
    if (!fifo_isempty_locked(&serialFIFO)) serial_poll();
  #endif

  #if HAS_DISPLAY && MCU_VARIANT != MCU_ESP32
    // ESP32 builds run the OLED refresh in display_refresh_task on
    // core 0 (see start_display_refresh_task above). Other MCUs keep
    // the inline main-loop refresh.
    if (disp_ready && !display_updating) update_display();
  #endif

  #if HAS_PMU
    if (pmu_ready) update_pmu();
  #endif

  #if HAS_BLUETOOTH || HAS_BLE == true
    if (!console_active && bt_ready) update_bt();
  #endif

  #if HAS_WIFI
    if (wifi_initialized) update_wifi();
    #if defined(HAS_LXMF_GATEWAY)
      // Sync the bootstrap-mode flag with the WiFi layer. Auto-fallback
      // and the user-driven /api/wifi/softap switch both flip
      // wr_runtime_softap in Remote.h; surface it to WebUI so the SPA
      // shows the bootstrap UI for the duration.
      if (wr_runtime_softap && !Web::WebUI::bootstrap_mode) {
        Web::WebUI::bootstrap_mode = true;
        NOTICE("WebUI: entered runtime softAP — bootstrap UI live");
      }
    #endif
    #if defined(TCP_TRANSPORT)
      TCPTransport::service();
    #endif
    #if defined(HAS_LXMF_GATEWAY)
      {
        // LXMFGateway::loop() reads / mutates the gateway state that the
        // WebServer task also touches via its handlers; lock around it.
        Web::WebUI::RnsLockGuard guard;
        LXMF::LXMFGateway::loop();
      }
      if (wifi_initialized) {
        Web::WebUI::start();        // idempotent — runs once after WiFi STA is up
        Web::WebUI::start_task();   // idempotent — spawns the WebServer FreeRTOS task once
        Web::WebUI::loop();         // periodic sweep; handleClient() runs in the task
      }
    #endif
  #endif

  #if HAS_INPUT
    input_read();
  #endif

  // Feed WDT
#if MCU_VARIANT == MCU_ESP32
  esp_task_wdt_reset();
#elif MCU_VARIANT == MCU_NRF52
  NRF_WDT->RR[0] = WDT_RR_RR_Reload;
#endif

  if (memory_low) {
    #if PLATFORM == PLATFORM_ESP32
      if (esp_get_free_heap_size() < 8192) {
        kiss_indicate_error(ERROR_MEMORY_LOW); memory_low = false;
      } else {
        memory_low = false;
      }
    #else
      kiss_indicate_error(ERROR_MEMORY_LOW); memory_low = false;
    #endif
  }
}

void sleep_now() {
  #if HAS_SLEEP == true
    stopRadio(); // TODO: Check this on all platforms
    #if PLATFORM == PLATFORM_ESP32
      #if BOARD_MODEL == BOARD_T3S3 || BOARD_MODEL == BOARD_XIAO_S3
        #if HAS_DISPLAY
          display_intensity = 0;
          update_display(true);
        #endif
      #endif
      #if BOARD_MODEL == BOARD_HELTEC32_V4
          digitalWrite(LORA_PA_CPS, LOW);
          digitalWrite(LORA_PA_CSD, LOW);
          digitalWrite(LORA_PA_PWR_EN, LOW);
          digitalWrite(Vext, HIGH);
      #endif
      #if PIN_DISP_SLEEP >= 0
        pinMode(PIN_DISP_SLEEP, OUTPUT);
        digitalWrite(PIN_DISP_SLEEP, DISP_SLEEP_LEVEL);
      #endif
      #if HAS_BLUETOOTH
        if (bt_state == BT_STATE_CONNECTED) {
          bt_stop();
          delay(100);
        }
      #endif
      esp_sleep_enable_ext0_wakeup(PIN_WAKEUP, WAKEUP_LEVEL);
      esp_deep_sleep_start();
    #elif PLATFORM == PLATFORM_NRF52
      #if BOARD_MODEL == BOARD_HELTEC_T114
        npset(0,0,0);
        digitalWrite(PIN_VEXT_EN, LOW);
        digitalWrite(PIN_T114_TFT_BLGT, HIGH);
        digitalWrite(PIN_T114_TFT_EN, HIGH);
      #elif BOARD_MODEL == BOARD_TECHO
        for (uint8_t i = display_intensity; i > 0; i--) { analogWrite(pin_backlight, i-1); delay(1); }
        epd_black(true); delay(300); epd_black(true); delay(300); epd_black(false);
        delay(2000);
        analogWrite(PIN_VEXT_EN, 0);
        delay(100);
      #endif
      sd_power_gpregret_set(0, 0x6d);
      nrf_gpio_cfg_sense_input(pin_btn_usr1, NRF_GPIO_PIN_PULLUP, NRF_GPIO_PIN_SENSE_LOW);
      NRF_POWER->SYSTEMOFF = 1;
    #endif
  #endif
}

// Long-short double-press gesture state for the LXMF identity-code path.
// A press in the 700ms..5000ms range arms the gesture for
// LXMF_GESTURE_WINDOW_MS; the next sub-700ms press while armed fires the
// identity-code request. Single short presses (no preceding long press)
// fall through to the existing BT toggle behaviour.
static unsigned long lxmf_long_press_armed_ms = 0;
#define LXMF_GESTURE_WINDOW_MS 2000UL

void button_event(uint8_t event, unsigned long duration) {
  #if MCU_VARIANT == MCU_ESP32 || MCU_VARIANT == MCU_NRF52
    // Unblank the display on any press, but don't otherwise consume the
    // press for action dispatch.
    bool was_blanked = display_blanked;
    if (was_blanked) display_unblank();

    if (duration > 10000) {
      #if HAS_CONSOLE
        #if HAS_BLUETOOTH || HAS_BLE
          bt_stop();
        #endif
        console_active = true;
        console_start();
      #endif
      lxmf_long_press_armed_ms = 0;
    } else if (duration > 5000) {
      #if HAS_BLUETOOTH || HAS_BLE
        if (bt_state != BT_STATE_CONNECTED) { bt_enable_pairing(); }
      #endif
      lxmf_long_press_armed_ms = 0;
    } else if (duration > 700) {
      #if HAS_SLEEP
        sleep_now();
      #endif
      #if defined(HAS_LXMF_GATEWAY)
        // Arm the long-short gesture. Next short press within
        // LXMF_GESTURE_WINDOW_MS fires the identity code request.
        lxmf_long_press_armed_ms = millis();
      #endif
    } else {
      #if defined(HAS_LXMF_GATEWAY)
        unsigned long now = millis();
        if (lxmf_long_press_armed_ms != 0 &&
            (now - lxmf_long_press_armed_ms) < LXMF_GESTURE_WINDOW_MS) {
          lxmf_long_press_armed_ms = 0;
          Web::WebUI::on_button_request_identity_code();
          // Gesture consumed — don't also toggle BT this press.
          return;
        }
      #endif
      #if HAS_BLUETOOTH || HAS_BLE
      if (!was_blanked && bt_state != BT_STATE_CONNECTED) {
        if (bt_state == BT_STATE_OFF) {
          bt_start();
          bt_conf_save(true);
        } else {
          bt_stop();
          bt_conf_save(false);
        }
      }
      #endif
    }
  #endif
}

volatile bool serial_polling = false;
void serial_poll() {
  serial_polling = true;

  #if MCU_VARIANT != MCU_ESP32 && MCU_VARIANT != MCU_NRF52
  while (!fifo_isempty_locked(&serialFIFO)) {
  #else
  while (!fifo_isempty(&serialFIFO)) {
  #endif
    char sbyte = fifo_pop(&serialFIFO);
    serial_callback(sbyte);
  }

  serial_polling = false;
}

#if MCU_VARIANT != MCU_ESP32
  #define MAX_CYCLES 20
#else
  #define MAX_CYCLES 10
#endif
void buffer_serial() {
  if (!serial_buffering) {
    serial_buffering = true;

    uint8_t c = 0;

    #if HAS_BLUETOOTH || HAS_BLE == true
    while (
      c < MAX_CYCLES &&
      #if HAS_WIFI
      ( (bt_state != BT_STATE_CONNECTED && Serial.available()) || (bt_state == BT_STATE_CONNECTED && SerialBT.available()) || (wr_state >= WR_STATE_ON && wifi_remote_available()) )
      #else
      ( (bt_state != BT_STATE_CONNECTED && Serial.available()) || (bt_state == BT_STATE_CONNECTED && SerialBT.available()) )
      #endif
      )
    #else
    while (c < MAX_CYCLES && Serial.available())
    #endif
    {
      c++;

      #if MCU_VARIANT != MCU_ESP32 && MCU_VARIANT != MCU_NRF52
        if (!fifo_isfull_locked(&serialFIFO)) { fifo_push_locked(&serialFIFO, Serial.read()); }
      #elif HAS_BLUETOOTH || HAS_BLE == true || HAS_WIFI
        if      (bt_state == BT_STATE_CONNECTED) { if (!fifo_isfull(&serialFIFO)) { fifo_push(&serialFIFO, SerialBT.read()); }
#if defined(HAS_LXMF_GATEWAY)
          {
            // Per-burst counter: log when 64 bytes accumulate, OR (in main loop)
            // when a burst ends (>50ms idle).  No time throttle on the count
            // itself, so big bursts are guaranteed to surface.
            extern uint32_t ble_in_burst_bytes;
            extern uint32_t ble_in_last_byte_ms;
            ble_in_burst_bytes++;
            ble_in_last_byte_ms = millis();
            if (ble_in_burst_bytes >= 64) {
              NOTICEF("BLE IN burst >=64 (total so far: %lu)", (unsigned long)ble_in_burst_bytes);
              ble_in_burst_bytes = 0;
            }
          }
#endif
        }
        #if HAS_WIFI
        else if (wifi_host_is_connected())       { if (!fifo_isfull(&serialFIFO)) { fifo_push(&serialFIFO, wifi_remote_read()); } }
        #endif
        else                                     { if (!fifo_isfull(&serialFIFO)) { fifo_push(&serialFIFO, Serial.read()); } }
      #else
        if (!fifo_isfull(&serialFIFO)) { fifo_push(&serialFIFO, Serial.read()); }
      #endif
    }

    serial_buffering = false;
  }
}

void serial_interrupt_init() {
  #if MCU_VARIANT == MCU_1284P
      TCCR3A = 0;
      TCCR3B = _BV(CS10) |
               _BV(WGM33)|
               _BV(WGM32);

      // Buffer incoming frames every 1ms
      ICR3 = 16000;
      TIMSK3 = _BV(ICIE3);

  #elif MCU_VARIANT == MCU_2560
      // TODO: This should probably be updated for
      // atmega2560 support. Might be source of
      // reported issues from snh.
      TCCR3A = 0;
      TCCR3B = _BV(CS10) |
               _BV(WGM33)|
               _BV(WGM32);

      // Buffer incoming frames every 1ms
      ICR3 = 16000;
      TIMSK3 = _BV(ICIE3);

  #elif MCU_VARIANT == MCU_ESP32
      // No interrupt-based polling on ESP32
  #endif

}

#if MCU_VARIANT == MCU_1284P || MCU_VARIANT == MCU_2560
  ISR(TIMER3_CAPT_vect) { buffer_serial(); }
#endif
