#include "bus.hpp"

#include <algorithm>
#include <cstdlib>
#include <random>
#include <stdexcept>

namespace cansim {

namespace {

constexpr char kDom = '0';
constexpr char kRec = '1';

double random_unit() {
  static thread_local std::mt19937 rng{std::random_device{}()};
  static thread_local std::uniform_real_distribution<double> dist(0.0, 1.0);
  return dist(rng);
}

size_t random_index(size_t n) {
  static thread_local std::mt19937 rng{std::random_device{}()};
  std::uniform_int_distribution<size_t> dist(0, n - 1);
  return dist(rng);
}

}  // namespace

Bus::Bus() = default;

Controller& Bus::add_controller(const std::string& id) {
  std::lock_guard<std::mutex> lock(mu_);
  auto& slot = controllers_[id];
  if (!slot) slot = std::make_unique<Controller>(id);
  return *slot;
}

void Bus::remove_controller(const std::string& id) {
  std::lock_guard<std::mutex> lock(mu_);
  controllers_.erase(id);
}

Controller* Bus::find(const std::string& id) {
  auto it = controllers_.find(id);
  return it == controllers_.end() ? nullptr : it->second.get();
}

ControllerSnapshot Bus::snapshot_of(const Controller& c) const {
  ControllerSnapshot s;
  s.id = c.id();
  s.tec = c.tec();
  s.rec = c.rec();
  s.mode = to_string(c.state());
  s.stats = c.stats();
  s.pending = c.pending_count();
  return s;
}

std::vector<ControllerSnapshot> Bus::snapshots_locked() const {
  std::vector<ControllerSnapshot> out;
  out.reserve(controllers_.size());
  for (const auto& [id, c] : controllers_) out.push_back(snapshot_of(*c));
  return out;
}

std::vector<ControllerSnapshot> Bus::snapshots() const {
  std::lock_guard<std::mutex> lock(mu_);
  return snapshots_locked();
}

void Bus::notify_state_locked(const Controller& c) {
  const auto snap = snapshot_of(c);
  for (auto& cb : state_listeners_) cb(snap);
}

bool Bus::submit(const std::string& from, CanFrame f) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = controllers_.find(from);
  if (it == controllers_.end()) return false;
  if (it->second->state() == ErrorState::BusOff) return false;
  it->second->queue_tx(std::move(f));
  notify_state_locked(*it->second);
  return true;
}

std::vector<Bus::Contender> Bus::gather_contenders_locked() {
  std::vector<Contender> out;
  for (auto& [id, ctl] : controllers_) {
    if (ctl->state() == ErrorState::BusOff) continue;
    if (!ctl->has_pending_tx()) continue;
    Contender c;
    c.ctl = ctl.get();
    try {
      c.laid = layout_frame(ctl->peek_tx());
    } catch (const std::exception&) {
      // Невалидный фрейм — выкидываем из очереди как ошибку отправки.
      ctl->pop_tx();
      ctl->on_tx_error(8);
      continue;
    }
    out.push_back(std::move(c));
  }
  return out;
}

void Bus::run_arbitration(std::vector<Contender>& contenders,
                          DeliveredFrame& out) {
  if (contenders.empty()) return;
  if (contenders.size() == 1) {
    out.winner = contenders.front().ctl->id();
    return;
  }

  size_t max_end = 0;
  for (const auto& c : contenders) max_end = std::max(max_end, c.laid.arbitration_end);

  for (size_t bit = 0; bit < max_end; ++bit) {
    char bus_bit = kRec;
    int active_here = 0;
    for (const auto& c : contenders) {
      if (!c.active) continue;
      if (bit >= c.laid.arbitration_end) continue;
      if (c.laid.unstuffed[bit] == kDom) bus_bit = kDom;
      ++active_here;
    }
    if (active_here <= 1) break;

    ArbitrationStep step;
    step.bit_index = bit;
    step.bus_bit = bus_bit;

    for (auto& c : contenders) {
      if (!c.active) continue;
      if (bit >= c.laid.arbitration_end) continue;
      if (c.laid.unstuffed[bit] != bus_bit) {
        c.active = false;
        step.dropped.push_back(c.ctl->id());
        c.ctl->on_arbitration_lost();
        out.losers.push_back(c.ctl->id());
      }
    }
    if (!step.dropped.empty()) out.arbitration.push_back(std::move(step));

    int still_active = 0;
    for (const auto& c : contenders) if (c.active) ++still_active;
    if (still_active <= 1) break;
  }

  // Если осталось больше одного — это одинаковый ID/RTR/IDE на нескольких узлах
  // (нарушение проектирования сети). Выбираем одного «детерминированно» как
  // победителя, остальным начислим bit-error при дальнейшей передаче.
  std::vector<Contender*> still;
  for (auto& c : contenders) if (c.active) still.push_back(&c);
  if (still.empty()) {
    // не должно случиться, но на всякий случай
    return;
  }
  if (still.size() == 1) {
    out.winner = still.front()->ctl->id();
    return;
  }
  // collision: тот, кто первый зарегистрирован, считается «победителем»,
  // остальные получат bit-error при следующих битах фрейма
  out.winner = still.front()->ctl->id();
  for (size_t i = 1; i < still.size(); ++i) {
    still[i]->active = false;
    out.losers.push_back(still[i]->ctl->id());
    still[i]->ctl->on_tx_error(8);
    ErrorReport er;
    er.kind = "bit_error";
    er.detected_by = still[i]->ctl->id();
    er.at_bit = still[i]->laid.arbitration_end;
    out.errors.push_back(std::move(er));
  }
}

void Bus::simulate_transmission(Contender& winner,
                                std::vector<Contender>& receivers,
                                DeliveredFrame& out) {
  const LaidFrame& laid = winner.laid;
  out.frame = laid.frame;
  out.unstuffed = laid.unstuffed;
  out.stuffed = laid.stuffed;
  out.stuff_positions = laid.stuff_positions;
  out.arbitration_begin = laid.arbitration_begin;
  out.arbitration_end = laid.arbitration_end;
  out.crc = laid.crc;

  // Бит-ошибки в потоке (случайно), детектируются передатчиком как bit error.
  bool bit_error_in_data = false;
  if (bit_error_rate_ > 0 && !out.stuffed.empty()) {
    if (random_unit() < bit_error_rate_) {
      const size_t flip_at = random_index(out.stuffed.size());
      out.stuffed[flip_at] = (out.stuffed[flip_at] == kDom) ? kRec : kDom;
      ErrorReport er;
      er.kind = "bit_error";
      er.detected_by = winner.ctl->id();
      er.at_bit = flip_at;
      out.errors.push_back(std::move(er));
      bit_error_in_data = true;
    }
  }

  // Проверка stuff-rule: 6 одинаковых битов подряд в области стаффинга
  bool stuff_error = false;
  size_t stuff_run = 0;
  char last_bit = 0;
  for (size_t i = 0; i < out.stuffed.size(); ++i) {
    if (out.stuffed[i] == last_bit) {
      ++stuff_run;
      if (stuff_run >= 6) {
        ErrorReport er;
        er.kind = "stuff_error";
        er.at_bit = i;
        er.detected_by = receivers.empty() ? std::string("(none)") : receivers.front().ctl->id();
        out.errors.push_back(std::move(er));
        stuff_error = true;
        break;
      }
    } else {
      stuff_run = 1;
      last_bit = out.stuffed[i];
    }
  }

  // CRC: получатели де-стафят, пересчитывают и сравнивают. Если бит-ошибка
  // повредила нам unstuffed-данные — будет несовпадение CRC.
  bool crc_error = false;
  {
    BitString destuffed = out.unstuffed;
    // Восстановим, что увидели бы получатели: применим обратное destuffing
    // на out.stuffed.
    BitString rx_unstuffed;
    rx_unstuffed.reserve(out.stuffed.size());
    int run = 0;
    char prev = 0;
    for (size_t i = 0; i < out.stuffed.size(); ++i) {
      const char b = out.stuffed[i];
      if (b == prev) {
        ++run;
        if (run == 6) {
          crc_error = true;
          break;
        }
        if (run == 5) {
          rx_unstuffed.push_back(b);
          ++i;
          if (i >= out.stuffed.size()) break;
          prev = out.stuffed[i];
          run = 1;
          continue;
        }
      } else {
        prev = b;
        run = 1;
      }
      rx_unstuffed.push_back(b);
    }
    if (!crc_error) {
      // длина unstuffed должна быть >= 16 (минимум CRC+1)
      if (rx_unstuffed.size() < 16) {
        crc_error = true;
      } else {
        const BitString before_crc = rx_unstuffed.substr(0, rx_unstuffed.size() - 15);
        const BitString rx_crc_bits = rx_unstuffed.substr(rx_unstuffed.size() - 15);
        const uint16_t recomputed = crc15(before_crc);
        BitString expected;
        for (int b = 14; b >= 0; --b) expected.push_back(((recomputed >> b) & 1) ? kRec : kDom);
        if (expected != rx_crc_bits) crc_error = true;
      }
    }
  }
  if (crc_error) {
    ErrorReport er;
    er.kind = "crc_error";
    er.detected_by = receivers.empty() ? std::string("(none)") : receivers.front().ctl->id();
    er.at_bit = out.stuffed.size();
    out.errors.push_back(std::move(er));
  }

  // ACK: если есть хотя бы один не-bus-off получатель с ack_enabled и нет
  // CRC/stuff/bit-ошибок — ACK становится dominant.
  FrameTrailer trailer = make_trailer();
  bool got_ack = false;
  std::vector<std::string> acked_by;
  if (!stuff_error && !crc_error && !bit_error_in_data) {
    for (auto& r : receivers) {
      if (r.ctl == winner.ctl) continue;
      if (r.ctl->state() == ErrorState::BusOff) continue;
      if (!r.ctl->ack_enabled()) continue;
      got_ack = true;
      acked_by.push_back(r.ctl->id());
    }
  }
  if (got_ack) {
    trailer.bits[trailer.ack_slot_index] = kDom;
  } else {
    ErrorReport er;
    er.kind = "ack_error";
    er.detected_by = winner.ctl->id();
    er.at_bit = out.stuffed.size() + trailer.ack_slot_index;
    out.errors.push_back(std::move(er));
  }
  out.trailer = trailer.bits;
  out.ack = got_ack;
  out.acked_by = std::move(acked_by);

  out.ok = out.errors.empty();
}

void Bus::apply_outcome_locked(DeliveredFrame& out,
                               Contender& winner,
                               std::vector<Contender>& receivers) {
  if (out.ok) {
    winner.ctl->on_tx_success();
    winner.ctl->pop_tx();
    for (auto& r : receivers) {
      if (r.ctl == winner.ctl) continue;
      if (r.ctl->state() == ErrorState::BusOff) continue;
      r.ctl->on_rx_success();
      if (r.ctl->accepts_app_rx(out.frame.id)) {
        r.ctl->on_app_rx_match();
        out.app_receivers.push_back(r.ctl->id());
      }
    }
  } else {
    winner.ctl->on_tx_error(8);
    // Получатели, обнаружившие ошибку — REC +1
    for (auto& r : receivers) {
      if (r.ctl == winner.ctl) continue;
      if (r.ctl->state() == ErrorState::BusOff) continue;
      r.ctl->on_rx_error(1);
    }
    // Кадр остаётся в очереди → автоматический retransmit
    ++out.retries_for_winner;
  }

  for (auto& [id, c] : controllers_) {
    out.states.push_back(snapshot_of(*c));
  }
}

bool Bus::step() {
  std::vector<Contender> contenders;
  std::vector<Contender> all_receivers;
  DeliveredFrame out;
  {
    std::lock_guard<std::mutex> lock(mu_);
    contenders = gather_contenders_locked();
    if (contenders.empty()) return false;

    out.seq = ++seq_;
    out.bus_time_us = bus_time_us_;

    // Все контроллеры (включая участников арбитража) могут быть получателями
    for (auto& [id, ctl] : controllers_) {
      Contender r;
      r.ctl = ctl.get();
      r.active = true;
      all_receivers.push_back(std::move(r));
    }

    run_arbitration(contenders, out);

    // Winner Contender (по id) среди contenders
    Contender* winner = nullptr;
    for (auto& c : contenders) {
      if (c.ctl->id() == out.winner) { winner = &c; break; }
    }
    if (!winner) return false;

    simulate_transmission(*winner, all_receivers, out);
    apply_outcome_locked(out, *winner, all_receivers);

    // приблизительное время кадра: stuffed + trailer
    bus_time_us_ += static_cast<uint64_t>(out.stuffed.size() + out.trailer.size()) *
                    static_cast<uint64_t>(bit_time_us_);
  }

  for (auto& cb : frame_listeners_) cb(out);
  for (const auto& snap : out.states) {
    for (auto& cb : state_listeners_) cb(snap);
  }
  return true;
}

}  // namespace cansim
