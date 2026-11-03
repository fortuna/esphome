#include "sx126x.h"
#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome::sx126x {

ESPHOME_LOG_TAG(TAG, "sx126x");
static constexpr uint32_t RESET_DELAY_HIGH_US = 5000;
static constexpr uint32_t RESET_DELAY_LOW_US = 2000;
static constexpr uint32_t TRANSMIT_TIMEOUT_MS = 4000;

void SX126x::setup() {
  this->packet_.reserve(SX126X_MAX_PACKET_SIZE);
  // setup pins
  this->busy_pin_->setup();
  this->rst_pin_->setup();
  this->dio1_pin_->setup();
  if (this->dio1_pin_->is_internal()) {
    static_cast<InternalGPIOPin *>(this->dio1_pin_)
        ->attach_interrupt(&SX126x::gpio_intr, this, gpio::INTERRUPT_RISING_EDGE);
  }

  // start spi
  this->port_.setup();

  // configure rf
  if (!this->configure_() && !this->is_failed()) {
    this->mark_failed(LOG_STR("Radio configuration failed"));
  }
}

void SX126x::configure() {
  if (this->is_transmitting() || this->rx_dispatching_) {
    ESP_LOGW(TAG, "Radio operation in progress, configuration ignored");
    return;
  }
  this->configure_();
}

bool SX126x::can_change_mode_() {
  if (this->state_ == RadioState::RADIO_STATE_IDLE && !this->rx_dispatching_)
    return true;
  ESP_LOGW(TAG, "Radio unavailable, mode change ignored");
  return false;
}

bool SX126x::is_transmitting() const {
  return this->state_ == RadioState::RADIO_STATE_PREPARING_TX || this->state_ == RadioState::RADIO_STATE_TRANSMITTING ||
         this->state_ == RadioState::RADIO_STATE_RESTORING;
}

bool SX126x::configure_() {
  if (this->is_failed())
    return false;
  this->state_ = RadioState::RADIO_STATE_UNCONFIGURED;
  if (!this->requested_profile_.sync_valid) {
    this->status_set_warning(LOG_STR("Invalid sync value"));
    return false;
  }
  this->state_ = RadioState::RADIO_STATE_CONFIGURING;
  this->rst_pin_->digital_write(true);
  delayMicroseconds(RESET_DELAY_HIGH_US);
  this->rst_pin_->digital_write(false);
  delayMicroseconds(RESET_DELAY_LOW_US);
  this->rst_pin_->digital_write(true);
  delayMicroseconds(RESET_DELAY_HIGH_US);
  const auto result = this->commands_.configure(this->requested_profile_, this->version_);
  this->state_ = RadioState::RADIO_STATE_UNCONFIGURED;
  if (result == ConfigurationResult::CONFIGURATION_RESULT_UNSUPPORTED) {
    this->status_set_warning(LOG_STR("Unsupported chip version"));
    return false;
  }
  if (!this->command_result_(result == ConfigurationResult::CONFIGURATION_RESULT_OK))
    return false;
  this->active_profile_ = this->requested_profile_;
  this->state_ = RadioState::RADIO_STATE_IDLE;
  this->enable_loop();
  return true;
}

size_t SX126x::get_max_packet_size() { return this->active_profile_.max_packet_size(); }

SX126xError SX126x::transmit_packet(const uint8_t *data, size_t len) {
  class Completion : public SX126xTransmitListener {
   public:
    void on_transmit_complete(SX126xError result) override {
      this->result = result;
      this->done = true;
    }
    SX126xError result{SX126xError::NONE};
    bool done{false};
  } completion;
  const auto admitted = this->start_transmit_async(&completion, data, len, TRANSMIT_TIMEOUT_MS);
  if (admitted != SX126xError::NONE)
    return admitted;
  while (!completion.done) {
    App.feed_wdt();
    this->poll_transmit_(millis());
  }
  return completion.result;
}

SX126xError SX126x::start_transmit_async(SX126xTransmitListener *listener, const uint8_t *data, size_t len,
                                         uint32_t timeout_ms) {
  if (!this->is_ready() || this->state_ == RadioState::RADIO_STATE_UNCONFIGURED ||
      this->state_ == RadioState::RADIO_STATE_RECOVERY_REQUIRED)
    return SX126xError::SX126X_ERROR_NOT_READY;
  if (this->state_ != RadioState::RADIO_STATE_IDLE)
    return SX126xError::SX126X_ERROR_BUSY;
  if (listener == nullptr || data == nullptr || len == 0 || len > this->get_max_packet_size() ||
      (this->active_profile_.payload_length > 0 && len != this->active_profile_.payload_length) || timeout_ms == 0 ||
      timeout_ms > 262143)
    return SX126xError::INVALID_PARAMS;

  // Admission never reads a packet or invokes a callback: borrowed RX data stays stable.
  if (this->dio1_pin_->digital_read()) {
    uint16_t status;
    if (!this->command_result_(this->commands_.read_irq(status)))
      return SX126xError::SX126X_ERROR_BUSY_TIMEOUT;
    if (status & IRQ_RX_DONE) {
      this->enable_loop();
      return SX126xError::SX126X_ERROR_BUSY;
    }
  }
  this->state_ = RadioState::RADIO_STATE_PREPARING_TX;
  this->tx_ = {listener, 0, timeout_ms};
  const bool prepared =
      this->commands_.standby(STDBY_XOSC) && this->commands_.clear_irqs() &&
      (this->active_profile_.payload_length != 0 || this->commands_.set_packet_params(this->active_profile_, len)) &&
      this->commands_.write_fifo(data, len) && this->commands_.transmit(timeout_ms);
  if (!this->command_result_(prepared)) {
    // A failed start owes no callback, but may have changed the chip's mode.
    this->state_ = RadioState::RADIO_STATE_RESTORING;
    const bool restored = this->command_result_(this->restore_after_transmit_());
    this->state_ = restored ? RadioState::RADIO_STATE_IDLE : RadioState::RADIO_STATE_RECOVERY_REQUIRED;
    this->tx_ = {};
    if (restored)
      this->enable_loop();
    return SX126xError::SX126X_ERROR_BUSY_TIMEOUT;
  }
  this->tx_.started_ms = millis();
  this->state_ = RadioState::RADIO_STATE_TRANSMITTING;
  this->enable_loop();
  return SX126xError::NONE;
}

bool SX126x::restore_after_transmit_() {
  return this->commands_.standby(STDBY_XOSC) && this->commands_.clear_irqs() &&
         (this->active_profile_.payload_length != 0 ||
          this->commands_.set_packet_params(this->active_profile_, this->get_max_packet_size())) &&
         (this->active_profile_.rx_start ? this->commands_.receive() : this->commands_.sleep());
}

void SX126x::finish_transmit_(SX126xError result) {
  if (this->state_ != RadioState::RADIO_STATE_TRANSMITTING)
    return;
  this->state_ = RadioState::RADIO_STATE_RESTORING;
  const bool restored = this->command_result_(this->restore_after_transmit_());
  if (!restored)
    result = SX126xError::SX126X_ERROR_BUSY_TIMEOUT;
  auto *listener = this->tx_.listener;
  this->tx_ = {};
  this->state_ = restored ? RadioState::RADIO_STATE_IDLE : RadioState::RADIO_STATE_RECOVERY_REQUIRED;
  listener->on_transmit_complete(result);
}

bool SX126x::cancel_transmit_async(SX126xTransmitListener *listener) {
  if (this->state_ != RadioState::RADIO_STATE_TRANSMITTING || listener == nullptr || listener != this->tx_.listener)
    return false;
  this->finish_transmit_(SX126xError::SX126X_ERROR_CANCELLED);
  return true;
}

void SX126x::poll_transmit_(uint32_t now) {
  if (this->state_ != RadioState::RADIO_STATE_TRANSMITTING)
    return;
  if (this->dio1_pin_->digital_read()) {
    uint16_t status;
    if (!this->command_result_(this->commands_.read_irq(status)) ||
        !this->command_result_(this->commands_.clear_irqs(status))) {
      this->finish_transmit_(SX126xError::SX126X_ERROR_BUSY_TIMEOUT);
      return;
    }
    if (status & (IRQ_TX_DONE | IRQ_RX_TX_TIMEOUT)) {
      this->finish_transmit_((status & IRQ_RX_TX_TIMEOUT) ? SX126xError::TIMEOUT : SX126xError::NONE);
      return;
    }
  }
  // A cached loop tick can precede admission; signed subtraction also handles wraparound.
  if (static_cast<int32_t>(now - this->tx_.started_ms) >= static_cast<int32_t>(this->tx_.timeout_ms))
    this->finish_transmit_(SX126xError::TIMEOUT);
}

void SX126x::call_listeners_(const std::vector<uint8_t> &packet, float rssi, float snr) {
  RxDelivery delivery(this->rx_dispatching_);
  for (auto &listener : this->listeners_) {
    listener->on_packet(packet, rssi, snr);
  }
  this->packet_trigger_.trigger(packet, rssi, snr);
}

void SX126x::loop() {
  if (this->rx_dispatching_ || this->is_failed())
    return;
  if (this->state_ == RadioState::RADIO_STATE_TRANSMITTING) {
    this->poll_transmit_(App.get_loop_component_start_time());
    return;
  }
  if (this->state_ != RadioState::RADIO_STATE_IDLE)
    return;
  if (!this->dio1_pin_->digital_read()) {
    if (this->dio1_pin_->is_internal()) {
      this->disable_loop();
    }
    return;
  }

  uint16_t status;
  if (!this->command_result_(this->commands_.read_irq(status)) ||
      !this->command_result_(this->commands_.clear_irqs(status)))
    return;
  if ((status & IRQ_RX_DONE) && !(status & IRQ_CRC_ERROR)) {
    float rssi, snr;
    if (this->command_result_(this->commands_.read_packet(this->active_profile_, this->packet_, rssi, snr)))
      this->call_listeners_(this->packet_, rssi, snr);
  }
}

bool SX126x::command_result_(bool success) {
  if (success) {
    this->status_clear_warning();
  } else {
    this->status_set_warning(LOG_STR("Wait busy timeout"));
  }
  return success;
}

void SX126x::dump_config() {
  ESP_LOGCONFIG(TAG, "SX126x:");
  LOG_PIN("  CS Pin: ", this->cs_pin_);
  LOG_PIN("  BUSY Pin: ", this->busy_pin_);
  LOG_PIN("  RST Pin: ", this->rst_pin_);
  LOG_PIN("  DIO1 Pin: ", this->dio1_pin_);
  ESP_LOGCONFIG(TAG,
                "  HW Version: %15s\n"
                "  Frequency: %" PRIu32 " Hz\n"
                "  Bandwidth: %" PRIu32 " Hz\n"
                "  PA Power: %" PRId8 " dBm\n"
                "  PA Ramp: %" PRIu16 " us\n"
                "  Payload Length: %" PRIu32 "\n"
                "  CRC Enable: %s\n"
                "  Rx Start: %s",
                this->version_, this->active_profile_.frequency,
                Sx126xCommands::bandwidth_hz(this->active_profile_.bandwidth), this->active_profile_.pa_power,
                Sx126xCommands::ramp_us(this->active_profile_.pa_ramp), this->active_profile_.payload_length,
                TRUEFALSE(this->active_profile_.crc_enable), TRUEFALSE(this->active_profile_.rx_start));
  if (this->active_profile_.modulation == PACKET_TYPE_GFSK) {
    const char *shaping = "NONE";
    if (this->active_profile_.shaping == GAUSSIAN_BT_0_3) {
      shaping = "GAUSSIAN_BT_0_3";
    } else if (this->active_profile_.shaping == GAUSSIAN_BT_0_5) {
      shaping = "GAUSSIAN_BT_0_5";
    } else if (this->active_profile_.shaping == GAUSSIAN_BT_0_7) {
      shaping = "GAUSSIAN_BT_0_7";
    } else if (this->active_profile_.shaping == GAUSSIAN_BT_1_0) {
      shaping = "GAUSSIAN_BT_1_0";
    }
    ESP_LOGCONFIG(TAG,
                  "  Modulation: FSK\n"
                  "  Deviation: %" PRIu32 " Hz\n"
                  "  Shaping: %s\n"
                  "  Preamble Size: %" PRIu16 "\n"
                  "  Preamble Detect: %" PRIu16 "\n"
                  "  Bitrate: %" PRIu32 "b/s",
                  this->active_profile_.deviation, shaping, this->active_profile_.preamble_size,
                  this->active_profile_.preamble_detect, this->active_profile_.bitrate);
  } else if (this->active_profile_.modulation == PACKET_TYPE_LORA) {
    const char *cr = "4/8";
    if (this->active_profile_.coding_rate == LORA_CR_4_5) {
      cr = "4/5";
    } else if (this->active_profile_.coding_rate == LORA_CR_4_6) {
      cr = "4/6";
    } else if (this->active_profile_.coding_rate == LORA_CR_4_7) {
      cr = "4/7";
    }
    ESP_LOGCONFIG(TAG,
                  "  Modulation: LORA\n"
                  "  Spreading Factor: %" PRIu8 "\n"
                  "  Coding Rate: %s\n"
                  "  Preamble Size: %" PRIu16,
                  this->active_profile_.spreading_factor, cr, this->active_profile_.preamble_size);
  }
  if (this->active_profile_.sync_size != 0) {
    char hex_buf[17];  // 8 bytes max = 16 hex chars + null
    ESP_LOGCONFIG(TAG, "  Sync Value: 0x%s",
                  format_hex_to(hex_buf, this->active_profile_.sync_value.data(), this->active_profile_.sync_size));
  }
  ESP_LOGCONFIG(TAG, "  Whitening Enable: %s", TRUEFALSE(this->active_profile_.whitening_enable));
  if (this->active_profile_.whitening_enable) {
    ESP_LOGCONFIG(TAG, "  Whitening Initial: 0x%03x", this->active_profile_.whitening_initial);
  }
}

}  // namespace esphome::sx126x
