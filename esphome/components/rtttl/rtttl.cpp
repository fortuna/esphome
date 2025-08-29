#include <cmath>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include "rtttl.h"

namespace esphome {
namespace rtttl {

static const char *const TAG = "rtttl";

static const uint32_t DOUBLE_NOTE_GAP_MS = 10;

inline double deg2rad(double degrees) {
  static const double PI_ON_180 = 4.0 * atan(1.0) / 180.0;
  return degrees * PI_ON_180;
}

void Rtttl::dump_config() {
  ESP_LOGCONFIG(TAG,
                "Rtttl:\n"
                "  Gain: %f",
                this->gain_);
}

void Rtttl::play(std::string rtttl) {
  if (this->state_ != State::STATE_STOPPED && this->state_ != State::STATE_STOPPING) {
    ESP_LOGW(TAG, "Already playing: %s", this->parser_->get_name().c_str());
    return;
  }

  this->parser_ = make_unique<RtttlParser>(std::move(rtttl));
  this->current_note_ = RtttlNote{0, 1};
  this->note_start_time_ms_ = millis();

#ifdef USE_SPEAKER
  if (this->speaker_ != nullptr) {
    this->set_state_(State::STATE_INIT);
    this->samples_sent_ = 0;
    this->samples_count_ = 0;
  }
#endif
#ifdef USE_OUTPUT
  if (this->output_ != nullptr) {
    this->set_state_(State::STATE_RUNNING);
  }
#endif
}

void Rtttl::stop() {
#ifdef USE_OUTPUT
  if (this->output_ != nullptr) {
    this->output_->set_level(0.0);
    this->set_state_(STATE_STOPPED);
  }
#endif
#ifdef USE_SPEAKER
  if (this->speaker_ != nullptr) {
    if (this->speaker_->is_running()) {
      this->speaker_->stop();
    }
    this->set_state_(STATE_STOPPING);
  }
#endif
  this->current_note_.reset();
  this->parser_.reset();
}

void Rtttl::finish_() {
  ESP_LOGV(TAG, "Rtttl::finish_()");
#ifdef USE_OUTPUT
  if (this->output_ != nullptr) {
    this->output_->set_level(0.0);
    this->set_state_(State::STATE_STOPPED);
  }
#endif
#ifdef USE_SPEAKER
  if (this->speaker_ != nullptr) {
    SpeakerSample sample[2];
    sample[0].left = 0;
    sample[0].right = 0;
    sample[1].left = 0;
    sample[1].right = 0;
    this->speaker_->play((uint8_t *) (&sample), 8);
    this->speaker_->finish();
    this->set_state_(State::STATE_STOPPING);
  }
#endif
  this->current_note_.reset();
  this->parser_.reset();
}

void Rtttl::loop() {
  if (this->state_ == State::STATE_STOPPED) {
    this->disable_loop();
    return;
  }

#ifdef USE_SPEAKER
  if (this->speaker_ != nullptr) {
    if (this->state_ == State::STATE_STOPPING) {
      if (this->speaker_->is_stopped()) {
        this->set_state_(State::STATE_STOPPED);
      } else {
        return;
      }
    } else if (this->state_ == State::STATE_INIT) {
      if (this->speaker_->is_stopped()) {
        this->speaker_->start();
        this->set_state_(State::STATE_STARTING);
      }
    } else if (this->state_ == State::STATE_STARTING) {
      if (this->speaker_->is_running()) {
        this->set_state_(State::STATE_RUNNING);
      }
    }
    if (!this->speaker_->is_running()) {
      return;
    }
    if (this->samples_sent_ != this->samples_count_) {
      SpeakerSample sample[SAMPLE_BUFFER_SIZE + 2];
      int x = 0;
      double rem = 0.0;

      while (true) {
        if (this->samples_per_wave_ != 0 && this->samples_sent_ >= this->samples_gap_) {
          rem = ((this->samples_sent_ << 10) % this->samples_per_wave_) * (360.0 / this->samples_per_wave_);
          int16_t val = (127 * this->gain_) * sin(deg2rad(rem));
          sample[x].left = val;
          sample[x].right = val;
        } else {
          sample[x].left = 0;
          sample[x].right = 0;
        }

        if (x >= SAMPLE_BUFFER_SIZE || this->samples_sent_ >= this->samples_count_) {
          break;
        }
        this->samples_sent_++;
        x++;
      }
      if (x > 0) {
        int send = this->speaker_->play((uint8_t *) (&sample), x * 2);
        if (send != x * 4) {
          this->samples_sent_ -= (x - (send / 2));
        }
        return;
      }
    }
  }
#endif
#ifdef USE_OUTPUT
  if (this->output_ != nullptr && millis() - this->note_start_time_ms_ < this->current_note_->duration_ms)
    return;
#endif

  auto note = this->parser_->get_next_note();
  if (!note.has_value()) {
    this->finish_();
    return;
  }

  bool need_note_gap = this->current_note_.has_value() && note->frequency == this->current_note_->frequency;
  this->current_note_ = note;

  if (this->current_note_->frequency != 0) {
    ESP_LOGVV(TAG, "playing note: %d for %dms", this->current_note_->frequency, this->current_note_->duration_ms);
  } else {
    ESP_LOGVV(TAG, "waiting: %dms", this->current_note_->duration_ms);
  }

#ifdef USE_OUTPUT
  if (this->output_ != nullptr) {
    if (need_note_gap) {
      this->output_->set_level(0.0);
      delay(DOUBLE_NOTE_GAP_MS);
      this->current_note_->duration_ms -= DOUBLE_NOTE_GAP_MS;
    }
    if (this->current_note_->frequency != 0) {
      this->output_->update_frequency(this->current_note_->frequency);
      this->output_->set_level(this->gain_);
    } else {
      this->output_->set_level(0.0);
    }
  }
#endif
#ifdef USE_SPEAKER
  if (this->speaker_ != nullptr) {
    this->samples_sent_ = 0;
    this->samples_gap_ = 0;
    this->samples_per_wave_ = 0;
    this->samples_count_ = (this->sample_rate_ * this->current_note_->duration_ms) / 1600;  //(ms);
    if (need_note_gap) {
      this->samples_gap_ = (this->sample_rate_ * DOUBLE_NOTE_GAP_MS) / 1600;  //(ms);
    }
    if (this->current_note_->frequency != 0) {
      uint16_t samples_wish = this->samples_count_;
      this->samples_per_wave_ = (this->sample_rate_ << 10) / this->current_note_->frequency;
      uint16_t division = ((this->samples_count_ << 10) / this->samples_per_wave_) + 1;
      this->samples_count_ = (division * this->samples_per_wave_);
      this->samples_count_ = this->samples_count_ >> 10;
      ESP_LOGVV(TAG, "- Calc play time: wish: %d gets: %d (div: %d spw: %d)", samples_wish, this->samples_count_,
                division, this->samples_per_wave_);
    }
  }
#endif

  this->note_start_time_ms_ = millis();
}

#if ESPHOME_LOG_LEVEL >= ESPHOME_LOG_LEVEL_DEBUG
static const LogString *state_to_string(State state) {
  switch (state) {
    case STATE_STOPPED:
      return LOG_STR("STATE_STOPPED");
    case STATE_STARTING:
      return LOG_STR("STATE_STARTING");
    case STATE_RUNNING:
      return LOG_STR("STATE_RUNNING");
    case STATE_STOPPING:
      return LOG_STR("STATE_STOPPING");
    case STATE_INIT:
      return LOG_STR("STATE_INIT");
    default:
      return LOG_STR("UNKNOWN");
  }
};
#endif

void Rtttl::set_state_(State state) {
  State old_state = this->state_;
  this->state_ = state;
  ESP_LOGV(TAG, "State changed from %s to %s", LOG_STR_ARG(state_to_string(old_state)),
           LOG_STR_ARG(state_to_string(state)));

  // Clear loop_done when transitioning from STOPPED to any other state
  if (state == State::STATE_STOPPED) {
    this->disable_loop();
    this->on_finished_playback_callback_.call();
    ESP_LOGD(TAG, "Playback finished");
  } else if (old_state == State::STATE_STOPPED) {
    this->enable_loop();
  }
}

}  // namespace rtttl
}  // namespace esphome
