#include "hdmi_cec.h"

#include "esphome/core/log.h"

namespace esphome {
namespace hdmi_cec {

static const char *const TAG = "hdmi_cec";
static const uint32_t START_BIT_MIN_US = 3500;
static const uint32_t HIGH_BIT_MIN_US = 400;
static const uint32_t HIGH_BIT_MAX_US = 800;

void HDMICEC::setup() {
  this->cec_isr_pin_ = this->cec_pin_->to_isr();
  this->recv_frame_buffer_.reserve(16); // max 16 bytes per CEC frame
  this->cec_pin_->pin_mode(gpio::FLAG_INPUT | gpio::FLAG_PULLUP);
  this->cec_pin_->attach_interrupt(HDMICEC::gpio_intr, this, gpio::INTERRUPT_ANY_EDGE);
}

void HDMICEC::dump_config() {
  ESP_LOGCONFIG(TAG, "HDMI-CEC");
  LOG_PIN("  Pin: ", this->cec_pin_);
}

void HDMICEC::loop() {
  if (this->recv_queue_.empty()) {
    return;
  }

  auto frame = this->recv_queue_.front();
  this->recv_queue_.pop();
  ESP_LOGD(TAG, "got frame with %d bytes. first byte is %02x", frame.size(), frame[0]);
}

void IRAM_ATTR HDMICEC::gpio_intr(HDMICEC *self) {
  const uint32_t now = micros();
  const bool level = self->cec_isr_pin_.digital_read();

  // on falling edge, store current time as the start of the low pulse
  if (level == false) {
    self->last_falling_edge_us_ = now;
    return;
  }

  // otherwise, it's a rising edge, so it's time to process the pulse length
  if (self->last_falling_edge_us_ == 0) {
    return;
  }

  auto pulse_duration = (micros() - self->last_falling_edge_us_);
  self->last_falling_edge_us_ = 0;

  if (pulse_duration > START_BIT_MIN_US) {
    // start bit detected. reset everything and start receiving
    self->decoder_state_ = DecoderState::ReceivingByte;
    reset_state_variables(self);
    return;
  }

  bool value = (pulse_duration >= HIGH_BIT_MIN_US && pulse_duration <= HIGH_BIT_MAX_US);

  switch (self->decoder_state_) {
    case DecoderState::ReceivingByte: {
      // write bit to the current byte
      self->recv_byte_buffer_ = (self->recv_byte_buffer_ << 1) | (value & 0b1);

      self->recv_bit_counter_++;
      if (self->recv_bit_counter_ >= 8) {
        // if we reached eight bits, push the current byte to the frame buffer
        self->recv_frame_buffer_.push_back(self->recv_byte_buffer_);

        self->recv_bit_counter_ = 0;
        self->recv_byte_buffer_ = 0;

        self->decoder_state_ = DecoderState::WaitingForEOM;
      } else {
        self->decoder_state_ = DecoderState::ReceivingByte;
      }
      break;
    }

    case DecoderState::WaitingForEOM: {
      bool isEOM = (value == 1);
      if (isEOM) {
        // pass frame to app
        self->recv_queue_.push(self->recv_frame_buffer_);
        ESP_LOGD(TAG, "frame complete. first byte is %02x", self->recv_frame_buffer_[0]);
        reset_state_variables(self);
      }

      self->decoder_state_ = (
        isEOM
        ? DecoderState::WaitingForEOMAck
        : DecoderState::WaitingForAck
      );
      break;
    }

    case DecoderState::WaitingForAck: {
      self->decoder_state_ = DecoderState::ReceivingByte;
      break;
    }

    case DecoderState::WaitingForEOMAck: {
      self->decoder_state_ = DecoderState::Idle;
      break;
    }

    default: {
      self->decoder_state_ = DecoderState::ReceivingByte;
      reset_state_variables(self);
      break;
    }
  }
}

void IRAM_ATTR HDMICEC::reset_state_variables(HDMICEC *self) {
  self->recv_bit_counter_ = 0;
  self->recv_byte_buffer_ = 0x0;
  self->recv_frame_buffer_.clear();
  self->recv_frame_buffer_.reserve(16);
}

}
}
