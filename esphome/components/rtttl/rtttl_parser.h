#pragma once

#include <string>
#include "esphome/core/optional.h"

namespace esphome {
namespace rtttl {

/**
 * @brief A struct to hold a single note from an RTTTL string.
 */
struct RtttlNote {
  uint16_t frequency;     ///< The frequency of the note in Hz. 0 for a pause.
  uint16_t duration_ms;  ///< The duration of the note in milliseconds.
};

/**
 * @brief A class to parse RTTTL strings.
 *
 * This class takes an RTTTL string and provides a method to get the next note
 * from the string. It handles parsing the RTTTL header and the note data.
 */
class RtttlParser {
 public:
  /**
   * @brief Construct a new RtttlParser object.
   *
   * @param rtttl The RTTTL string to parse.
   */
  explicit RtttlParser(std::string rtttl);
  const std::string &get_name() const { return this->name_; }

  /**
   * @brief Get the next note from the RTTTL string.
   *
   * This method parses the next note from the RTTTL string and returns it as an
   * `optional<RtttlNote>`. If there are no more notes to parse or if an error
   * occurs during parsing, it returns an empty optional.
   *
   * @return optional<RtttlNote> The next note, or an empty optional if parsing is finished.
   */
  optional<RtttlNote> get_next_note();

 private:
  /// The RTTTL string to parse.
  std::string rtttl_;
  /// The name of the song.
  std::string name_;
  /// The current position in the RTTTL string.
  size_t position_{0};
  /// The duration of a whole note in milliseconds.
  uint16_t wholenote_ms_;
  /// The default duration of a note (e.g. 4 for a quarter note).
  uint8_t default_duration_;
  /// The default octave for a note.
  uint8_t default_octave_;

  uint8_t get_integer_();
};

}  // namespace rtttl
}  // namespace esphome
