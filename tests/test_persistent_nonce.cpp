// Exercise the production header against byte writes and simulated power loss.
#include "persistent_nonce.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
unsigned checks = 0;
unsigned interrupted_transactions = 0;

#define CHECK(condition) do { \
  ++checks; \
  if (!(condition)) { \
    std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
    std::exit(1); \
  } \
} while (0)

struct PowerCut {};

struct Write {
  int address;
  uint8_t before;
  uint8_t requested;
};

struct FakeStorage {
  std::vector<uint8_t> bytes;
  std::vector<Write> writes;
  size_t cut_after_call = 0;
  size_t drop_call = 0;
  size_t corrupt_call = 0;
  unsigned waits = 0;

  explicit FakeStorage(size_t size = 512) : bytes(size, 0xFF) {}
  explicit FakeStorage(const std::vector<uint8_t>& saved) : bytes(saved) {}

  int length() const { return static_cast<int>(bytes.size()); }
  uint8_t read(int address) const { return bytes.at(static_cast<size_t>(address)); }
  void update(int address, uint8_t value) {
    uint8_t& cell = bytes.at(static_cast<size_t>(address));
    writes.push_back(Write{address, cell, value});
    const size_t call = writes.size();
    if (call != drop_call) cell = call == corrupt_call ? value ^ 0x80 : value;
    if (cut_after_call && call == cut_after_call) throw PowerCut{};
  }
  void wait() { ++waits; }
};

typedef PersistentNonceJournal<FakeStorage> Journal;
const uint64_t MAX_NONCE = std::numeric_limits<uint64_t>::max();

FakeStorage after_allocations(unsigned count) {
  FakeStorage store;
  Journal journal(store);
  CHECK(journal.begin());
  for (unsigned i = 1; i <= count; ++i) {
    uint64_t nonce = 0;
    CHECK(journal.allocate(nonce));
    CHECK(nonce == i);
  }
  return FakeStorage(store.bytes);
}

void consecutive_and_reboot() {
  FakeStorage store;
  Journal journal(store);
  CHECK(journal.begin());
  CHECK(store.writes.empty());
  for (uint64_t expected = 1; expected <= 100; ++expected) {
    uint64_t nonce = 0;
    CHECK(journal.allocate(nonce));
    CHECK(nonce == expected);
  }
  const std::vector<uint8_t> saved = store.bytes;
  for (unsigned boot = 0; boot < 8; ++boot) {
    FakeStorage reboot(saved);
    Journal rebooted(reboot);
    CHECK(rebooted.begin());
    CHECK(reboot.writes.empty());
    CHECK(reboot.bytes == saved);
  }
  FakeStorage reboot(saved);
  Journal rebooted(reboot);
  CHECK(rebooted.begin());
  uint64_t nonce = 0;
  CHECK(rebooted.allocate(nonce));
  CHECK(nonce == 101);

  // Reboot after every allocation for several full journal rotations.
  std::vector<uint8_t> persisted(512, 0xFF);
  for (uint64_t expected = 1; expected <= 128; ++expected) {
    FakeStorage each_boot(persisted);
    Journal once(each_boot);
    CHECK(once.begin());
    CHECK(each_boot.writes.empty());
    CHECK(once.allocate(nonce));
    CHECK(nonce == expected);
    persisted = each_boot.bytes;
  }
}

void cuts_after_every_byte(unsigned successful_allocations) {
  const FakeStorage original = after_allocations(successful_allocations);
  const uint64_t attempted_nonce = successful_allocations + 1;
  FakeStorage complete(original.bytes);
  Journal complete_journal(complete);
  CHECK(complete_journal.begin());
  uint64_t nonce = 0;
  CHECK(complete_journal.allocate(nonce));
  CHECK(nonce == attempted_nonce);
  CHECK(!complete.writes.empty());

  // Include a loss before the first write, and after every update() byte call.
  // Calls that EEPROM.update would skip are included as additional cut points.
  for (size_t cut = 0; cut <= complete.writes.size(); ++cut) {
    FakeStorage failing(original.bytes);
    Journal interrupted(failing);
    CHECK(interrupted.begin());
    if (cut) {
      failing.cut_after_call = cut;
      bool was_cut = false;
      try { interrupted.allocate(nonce); } catch (const PowerCut&) { was_cut = true; }
      CHECK(was_cut);
    }
    ++interrupted_transactions;
    FakeStorage reboot(failing.bytes);
    Journal recovered(reboot);
    const bool began = recovered.begin();
    CHECK(reboot.writes.empty());
    if (!successful_allocations && cut && cut < complete.writes.size()) {
      // A partially provisioned virgin EEPROM has no trusted prior nonce.
      CHECK(!began);
      const std::vector<uint8_t> rejected = reboot.bytes;
      CHECK(!recovered.allocate(nonce));
      CHECK(reboot.writes.empty());
      CHECK(reboot.bytes == rejected);
      continue;
    }
    CHECK(began);
    uint64_t resumed_nonce = 0;
    CHECK(recovered.allocate(resumed_nonce));
    CHECK(resumed_nonce > successful_allocations);
    CHECK(resumed_nonce == attempted_nonce || resumed_nonce == attempted_nonce + 1);
    if (!cut) CHECK(resumed_nonce == attempted_nonce);
    if (cut == complete.writes.size()) CHECK(resumed_nonce == attempted_nonce + 1);
    CHECK(recovered.allocate(nonce));
    CHECK(nonce == resumed_nonce + 1);
  }
}

void rejected_storage_and_overflow() {
  for (size_t size : {size_t(0), size_t(15), size_t(511)}) {
    FakeStorage store(size);
    Journal journal(store);
    CHECK(!journal.begin());
    uint64_t nonce = 0;
    CHECK(!journal.allocate(nonce));
    CHECK(store.writes.empty());
  }
  for (uint64_t initial : {uint64_t(0), MAX_NONCE}) {
    FakeStorage store;
    Journal journal(store);
    CHECK(!journal.begin(initial));
    uint64_t nonce = 0;
    CHECK(!journal.allocate(nonce));
    CHECK(store.writes.empty());
  }
  FakeStorage uninitialized;
  Journal not_begun(uninitialized);
  uint64_t nonce = 0;
  CHECK(!not_begun.allocate(nonce));
  CHECK(uninitialized.writes.empty());

  FakeStorage larger(600);
  Journal journal(larger);
  CHECK(journal.begin(1234567890123456789ULL));
  CHECK(journal.allocate(nonce));
  CHECK(nonce == 1234567890123456789ULL);
  for (size_t i = 512; i < larger.bytes.size(); ++i) CHECK(larger.bytes[i] == 0xFF);
  for (const Write& write : larger.writes) CHECK(write.address < 512);
  FakeStorage reboot(larger.bytes);
  Journal resumed(reboot);
  CHECK(resumed.begin());
  CHECK(resumed.allocate(nonce));
  CHECK(nonce == 1234567890123456790ULL);

  FakeStorage nearly_exhausted;
  Journal last(nearly_exhausted);
  CHECK(last.begin(MAX_NONCE - 1));
  CHECK(last.allocate(nonce));
  CHECK(nonce == MAX_NONCE - 1);
  const std::vector<uint8_t> exhausted_bytes = nearly_exhausted.bytes;
  const size_t written = nearly_exhausted.writes.size();
  CHECK(!last.allocate(nonce));
  CHECK(!last.allocate(nonce));
  CHECK(nearly_exhausted.writes.size() == written);
  CHECK(nearly_exhausted.bytes == exhausted_bytes);
  FakeStorage exhausted(exhausted_bytes);
  Journal exhausted_boot(exhausted);
  CHECK(exhausted_boot.begin());
  CHECK(!exhausted_boot.allocate(nonce));
  CHECK(exhausted.writes.empty());
  CHECK(exhausted.bytes == exhausted_bytes);
}

void corruption_fails_closed() {
  // Mutate committed records generated by the actual header, including old slots.
  const FakeStorage original = after_allocations(40);
  for (size_t slot = 0; slot < 32; ++slot) {
    CHECK(original.bytes[slot * 16] == 0xA5);
    for (size_t byte = 1; byte < 16; ++byte) {
      FakeStorage corrupt(original.bytes);
      corrupt.bytes[slot * 16 + byte] ^= 1;
      Journal journal(corrupt);
      CHECK(!journal.begin());
      uint64_t nonce = 0;
      CHECK(!journal.allocate(nonce));
      CHECK(corrupt.writes.empty());
    }
  }
  for (uint8_t marker : {uint8_t(0x01), uint8_t(0x5A), uint8_t(0xA4)}) {
    for (bool has_valid : {false, true}) {
      FakeStorage unknown = has_valid ? after_allocations(1) : FakeStorage();
      unknown.bytes[31 * 16] = marker;
      Journal journal(unknown);
      CHECK(!journal.begin());
      uint64_t nonce = 0;
      CHECK(!journal.allocate(nonce));
      CHECK(unknown.writes.empty());
    }
  }
  for (uint8_t marker : {uint8_t(0x00), uint8_t(0xFF), uint8_t(0xA5)}) {
    FakeStorage dirty;
    dirty.bytes[0] = marker;
    dirty.bytes[4] = 2;
    Journal journal(dirty);
    CHECK(!journal.begin());
    uint64_t nonce = 0;
    CHECK(!journal.allocate(nonce));
    CHECK(dirty.writes.empty());
  }
}

void write_failures_are_not_exposed() {
  // Drop or corrupt every changed write in a virgin transaction. The allocator
  // must detect readback failure and never give the caller an unrecorded nonce.
  for (unsigned prior : {0U, 1U, 32U}) {
    const FakeStorage original = after_allocations(prior);
    FakeStorage reference(original.bytes);
    Journal good(reference);
    CHECK(good.begin());
    uint64_t nonce = 0;
    CHECK(good.allocate(nonce));
    for (size_t index = 0; index < reference.writes.size(); ++index) {
      const Write& write = reference.writes[index];
      if (write.before == write.requested) continue;
      for (bool corrupt : {false, true}) {
        FakeStorage bad(original.bytes);
        Journal journal(bad);
        CHECK(journal.begin());
        if (corrupt) bad.corrupt_call = index + 1;
        else bad.drop_call = index + 1;
        CHECK(!journal.allocate(nonce));
        // The same failed journal must not later expose a nonce without reboot.
        const size_t calls = bad.writes.size();
        CHECK(!journal.allocate(nonce));
        CHECK(bad.writes.size() == calls);
        FakeStorage reboot(bad.bytes);
        Journal recovered(reboot);
        if (recovered.begin()) {
          if (!prior) {
            // A dropped initial invalidation is detected before body writes;
            // the still-erased EEPROM may safely retry its first allocation.
            for (uint8_t byte : bad.bytes) CHECK(byte == 0xFF);
          }
          CHECK(recovered.allocate(nonce));
          CHECK(nonce > prior);
        } else {
          CHECK(!recovered.allocate(nonce));
          CHECK(reboot.writes.empty());
        }
      }
    }
  }
}
} // namespace

int main() {
  consecutive_and_reboot();
  for (unsigned prior : {0U, 1U, 31U, 32U, 33U, 63U, 64U}) {
    cuts_after_every_byte(prior);
  }
  rejected_storage_and_overflow();
  corruption_fails_closed();
  write_failures_are_not_exposed();
  std::printf("PASS: %u persistent nonce checks; %u interrupted EEPROM transactions.\n",
              checks, interrupted_transactions);
}
