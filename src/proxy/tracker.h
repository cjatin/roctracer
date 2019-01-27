/******************************************************************************
Copyright (c) 2018 Advanced Micro Devices, Inc. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*******************************************************************************/

#ifndef SRC_CORE_TRACKER_H_
#define SRC_CORE_TRACKER_H_

#include <amd_hsa_signal.h>
#include <assert.h>
#include <hsa.h>
#include <hsa_ext_amd.h>

#include <atomic>
#include <list>
#include <mutex>

#include "util/hsa_rsrc_factory.h"
#include "util/exception.h"
#include "util/logger.h"

namespace proxy {
// Dispatch record
typedef struct {
  uint64_t dispatch;                                   // dispatch timestamp, ns
  uint64_t begin;                                      // kernel begin timestamp, ns
  uint64_t end;                                        // kernel end timestamp, ns
  uint64_t complete;                                   // completion signal timestamp, ns
} async_record_t;

class Tracker {
  public:
  typedef std::mutex mutex_t;
  typedef util::HsaRsrcFactory::timestamp_t timestamp_t;
  typedef async_record_t record_t;
  struct entry_t;
  typedef std::list<entry_t*> sig_list_t;
  typedef sig_list_t::iterator sig_list_it_t;
  typedef uint64_t counter_t;

  struct entry_t {
    counter_t index;
    std::atomic<bool> valid;
    Tracker* tracker;
    sig_list_t::iterator it;
    hsa_agent_t agent;
    hsa_signal_t orig;
    hsa_signal_t signal;
    record_t* record;
    std::atomic<void*> handler;
    void* arg;
    bool is_memcopy;
  };

  static Tracker* Create() {
    std::lock_guard<mutex_t> lck(glob_mutex_);
    if (instance_ == NULL) instance_ = new Tracker;
    return instance_;
  }

  static Tracker& Instance() {
    if (instance_ == NULL) instance_ = Create();
    return *instance_;
  }

  static void Destroy() {
    std::lock_guard<mutex_t> lck(glob_mutex_);
    if (instance_ != NULL) delete instance_;
    instance_ = NULL;
  }

  // Add tracker entry
  entry_t* Alloc(const hsa_agent_t& agent, const hsa_signal_t& orig) {
    hsa_status_t status = HSA_STATUS_ERROR;

    // Creating a new tracker entry
    entry_t* entry = new entry_t{};
    assert(entry);
    entry->tracker = this;
    entry->agent = agent;
    entry->orig = orig;

    // Creating a record with the dispatch timestamps
    record_t* record = new record_t{};
    assert(record);
    record->dispatch = hsa_rsrc_->TimestampNs();
    entry->record = record;

    // Creating a proxy signal
    status = hsa_signal_create(1, 0, NULL, &(entry->signal));
    if (status != HSA_STATUS_SUCCESS) EXC_RAISING(status, "hsa_signal_create");
    status = hsa_amd_signal_async_handler(entry->signal, HSA_SIGNAL_CONDITION_LT, 1, Handler, entry);
    if (status != HSA_STATUS_SUCCESS) EXC_RAISING(status, "hsa_amd_signal_async_handler");

    // Adding antry to the list
    mutex_.lock();
    entry->it = sig_list_.insert(sig_list_.end(), entry);
    entry->index = counter_++;
    mutex_.unlock();

    return entry;
  }

  // Delete tracker entry
  void Delete(entry_t* entry) {
    hsa_signal_destroy(entry->signal);
    mutex_.lock();
    sig_list_.erase(entry->it);
    mutex_.unlock();
    delete entry;
  }

  // Enable tracker entry
  void Enable(entry_t* entry, void* handler, void* arg) {
    // Set entry handler and release the entry
    entry->arg = arg;
    entry->handler.store(handler, std::memory_order_release);

    // Debug trace
    if (trace_on_) {
      auto outstanding = outstanding_.fetch_add(1);
      fprintf(stdout, "Tracker::Add: entry %p, record %p, outst %lu\n", entry, entry->record, outstanding);
      fflush(stdout);
    }
  }

  void EnableDispatch(entry_t* entry, hsa_amd_signal_handler handler, void* arg) {
    entry->is_memcopy = false;
    Enable(entry, reinterpret_cast<void*>(handler), arg);
  }
  void EnableMemcopy(entry_t* entry, hsa_amd_signal_handler handler, void* arg) {
    entry->is_memcopy = true;
    Enable(entry, reinterpret_cast<void*>(handler), arg);
  }

  private:
  Tracker() :
    outstanding_(0),
    hsa_rsrc_(&(util::HsaRsrcFactory::Instance()))
  {}

  ~Tracker() {
    auto it = sig_list_.begin();
    auto end = sig_list_.end();
    while (it != end) {
      auto cur = it++;
      hsa_rsrc_->SignalWait((*cur)->signal);
      Erase(cur);
    }
  }

  // Delete an entry by iterator
  void Erase(const sig_list_it_t& it) { Delete(*it); }

  // Entry completion
  inline void Complete(hsa_signal_value_t signal_value, entry_t* entry) {
    record_t* record = entry->record;

    // Debug trace
    if (trace_on_) {
      auto outstanding = outstanding_.fetch_sub(1);
      fprintf(stdout, "Tracker::Handler: entry %p, record %p, outst %lu\n", entry, entry->record, outstanding);
      fflush(stdout);
    }

    // Query begin/end and complete timestamps
    if (entry->is_memcopy) {
      hsa_amd_profiling_async_copy_time_t async_copy_time{};
      hsa_status_t status = hsa_amd_profiling_get_async_copy_time(entry->signal, &async_copy_time);
      if (status != HSA_STATUS_SUCCESS) EXC_RAISING(status, "hsa_amd_profiling_get_async_copy_time");
      record->begin = hsa_rsrc_->SysclockToNs(async_copy_time.start);
      record->end = hsa_rsrc_->SysclockToNs(async_copy_time.end);
    } else {
      hsa_amd_profiling_dispatch_time_t dispatch_time{};
      hsa_status_t status = hsa_amd_profiling_get_dispatch_time(entry->agent, entry->signal, &dispatch_time);
      if (status != HSA_STATUS_SUCCESS) EXC_RAISING(status, "hsa_amd_profiling_get_dispatch_time");
      record->begin = hsa_rsrc_->SysclockToNs(dispatch_time.start);
      record->end = hsa_rsrc_->SysclockToNs(dispatch_time.end);
    }

    record->complete = hsa_rsrc_->TimestampNs();
    entry->valid.store(true, std::memory_order_release);

    // Original intercepted signal completion
    hsa_signal_t orig = entry->orig;
    if (orig.handle) {
      amd_signal_t* orig_signal_ptr = reinterpret_cast<amd_signal_t*>(orig.handle);
      amd_signal_t* prof_signal_ptr = reinterpret_cast<amd_signal_t*>(entry->signal.handle);
      orig_signal_ptr->start_ts = prof_signal_ptr->start_ts;
      orig_signal_ptr->end_ts = prof_signal_ptr->end_ts;

      const hsa_signal_value_t new_value = hsa_signal_load_relaxed(orig) - 1;
      if (signal_value != new_value) EXC_ABORT(HSA_STATUS_ERROR, "Tracker::Complete bad signal value");
      hsa_signal_store_screlease(orig, signal_value);
    }
  }

  inline static void HandleEntry(hsa_signal_value_t signal_value, entry_t* entry) {
    // Call entry handler
    void* handler = static_cast<void*>(entry->handler);
    reinterpret_cast<hsa_amd_signal_handler>(handler)(signal_value, entry->arg);
    // Delete tracker entry
    entry->tracker->Delete(entry);
  }

  // Handler for packet completion
  static bool Handler(hsa_signal_value_t signal_value, void* arg) {
    // Acquire entry
    entry_t* entry = reinterpret_cast<entry_t*>(arg);
    volatile std::atomic<void*>* ptr = &entry->handler;
    while (ptr->load(std::memory_order_acquire) == NULL) sched_yield();

    // Complete entry
    Tracker* tracker = entry->tracker;
    tracker->Complete(signal_value, entry);

    if (ordering_enabled_ == false) {
      HandleEntry(signal_value, entry);
    } else {
      // Acquire last entry
      entry_t* back = tracker->sig_list_.back();
      volatile std::atomic<void*>* ptr = &back->handler;
      while (ptr->load(std::memory_order_acquire) == NULL) sched_yield();

      tracker->handler_mutex_.lock();
      sig_list_it_t it = tracker->sig_list_.begin();
      sig_list_it_t end = back->it;
      while (it != end) {
        entry = *(it++);
        if (entry->valid.load(std::memory_order_acquire)) {
          HandleEntry(signal_value, entry);
        } else {
          break;
        }
      }
      tracker->handler_mutex_.unlock();
    }

    return false;
  }

  // instance
  static Tracker* instance_;
  static mutex_t glob_mutex_;
  static counter_t counter_;

  // Tracked signals list
  sig_list_t sig_list_;
  // Inter-thread synchronization
  mutex_t mutex_;
  mutex_t handler_mutex_;
  // Outstanding dispatches
  std::atomic<uint64_t> outstanding_;
  // HSA resources factory
  util::HsaRsrcFactory* hsa_rsrc_;
  // Handling ordering enabled
  static const bool ordering_enabled_ = false;
  // Enable tracing
  static const bool trace_on_ = false;
};

} // namespace rocprofiler

#endif // SRC_CORE_TRACKER_H_