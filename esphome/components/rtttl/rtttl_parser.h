#pragma once

#include <string>
#include "esphome/core/optional.h"

namespace esphome {
namespace rtttl {

/**
 * @brief A struct to hold a single note from an RTTTL string.
 */
struct RTTTLNote {
  uint16_t frequency;  ///< The frequency of the note in Hz. 0 for a pause.
  uint16_t duration;   ///< The duration of the note in milliseconds.
};

/**
 * @brief A class to parse RTTTL strings.
 *
 * This class takes an RTTTL string and provides a method to get the next note
 * from the string. It handles parsing the RTTTL header and the note data.
 */
class RTTTLParser {
 public:
  /**
   * @brief Construct a new RTTTLParser object.
   *
   * @param rtttl The RTTTL string to parse.
   */
  explicit RTTTLParser(std::string rtttl);
  const std::string &get_name() const { return this->name_; }

  /**
   * @brief Get the next note from the RTTTL string.
   *
   * This method parses the next note from the RTTTL string and returns it as an
   * `optional<RTTTLNote>`. If there are no more notes to parse or if an error
   * occurs during parsing, it returns an empty optional.
   *
   * @return optional<RTTTLNote> The next note, or an empty optional if parsing is finished.
   */
  optional<RTTTLNote> get_next_note();

 protected:
  std::string rtttl_;
  std::string name_;
  size_t position_{0};
  uint16_t wholenote_;
  uint8_t default_duration_;
  uint8_t default_octave_;

  uint8_t get_integer_();
};

}  // namespace rtttl
}  // namespace esphome
