#pragma once

#include <seastar/core/fstream.hh>
#include <seastar/core/semaphore.hh>
#include <seastar/core/sstring.hh>
#include <seastar/core/timer.hh>
#include <seastar/util/noncopyable_function.hh>
#include <smf/time_utils.h>

#include "wal_generated.h"
#include "wal_opts.h"
#include "wal_requests.h"
#include "wal_segment.h"
#include "wal_writer_utils.h"

namespace v {
struct wal_writer_node_opts {
  /// \brief callback that users can opt-in
  /// gets called every time we roll a log segment
  ///
  using notify_create_log_handle_t =
    seastar::noncopyable_function<seastar::future<>(seastar::sstring)>;

  /// \brief callback that users can opt-in to listen to
  /// the name of the file being written to and the latest size flushed
  /// to disk. This callback is only triggered *AFTER* flush() succeeds
  ///
  using notify_size_change_handle_t =
    seastar::noncopyable_function<seastar::future<>(seastar::sstring, int64_t)>;

  SMF_DISALLOW_COPY_AND_ASSIGN(wal_writer_node_opts);

  wal_writer_node_opts(const wal_opts &op,
                       const wal_topic_create_request *create_props,
                       const seastar::sstring &writer_dir,
                       const seastar::io_priority_class &prio,
                       notify_create_log_handle_t cfunc,
                       notify_size_change_handle_t sfunc)
    : wopts(op), tprops(THROW_IFNULL(create_props)),
      writer_directory(writer_dir), pclass(prio),
      log_segment_create_notify(std::move(cfunc)),
      log_segment_size_notify(std::move(sfunc)) {}

  wal_writer_node_opts(wal_writer_node_opts &&o) noexcept
    : wopts(o.wopts), tprops(o.tprops), writer_directory(o.writer_directory),
      pclass(o.pclass),
      log_segment_create_notify(std::move(o.log_segment_create_notify)),
      log_segment_size_notify(std::move(o.log_segment_size_notify)),
      epoch(o.epoch) {}

  const wal_opts &wopts;
  const wal_topic_create_request *tprops;
  const seastar::sstring &writer_directory;
  const seastar::io_priority_class &pclass;

  notify_create_log_handle_t log_segment_create_notify;
  notify_size_change_handle_t log_segment_size_notify;

  int64_t epoch = 0;
};

/// \brief - given a prefix and an epoch (monotinically increasing counter)
/// wal_writer_node will continue writing records in file_size multiples
/// which by default is 64MB. It will compress the buffers that are bigger
/// than min_compression_size
///
/// Note: the file closes happen concurrently, they simply get scheduled,
/// however, the fstream_.close() is called after the file has been flushed
/// to disk, so even during crash we are safe
///
class wal_writer_node {
 public:
  explicit wal_writer_node(wal_writer_node_opts opts);
  SMF_DISALLOW_COPY_AND_ASSIGN(wal_writer_node);

  /// \brief writes the projection to disk ensuring file size capacity
  seastar::future<std::unique_ptr<wal_write_reply>> append(wal_write_request);
  /// \brief flushes the file before closing
  seastar::future<> close();
  /// \brief opens the file w/ open_flags::rw | open_flags::create |
  ///                          open_flags::truncate
  /// the file should fail if it exists. It should not exist on disk, as
  /// we'll truncate them
  seastar::future<> open();

  seastar::sstring
  filename() const {
    if (!lease_) { return ""; }
    return lease_->filename;
  }
  ~wal_writer_node();

  int64_t
  space_left() const {
    return opts_.wopts.max_log_segment_size - current_size_;
  }
  int64_t
  current_offset() const {
    return opts_.epoch + current_size_;
  }

 private:
  seastar::future<> rotate_fstream();
  /// \brief 0-copy append to buffer
  /// https://github.com/apache/kafka/blob/fb21209b5ad30001eeace56b3c8ab060e0ceb021/core/src/main/scala/kafka/log/Log.scala
  /// do append has a similar logic as the kafka log.
  /// effectively just check if there is enough space, if not rotate and then
  /// write.
  seastar::future<> do_append(const wal_binary_record *f);
  seastar::future<> disk_write(const wal_binary_record *f);

 private:
  wal_writer_node_opts opts_;
  int64_t current_size_ = 0;
  // the lease has to be a lw_shared_ptr because the object
  // may go immediately out of existence, before we get a chance to close the
  // file it needs to exist in the background fiber that closes the
  // underlying file
  //
  seastar::lw_shared_ptr<wal_segment> lease_ = nullptr;
  seastar::semaphore serialize_writes_{1};
  seastar::timer<> flush_timeout_;
  bool is_closed_{false};
};

}  // namespace v