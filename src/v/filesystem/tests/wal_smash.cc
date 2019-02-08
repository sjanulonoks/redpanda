#include "wal_smash.h"

#include <smf/fbs_typed_buf.h>
#include <smf/native_type_utils.h>

#include "filesystem/wal_core_mapping.h"
#include "filesystem/wal_segment_record.h"
#include "hashing/jump_consistent_hash.h"
#include "hashing/xx.h"

// test only
#include "gen_create_topic_buf.h"

namespace v {

wal_smash::wal_smash(wal_smash_opts opt,
                     seastar::distributed<write_ahead_log> *w)
  : opts_(std::move(opt)), wal_(THROW_IFNULL(w)) {
  opts_.ns_id =
    v::xxhash_64(opts_.topic_namespace.c_str(), opts_.topic_namespace.size());
  opts_.topic_id = v::xxhash_64(opts_.topic.c_str(), opts_.topic.size());
  // TODO(agallego) - when create returns real offsets we should change
  // this logic
  for (int32_t partition = 0; partition < opts_.topic_partitions; ++partition) {
    partition_offsets_.emplace(partition,
                               offset_meta_idx{0, seastar::semaphore(1)});

    wal_nstpidx idx(opts_.ns_id, opts_.topic_id, partition);
    auto core = jump_consistent_hash(idx.id(), seastar::smp::count);
    core_to_partitions_[core].push_back(partition);
  }
}

seastar::future<>
wal_smash::stop() {
  return seastar::make_ready_future<>();
}

seastar::future<std::unique_ptr<wal_create_reply>>
wal_smash::create(std::vector<int32_t> partitions) {
  LOG_THROW_IF(partitions.empty(), "No partitions to create for this core");
  auto tbuf = smf::fbs_typed_buf<wal_topic_create_request>(gen_create_topic_buf(
    opts_.topic_namespace, opts_.topic, opts_.topic_partitions,
    opts_.topic_type, opts_.topic_props));

  wal_create_request create_req(tbuf.get(), seastar::engine().cpu_id(),
                                partitions);
  return seastar::do_with(std::move(tbuf), std::move(create_req),
                          [this](auto &t, auto &create_req) {
                            return wal_->local().create(std::move(create_req));
                          });
}

seastar::future<std::unique_ptr<wal_write_reply>>
wal_smash::write_one(int32_t partition) {
  LOG_THROW_IF(partition_offsets_.empty(), "Skipped call to create()");
  v::wal_put_requestT put;
  put.topic = opts_.topic_id;
  put.ns = opts_.ns_id;
  for (auto n = 0; n < opts_.write_batch_size; ++n) {
    auto k = rand_.next_alphanum(opts_.random_key_bytes);
    auto v = rand_.next_alphanum(opts_.random_val_bytes);
    auto ctype = (int32_t)v.size() >= opts_.record_compression_value_threshold
                   ? opts_.record_compression_type
                   : v::wal_compression_type::wal_compression_type_none;
    auto record = v::wal_segment_record::coalesce(k.data(), k.size(), v.data(),
                                                  v.size(), ctype);
    stats_.bytes_written += record->data.size();

    auto &puts = put.partition_puts;
    // OK to std::find usually small ~16
    auto it = std::find_if(puts.begin(), puts.end(), [partition](auto &tpl) {
      return partition == tpl->partition;
    });
    if (it == puts.end()) {
      auto ptr = std::make_unique<v::wal_put_partition_recordsT>();
      ptr->partition = partition;
      ptr->records.push_back(std::move(record));
      puts.push_back(std::move(ptr));
    } else {
      (*it)->records.push_back(std::move(record));
    }
  }  // end of forloop batch_size
  auto body = smf::native_table_as_buffer<wal_put_request>(put);
  auto tput = smf::fbs_typed_buf<wal_put_request>(std::move(body));
  auto v = wal_core_mapping::core_assignment(tput.get());
  return seastar::do_with(std::move(tput), std::move(v),
                          [this](auto &tput, auto &v) mutable {
                            stats_.writes++;
                            return wal_->local().append(std::move(*v.begin()));
                          });
}

seastar::future<std::unique_ptr<wal_read_reply>>
wal_smash::read_one(int32_t opartition) {
  LOG_THROW_IF(partition_offsets_.empty(), "Skipped call to create()");

  const int32_t partition =
    opartition <= -1
      ? jump_consistent_hash(stats_.reads++, partition_offsets_.size())
      : opartition;

  return seastar::with_semaphore(partition_offsets_[partition].lock, 1, [=] {
    wal_get_requestT get;
    get.topic = opts_.topic_id;
    get.ns = opts_.ns_id;
    get.partition = partition;
    get.offset = partition_offsets_[partition].offset;
    get.max_bytes = opts_.consumer_max_read_bytes;

    auto body = smf::native_table_as_buffer<wal_get_request>(get);
    auto tbuf = smf::fbs_typed_buf<wal_get_request>(std::move(body));
    auto req = wal_core_mapping::core_assignment(tbuf.get());
    return seastar::do_with(
      std::move(tbuf), std::move(req), [this](auto &tbuf, auto &req) mutable {
        return wal_->local().get(std::move(req)).then([this](auto r) {
          stats_.reads++;
          stats_.bytes_read += r->on_disk_size();
          auto partition = r->reply().partition;
          int64_t old_offset = partition_offsets_[partition].offset;
          int64_t new_offset = r->reply().next_offset;
          LOG_THROW_IF(old_offset > new_offset,
                       "Reply had an earlier offset: {}, than request: {}",
                       new_offset, old_offset);
          partition_offsets_[partition].offset = new_offset;
          return seastar::make_ready_future<std::unique_ptr<wal_read_reply>>(
            std::move(r));
        });
      });
  });
}
}  // namespace v