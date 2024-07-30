/*
 * Copyright 2019 Amazon.com, Inc. or its affiliates.
 * Licensed under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <thread>
#include <aws/kinesis/core/shard_map.h>

#include <aws/kinesis/model/ListShardsRequest.h>

namespace aws {
namespace kinesis {
namespace core {

const std::chrono::milliseconds ShardMap::kMinBackoff{1000};
const std::chrono::milliseconds ShardMap::kMaxBackoff{30000};
const std::chrono::milliseconds ShardMap::kClosedShardTtl{30000};

ShardMap::ShardMap(
    std::shared_ptr<aws::utils::Executor> executor,
    std::shared_ptr<Aws::Kinesis::KinesisClient> kinesis_client,
    std::string stream,
    std::string stream_arn,
    std::shared_ptr<aws::metrics::MetricsManager> metrics_manager,
    std::chrono::milliseconds min_backoff,
    std::chrono::milliseconds max_backoff,
    std::chrono::milliseconds closed_shard_ttl)
    : executor_(std::move(executor)),
      kinesis_client_(std::move(kinesis_client)),
      stream_(std::move(stream)),
      stream_arn_(std::move(stream_arn)),
      metrics_manager_(std::move(metrics_manager)),
      state_(INVALID),
      min_backoff_(min_backoff),
      max_backoff_(max_backoff),
      closed_shard_ttl_(closed_shard_ttl),
      backoff_(min_backoff_) {
  update();
}

// Mutex shard_cache_mutex_;

boost::optional<uint64_t> ShardMap::shard_id(const uint128_t& hash_key) {
  ReadLock lock(mutex_, aws::defer_lock);

  if (lock.try_lock() && state_ == READY) {
    auto it = std::lower_bound(end_hash_key_to_shard_id_.begin(),
                               end_hash_key_to_shard_id_.end(),
                               hash_key,
                               [](const auto& pair, auto key) {
                                 return pair.first < key;
                               });
    if (it != end_hash_key_to_shard_id_.end()) {
      return it->second;
    } else {
      LOG(error) << "Could not map hash key to shard id. Something's wrong"
                 << " with the shard map. Hash key = " << hash_key;
    }
  }

  return boost::none;
}

boost::optional<Aws::Kinesis::Model::Shard> ShardMap::shard(const uint64_t& actual_shard) {
  ReadLock lock(shard_cache_mutex_, aws::defer_lock);
  auto it = shard_id_to_shard_.find(actual_shard);
  if (it != shard_id_to_shard_.end()) {
      return it->second.first;
  }
  return boost::none;
}


void ShardMap::invalidate(const TimePoint& seen_at, const boost::optional<uint64_t> predicted_shard) {
  WriteLock lock(mutex_);
  
  if (seen_at > updated_at_ && state_ == READY) {
    // todo: stric the check 
    if (!predicted_shard || (shard_id_to_shard_.find(*predicted_shard) != shard_id_to_shard_.end())) {
      std::chrono::duration<double, std::milli> fp_ms = seen_at - updated_at_;
      LOG(info) << "Deciding to update shard map for \"" << stream_ 
                <<"\" with a gap between seen_at and updated_at_ of " << fp_ms.count() << " ms " << "predicted shard: " << predicted_shard;
      update();
    }
  }
}

void ShardMap::update() {
  if (state_ == UPDATING) {
    return;
  }

  state_ = UPDATING;
  LOG(info) << "Updating shard map for stream \"" << stream_ << "\"";
  clear_all_stored_shards();
  if (scheduled_callback_) {
    scheduled_callback_->cancel();
  }
  
  //We can call list shards directly without checking for stream state
  //since list shard fails if the stream is not in the appropriate state. 
  list_shards();
}

void ShardMap::list_shards(const Aws::String& next_token) {
  Aws::Kinesis::Model::ListShardsRequest req;
  req.SetMaxResults(1000);

  if (!next_token.empty()) {
    req.SetNextToken(next_token);
  } else {
    req.SetStreamName(stream_);
    if (!stream_arn_.empty()) req.SetStreamARN(stream_arn_);
    Aws::Kinesis::Model::ShardFilter shardFilter;
    shardFilter.SetType(Aws::Kinesis::Model::ShardFilterType::AT_LATEST);
    req.SetShardFilter(shardFilter);
  }

  kinesis_client_->ListShardsAsync(
      req,
      [this](auto /*client*/, auto& /*req*/, auto& outcome, auto& /*ctx*/) {
        this->list_shards_callback(outcome);
      },
      std::shared_ptr<const Aws::Client::AsyncCallerContext>());
}

void ShardMap::list_shards_callback(
      const Aws::Kinesis::Model::ListShardsOutcome& outcome) {
  if (!outcome.IsSuccess()) {
    auto e = outcome.GetError();
    update_fail(e.GetExceptionName(), e.GetMessage());
    return;
  }

  auto& shards = outcome.GetResult().GetShards();  
  for (auto& shard : shards) {
    open_shards.push_back(shard);
  }

  backoff_ = min_backoff_;
  
  auto& next_token = outcome.GetResult().GetNextToken();
  if (!next_token.empty()) {
    list_shards(next_token);
    return;
  }

  // sort_all_open_shards();
  build_minimal_disjoint_hashranges();

  WriteLock lock(mutex_);
  state_ = READY;
  updated_at_ = std::chrono::steady_clock::now();

  LOG(info) << "Successfully updated shard map for stream \""
            << stream_ << (stream_arn_.empty() ? "\"" : "\" (arn: \"" + stream_arn_ + "\"). Found ")
            << end_hash_key_to_shard_id_.size() << " shards";
}

void ShardMap::update_fail(const std::string &code, const std::string &msg) {
  LOG(error) << "Shard map update for stream \""
             << stream_ << (stream_arn_.empty() ? "\"" : "\" (arn: \"" + stream_arn_ + "\") failed. ")
             << "Code: " << code << " Message: " << msg << "; retrying in "
             << backoff_.count() << " ms";

  WriteLock lock(mutex_);
  state_ = INVALID;

  if (!scheduled_callback_) {
    scheduled_callback_ =
        executor_->schedule([
            this] { this->update(); },
            backoff_);
  } else {
    scheduled_callback_->reschedule(backoff_);
  }

  backoff_ = std::min(backoff_ * 3 / 2, max_backoff_);
}


void ShardMap::clear_all_stored_shards() {
  end_hash_key_to_shard_id_.clear();
  open_shards.clear();
}

void ShardMap::store_open_shard(const uint64_t shard_id, const uint128_t end_hash_key) {
  end_hash_key_to_shard_id_.push_back(
      std::make_pair(end_hash_key, shard_id));
}

void ShardMap::build_minimal_disjoint_hashranges() {
  if (open_shards.empty()) {
      return;
  }
  LOG(info) << "Sorting into buckets";
  // Sort shards by starting hashkey then by ending hashkey 
  std::sort(open_shards.begin(), open_shards.end(), [](const Aws::Kinesis::Model::Shard& a, const Aws::Kinesis::Model::Shard& b) {
      const uint128_t startA = uint128_t(a.GetHashKeyRange().GetStartingHashKey());
      const uint128_t startB = uint128_t(b.GetHashKeyRange().GetStartingHashKey());
      const uint128_t endA = uint128_t(a.GetHashKeyRange().GetEndingHashKey());
      const uint128_t endB = uint128_t(b.GetHashKeyRange().GetEndingHashKey());
      return (startA < startB) || (startA == startB && endA < endB);
  });

  // This is used filter out the closed shard from the shard_id_to_shard_ map.
  std::set<uint64_t> open_shard_ids;
  uint128_t lastEndingHashKey = 0;
  
  WriteLock lock(shard_cache_mutex_, aws::defer_lock);

  for (const auto& shard : open_shards) {
      const auto& shard_id = shard_id_from_str(shard.GetShardId());
      open_shard_ids.insert(shard_id);
      shard_id_to_shard_.insert({shard_id, std::make_pair(shard, std::chrono::time_point<std::chrono::steady_clock>())});
      
      const auto& range = shard.GetHashKeyRange();
      const uint128_t& start = uint128_t(range.GetStartingHashKey());
      const uint128_t& end = uint128_t(range.GetEndingHashKey());

      if (lastEndingHashKey == 0 || start > lastEndingHashKey) {
        store_open_shard(shard_id, uint128_t(end));
        lastEndingHashKey = end;
      }
  }

  //todo: remove 
  for (auto& entry : end_hash_key_to_shard_id_) {
     LOG(info) << "printing end_hash_key_to_shard_id_" << entry.second << " and "<< entry.first;
  }

  auto now = std::chrono::steady_clock::now();
  for (auto& entry : shard_id_to_shard_) {
    const uint64_t shard_id = entry.first;
    // Skip if ttl is added 
    if (entry.second.second != std::chrono::time_point<std::chrono::steady_clock>()) {
      continue;
    }
    // Add ttl if shard in not open or has ending sequence number added.
    if (open_shard_ids.find(shard_id) == open_shard_ids.end() || 
        !entry.second.first.GetSequenceNumberRange().GetEndingSequenceNumber().empty()) {
      LOG(info) << "Going to mark shard to remove " << shard_id << " from the map";
        entry.second.second = now;
    }
  }
}

void ShardMap::refresh() {
  while (true) {
    std::this_thread::sleep_for(closed_shard_ttl_ / 2); 
    bool is_updated = true;
    {
      ReadLock lock(shard_cache_mutex_, aws::defer_lock);
      for (auto& entry : shard_id_to_shard_) {
        if (entry.second.second != std::chrono::time_point<std::chrono::steady_clock>()) {
          is_updated = false;
          break;
        }
      }
    }
    if (is_updated) {
      continue;
    }
    auto now = std::chrono::steady_clock::now();
    {
      WriteLock lock(shard_cache_mutex_, aws::defer_lock);
      // Remove item if enough time is passed since the entry is marked for deletion. 
      for (auto it = shard_id_to_shard_.begin(); it != shard_id_to_shard_.end(); ) {
          if (it->second.second + closed_shard_ttl_ >= now) {
              it = shard_id_to_shard_.erase(it); 
          } else {
              ++it;
          }
      }
    }
  }
}

} //namespace core
} //namespace kinesis
} //namespace aws
