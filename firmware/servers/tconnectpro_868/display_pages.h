#pragma once

#include <stddef.h>
#include <stdint.h>

// Only the LCD view is paged; receiver state and alarms cover every device.
class SpdDisplayPages {
public:
  static constexpr size_t ROWS_PER_PAGE = 8;
  SpdDisplayPages(size_t totalRows, uint32_t intervalMs)
      : totalRows_(totalRows), intervalMs_(intervalMs) {}

  size_t pageCount() const { return (totalRows_ + ROWS_PER_PAGE - 1) / ROWS_PER_PAGE; }
  size_t pageIndex() const { return pageIndex_; }
  size_t firstRow() const { return pageIndex_ * ROWS_PER_PAGE; }
  size_t rowCount() const {
    const size_t remaining = totalRows_ - firstRow();
    return remaining < ROWS_PER_PAGE ? remaining : ROWS_PER_PAGE;
  }

  void restart(uint32_t now) {
    pageIndex_ = 0;
    pageStartedMs_ = now;
  }

  bool advance(uint32_t now, bool paused) {
    if (paused) {
      // Give the current page a fresh interval after the dialog closes.
      pageStartedMs_ = now;
      return false;
    }
    if (pageCount() <= 1 || (uint32_t)(now - pageStartedMs_) < intervalMs_) {
      return false;
    }
    pageIndex_ = (pageIndex_ + 1) % pageCount();
    pageStartedMs_ = now;
    return true;
  }

private:
  size_t totalRows_;
  uint32_t intervalMs_;
  size_t pageIndex_ = 0;
  uint32_t pageStartedMs_ = 0;
};
