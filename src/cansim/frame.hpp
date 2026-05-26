#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace cansim {

/**
 * Биты на проводе: '0' = dominant, '1' = recessive.
 * Хранение в std::string удобно для арбитража (wired-AND по символам)
 * и для отображения в UI.
 */
using BitString = std::string;

/** Один CAN-кадр на прикладном уровне (без CRC/ACK/EOF). */
struct CanFrame {
  uint32_t id = 0;
  bool extended = false;
  bool rtr = false;
  uint8_t dlc = 0;
  std::vector<uint8_t> data;
};

/** Поле кадра, к которому относится бит unstuffed-потока. */
enum class FieldTag : uint8_t {
  SOF,
  ARBITRATION,
  CONTROL,
  DATA,
  CRC,
};

/** Полная раскладка кадра по битам. */
struct LaidFrame {
  CanFrame frame;
  BitString unstuffed;
  BitString stuffed;
  std::vector<FieldTag> tags;
  std::vector<size_t> stuff_positions;
  uint16_t crc = 0;
  size_t arbitration_begin = 0;
  size_t arbitration_end = 0;
};

/** Полный трейлер кадра: CRC-delim, ACK slot, ACK delim, EOF (7), IFS (3). */
struct FrameTrailer {
  BitString bits;
  size_t ack_slot_index = 0;
};

/** CAN CRC-15 по полиному 0x4599. */
uint16_t crc15(const BitString& unstuffed);

/** Bit stuffing: после 5 одинаковых битов вставляется бит противоположной полярности. */
BitString stuff_bits(const BitString& in, std::vector<size_t>* out_positions = nullptr);

/** Раскладывает CAN-кадр в полный битовый поток с CRC и stuffing. */
LaidFrame layout_frame(const CanFrame& f);

/** Возвращает трейлер: CRC delim + ACK slot + ACK delim + EOF + IFS. */
FrameTrailer make_trailer();

/** Самопроверка стэйта (вызывается из bus daemon при старте). */
bool frame_self_test();

}  // namespace cansim
