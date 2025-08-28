#include "rtttl_parser.h"
#include "esphome/core/log.h"

namespace esphome {
namespace rtttl {

static const char *const TAG = "rtttl_parser";

// These values can also be found as constants in the Tone library (Tone.h)
// Note frequencies for octaves 4-7
static const uint16_t NOTES[] = {
    0,    262,  277,  294,  311,  330,  349,  370,  392,  415,  440,  466,  494,
    523,  554,  587,  622,  659,  698,  740,  784,  831,  880,  932,  988,  1047,
    1109, 1175, 1245, 1319, 1397, 1480, 1568, 1661, 1760, 1865, 1976, 2093, 2217,
    2349, 2489, 2637, 2794, 2960, 3136, 3322, 3520, 3729, 3951};

RtttlParser::RtttlParser(std::string rtttl) : rtttl_(std::move(rtttl)) {
  this->default_duration_ = 4;
  this->default_octave_ = 6;
  int bpm = 63;
  uint8_t num;

  // Find the name
  this->position_ = this->rtttl_.find(':');
  if (this->position_ == std::string::npos || this->position_ > 15) {
    ESP_LOGE(TAG, "Unable to determine name; missing ':'");
    this->position_ = std::string::npos;
    return;
  }
  this->name_ = this->rtttl_.substr(0, this->position_);

  // Get default duration
  this->position_ = this->rtttl_.find("d=", this->position_);
  if (this->position_ == std::string::npos) {
    ESP_LOGE(TAG, "Missing 'd='");
    this->position_ = std::string::npos;
    return;
  }
  this->position_ += 2;
  num = this->get_integer_();
  if (num > 0)
    this->default_duration_ = num;

  // Get default octave
  this->position_ = this->rtttl_.find("o=", this->position_);
  if (this->position_ == std::string::npos) {
    ESP_LOGE(TAG, "Missing 'o='");
    this->position_ = std::string::npos;
    return;
  }
  this->position_ += 2;
  num = get_integer_();
  if (num >= 4 && num <= 7)
    this->default_octave_ = num;

  // Get BPM
  this->position_ = this->rtttl_.find("b=", this->position_);
  if (this->position_ == std::string::npos) {
    ESP_LOGE(TAG, "Missing 'b='");
    this->position_ = std::string::npos;
    return;
  }
  this->position_ += 2;
  num = get_integer_();
  if (num != 0)
    bpm = num;

  this->position_ = this->rtttl_.find(':', this->position_);
  if (this->position_ == std::string::npos) {
    ESP_LOGE(TAG, "Missing second ':'");
    this->position_ = std::string::npos;
    return;
  }
  this->position_++;

  this->wholenote_ms_ = (60 * 1000L / bpm) * 4;
}

optional<RtttlNote> RtttlParser::get_next_note() {
  if (this->position_ == std::string::npos || this->position_ >= this->rtttl_.length()) {
    return {};
  }

  // Skip commas and spaces
  while (this->rtttl_[this->position_] == ',' || this->rtttl_[this->position_] == ' ')
    this->position_++;

  if (this->position_ >= this->rtttl_.length()) {
    return {};
  }

  // Get note duration
  uint16_t duration;
  uint8_t num = this->get_integer_();
  if (num) {
    duration = this->wholenote_ms_ / num;
  } else {
    duration = this->wholenote_ms_ / this->default_duration_;
  }

  // Get note
  uint8_t note;
  switch (this->rtttl_[this->position_]) {
    case 'c':
      note = 1;
      break;
    case 'd':
      note = 3;
      break;
    case 'e':
      note = 5;
      break;
    case 'f':
      note = 6;
      break;
    case 'g':
      note = 8;
      break;
    case 'a':
      note = 10;
      break;
    case 'h':
    case 'b':
      note = 12;
      break;
    case 'p':
    default:
      note = 0;
  }
  this->position_++;

  // Sharp
  if (this->rtttl_[this->position_] == '#') {
    note++;
    this->position_++;
  }

  // Dotted note
  if (this->rtttl_[this->position_] == '.') {
    duration += duration / 2;
    this->position_++;
  }

  // Octave
  uint8_t scale = this->get_integer_();
  if (scale == 0)
    scale = this->default_octave_;

  if (scale < 4 || scale > 7) {
    ESP_LOGE(TAG, "Octave must be between 4 and 7 (it is %d)", scale);
    this->position_ = std::string::npos;
    return {};
  }

  uint16_t frequency = 0;
  if (note) {
    auto note_index = (scale - 4) * 12 + note;
    if (note_index >= sizeof(NOTES) / sizeof(NOTES[0])) {
      ESP_LOGE(TAG, "Note out of range");
      this->position_ = std::string::npos;
      return {};
    }
    frequency = NOTES[note_index];
  }

  return RtttlNote{frequency, duration};
}

uint8_t RtttlParser::get_integer_() {
  uint8_t ret = 0;
  while (this->position_ < this->rtttl_.length() && isdigit(this->rtttl_[this->position_])) {
    ret = (ret * 10) + (this->rtttl_[this->position_++] - '0');
  }
  return ret;
}

}  // namespace rtttl
}  // namespace esphome
