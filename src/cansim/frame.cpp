#include "frame.hpp"

#include <stdexcept>

namespace cansim {

namespace {

constexpr char kDom = '0';
constexpr char kRec = '1';

void push_bit(LaidFrame& lf, uint8_t bit, FieldTag tag) {
  lf.unstuffed.push_back(bit ? kRec : kDom);
  lf.tags.push_back(tag);
}

void push_bits(LaidFrame& lf, uint32_t value, int n_bits, FieldTag tag) {
  for (int i = n_bits - 1; i >= 0; --i) {
    push_bit(lf, (value >> i) & 1u, tag);
  }
}

}  // namespace

uint16_t crc15(const BitString& bits) {
  uint16_t crc = 0;
  for (char ch : bits) {
    const uint16_t in = (ch == kRec) ? 1 : 0;
    const uint16_t crcnext = ((crc >> 14) & 1u) ^ in;
    crc = static_cast<uint16_t>((crc << 1) & 0x7FFFu);
    if (crcnext) crc ^= 0x4599u;
  }
  return crc & 0x7FFFu;
}

BitString stuff_bits(const BitString& in, std::vector<size_t>* out_positions) {
  BitString out;
  out.reserve(in.size() + in.size() / 4 + 1);
  char last = 0;
  int run = 0;
  for (char b : in) {
    out.push_back(b);
    if (b == last) {
      ++run;
      if (run == 5) {
        const char stuff = (b == kDom) ? kRec : kDom;
        out.push_back(stuff);
        if (out_positions) out_positions->push_back(out.size() - 1);
        last = stuff;
        run = 1;
      }
    } else {
      last = b;
      run = 1;
    }
  }
  return out;
}

LaidFrame layout_frame(const CanFrame& f) {
  LaidFrame lf;
  lf.frame = f;
  if (lf.frame.dlc > 8) lf.frame.dlc = 8;
  if (lf.frame.data.size() > 8) lf.frame.data.resize(8);
  if (!lf.frame.rtr) {
    if (lf.frame.data.size() < lf.frame.dlc) lf.frame.data.resize(lf.frame.dlc, 0);
    if (lf.frame.dlc < lf.frame.data.size()) lf.frame.dlc = static_cast<uint8_t>(lf.frame.data.size());
  }

  push_bit(lf, 0, FieldTag::SOF);
  lf.arbitration_begin = lf.unstuffed.size();

  if (!lf.frame.extended) {
    if ((lf.frame.id & 0x7FFu) != lf.frame.id) {
      throw std::invalid_argument("Standard ID exceeds 11 bits");
    }
    push_bits(lf, lf.frame.id & 0x7FFu, 11, FieldTag::ARBITRATION);
    push_bit(lf, lf.frame.rtr ? 1 : 0, FieldTag::ARBITRATION);
    lf.arbitration_end = lf.unstuffed.size();
    push_bit(lf, 0, FieldTag::CONTROL);  // IDE = dominant
    push_bit(lf, 0, FieldTag::CONTROL);  // r0 reserved
  } else {
    if ((lf.frame.id & 0x1FFFFFFFu) != lf.frame.id) {
      throw std::invalid_argument("Extended ID exceeds 29 bits");
    }
    const uint32_t id_a = (lf.frame.id >> 18) & 0x7FFu;
    const uint32_t id_b = lf.frame.id & 0x3FFFFu;
    push_bits(lf, id_a, 11, FieldTag::ARBITRATION);
    push_bit(lf, 1, FieldTag::ARBITRATION);  // SRR recessive
    push_bit(lf, 1, FieldTag::ARBITRATION);  // IDE recessive
    push_bits(lf, id_b, 18, FieldTag::ARBITRATION);
    push_bit(lf, lf.frame.rtr ? 1 : 0, FieldTag::ARBITRATION);
    lf.arbitration_end = lf.unstuffed.size();
    push_bit(lf, 0, FieldTag::CONTROL);  // r1
    push_bit(lf, 0, FieldTag::CONTROL);  // r0
  }

  push_bits(lf, lf.frame.dlc & 0x0Fu, 4, FieldTag::CONTROL);
  for (uint8_t b : lf.frame.data) push_bits(lf, b, 8, FieldTag::DATA);

  lf.crc = crc15(lf.unstuffed);
  push_bits(lf, lf.crc, 15, FieldTag::CRC);

  lf.stuffed = stuff_bits(lf.unstuffed, &lf.stuff_positions);
  return lf;
}

FrameTrailer make_trailer() {
  FrameTrailer tr;
  tr.bits.push_back(kRec);  // CRC delimiter
  tr.ack_slot_index = tr.bits.size();
  tr.bits.push_back(kRec);  // ACK slot (TX drives recessive)
  tr.bits.push_back(kRec);  // ACK delimiter
  for (int i = 0; i < 7; ++i) tr.bits.push_back(kRec);  // EOF
  for (int i = 0; i < 3; ++i) tr.bits.push_back(kRec);  // IFS
  return tr;
}

bool frame_self_test() {
  CanFrame f;
  f.id = 0x123;
  f.dlc = 1;
  f.data = {0xAB};
  const auto lf = layout_frame(f);
  if (lf.unstuffed.empty() || lf.unstuffed.front() != kDom) return false;
  if (lf.arbitration_end <= lf.arbitration_begin) return false;

  // 5 одинаковых битов в подряд → должен быть вставлен stuff
  CanFrame s;
  s.id = 0;
  s.dlc = 1;
  s.data = {0x00};
  const auto ls = layout_frame(s);
  if (ls.stuff_positions.empty()) return false;
  if (ls.stuffed.size() <= ls.unstuffed.size()) return false;

  return true;
}

}  // namespace cansim
