/*
 * Copyright (c) 2020-2021, NVIDIA CORPORATION.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <cuco/detail/bitwise_compare.cuh>

namespace cuco {

template <typename Key, typename Value, cuda::thread_scope Scope, typename Allocator>
static_map<Key, Value, Scope, Allocator>::static_map(std::size_t capacity,
                                                     Key empty_key_sentinel,
                                                     Value empty_value_sentinel,
                                                     Allocator const& alloc,
                                                     cudaStream_t stream)
  : capacity_{std::max(capacity, std::size_t{1})},  // to avoid dereferencing a nullptr (Issue #72)
    empty_key_sentinel_{empty_key_sentinel},
    empty_value_sentinel_{empty_value_sentinel},
    slot_allocator_{alloc},
    counter_allocator_{alloc}
{
  slots_         = std::allocator_traits<slot_allocator_type>::allocate(slot_allocator_, capacity_);
  num_successes_ = std::allocator_traits<counter_allocator_type>::allocate(counter_allocator_, 1);

  auto constexpr block_size = 256;
  auto constexpr stride     = 4;
  auto const grid_size      = (capacity_ + stride * block_size - 1) / (stride * block_size);
  detail::initialize<block_size, atomic_key_type, atomic_mapped_type>
    <<<grid_size, block_size, 0, stream>>>(
      slots_, empty_key_sentinel, empty_value_sentinel, capacity_);
}

template <typename Key, typename Value, cuda::thread_scope Scope, typename Allocator>
static_map<Key, Value, Scope, Allocator>::~static_map()
{
  std::allocator_traits<slot_allocator_type>::deallocate(slot_allocator_, slots_, capacity_);
  std::allocator_traits<counter_allocator_type>::deallocate(counter_allocator_, num_successes_, 1);
}

template <typename Key, typename Value, cuda::thread_scope Scope, typename Allocator>
template <typename InputIt, typename Hash, typename KeyEqual>
void static_map<Key, Value, Scope, Allocator>::insert(
  InputIt first, InputIt last, Hash hash, KeyEqual key_equal, cudaStream_t stream)
{
  auto num_keys = std::distance(first, last);
  if (num_keys == 0) { return; }

  auto const block_size = 128;
  auto const stride     = 1;
  auto const tile_size  = 4;
  auto const grid_size  = (tile_size * num_keys + stride * block_size - 1) / (stride * block_size);
  auto view             = get_device_mutable_view();

  // TODO: memset an atomic variable is unsafe
  static_assert(sizeof(std::size_t) == sizeof(atomic_ctr_type));
  CUCO_CUDA_TRY(cudaMemsetAsync(num_successes_, 0, sizeof(atomic_ctr_type), stream));
  std::size_t h_num_successes;

  detail::insert<block_size, tile_size><<<grid_size, block_size, 0, stream>>>(
    first, first + num_keys, num_successes_, view, hash, key_equal);
  CUCO_CUDA_TRY(cudaMemcpyAsync(
    &h_num_successes, num_successes_, sizeof(atomic_ctr_type), cudaMemcpyDeviceToHost, stream));

  CUCO_CUDA_TRY(cudaStreamSynchronize(stream));  // stream sync to ensure h_num_successes is updated

  size_ += h_num_successes;
}

template <typename Key, typename Value, cuda::thread_scope Scope, typename Allocator>
template <typename InputIt,
          typename StencilIt,
          typename Predicate,
          typename Hash,
          typename KeyEqual>
void static_map<Key, Value, Scope, Allocator>::insert_if(InputIt first,
                                                         InputIt last,
                                                         StencilIt stencil,
                                                         Predicate pred,
                                                         Hash hash,
                                                         KeyEqual key_equal,
                                                         cudaStream_t stream)
{
  auto num_keys = std::distance(first, last);
  if (num_keys == 0) { return; }

  auto constexpr block_size = 128;
  auto constexpr stride     = 1;
  auto constexpr tile_size  = 4;
  auto const grid_size = (tile_size * num_keys + stride * block_size - 1) / (stride * block_size);
  auto view            = get_device_mutable_view();

  // TODO: memset an atomic variable is unsafe
  static_assert(sizeof(std::size_t) == sizeof(atomic_ctr_type));
  CUCO_CUDA_TRY(cudaMemsetAsync(num_successes_, 0, sizeof(atomic_ctr_type), stream));
  std::size_t h_num_successes;

  detail::insert_if_n<block_size, tile_size><<<grid_size, block_size, 0, stream>>>(
    first, num_keys, num_successes_, view, stencil, pred, hash, key_equal);
  CUCO_CUDA_TRY(cudaMemcpyAsync(
    &h_num_successes, num_successes_, sizeof(atomic_ctr_type), cudaMemcpyDeviceToHost, stream));
  CUCO_CUDA_TRY(cudaStreamSynchronize(stream));

  size_ += h_num_successes;
}

template <typename Key, typename Value, cuda::thread_scope Scope, typename Allocator>
template <typename InputIt, typename OutputIt, typename Hash, typename KeyEqual>
void static_map<Key, Value, Scope, Allocator>::find(InputIt first,
                                                    InputIt last,
                                                    OutputIt output_begin,
                                                    Hash hash,
                                                    KeyEqual key_equal,
                                                    cudaStream_t stream)
{
  auto num_keys = std::distance(first, last);
  if (num_keys == 0) { return; }

  auto const block_size = 128;
  auto const stride     = 1;
  auto const tile_size  = 4;
  auto const grid_size  = (tile_size * num_keys + stride * block_size - 1) / (stride * block_size);
  auto view             = get_device_view();

  detail::find<block_size, tile_size, Value>
    <<<grid_size, block_size, 0, stream>>>(first, last, output_begin, view, hash, key_equal);
}

template <typename Key, typename Value, cuda::thread_scope Scope, typename Allocator>
template <typename InputIt, typename OutputIt, typename Hash, typename KeyEqual>
void static_map<Key, Value, Scope, Allocator>::contains(InputIt first,
                                                        InputIt last,
                                                        OutputIt output_begin,
                                                        Hash hash,
                                                        KeyEqual key_equal,
                                                        cudaStream_t stream)
{
  auto num_keys = std::distance(first, last);
  if (num_keys == 0) { return; }

  auto const block_size = 128;
  auto const stride     = 1;
  auto const tile_size  = 4;
  auto const grid_size  = (tile_size * num_keys + stride * block_size - 1) / (stride * block_size);
  auto view             = get_device_view();

  detail::contains<block_size, tile_size>
    <<<grid_size, block_size, 0, stream>>>(first, last, output_begin, view, hash, key_equal);
}

template <typename Key, typename Value, cuda::thread_scope Scope, typename Allocator>
template <typename KeyEqual>
__device__ static_map<Key, Value, Scope, Allocator>::device_mutable_view::insert_result
static_map<Key, Value, Scope, Allocator>::device_mutable_view::packed_cas(
  iterator current_slot, value_type const& insert_pair, KeyEqual key_equal) noexcept
{
  auto expected_key   = this->get_empty_key_sentinel();
  auto expected_value = this->get_empty_value_sentinel();

  cuco::detail::pair_converter<value_type> expected_pair{
    cuco::make_pair<Key, Value>(std::move(expected_key), std::move(expected_value))};
  cuco::detail::pair_converter<value_type> new_pair{insert_pair};

  auto slot =
    reinterpret_cast<cuda::atomic<typename cuco::detail::pair_converter<value_type>::packed_type>*>(
      current_slot);

  bool success = slot->compare_exchange_strong(
    expected_pair.packed, new_pair.packed, cuda::std::memory_order_relaxed);
  if (success) {
    return insert_result::SUCCESS;
  }
  // duplicate present during insert
  else if (key_equal(insert_pair.first, expected_pair.pair.first)) {
    return insert_result::DUPLICATE;
  }

  return insert_result::CONTINUE;
}

template <typename Key, typename Value, cuda::thread_scope Scope, typename Allocator>
template <typename KeyEqual>
__device__ static_map<Key, Value, Scope, Allocator>::device_mutable_view::insert_result
static_map<Key, Value, Scope, Allocator>::device_mutable_view::back_to_back_cas(
  iterator current_slot, value_type const& insert_pair, KeyEqual key_equal) noexcept
{
  using cuda::std::memory_order_relaxed;

  auto expected_key   = this->get_empty_key_sentinel();
  auto expected_value = this->get_empty_value_sentinel();

  // Back-to-back CAS for 8B/8B key/value pairs
  auto& slot_key   = current_slot->first;
  auto& slot_value = current_slot->second;

  bool key_success =
    slot_key.compare_exchange_strong(expected_key, insert_pair.first, memory_order_relaxed);
  bool value_success =
    slot_value.compare_exchange_strong(expected_value, insert_pair.second, memory_order_relaxed);

  if (key_success) {
    while (not value_success) {
      value_success =
        slot_value.compare_exchange_strong(expected_value = this->get_empty_value_sentinel(),
                                           insert_pair.second,
                                           memory_order_relaxed);
    }
    return insert_result::SUCCESS;
  } else if (value_success) {
    slot_value.store(this->get_empty_value_sentinel(), memory_order_relaxed);
  }

  // our key was already present in the slot, so our key is a duplicate
  if (key_equal(insert_pair.first, expected_key)) { return insert_result::DUPLICATE; }

  return insert_result::CONTINUE;
}

template <typename Key, typename Value, cuda::thread_scope Scope, typename Allocator>
template <typename KeyEqual>
__device__ static_map<Key, Value, Scope, Allocator>::device_mutable_view::insert_result
static_map<Key, Value, Scope, Allocator>::device_mutable_view::cas_dependent_write(
  iterator current_slot, value_type const& insert_pair, KeyEqual key_equal) noexcept
{
  using cuda::std::memory_order_relaxed;
  auto expected_key = this->get_empty_key_sentinel();

  auto& slot_key = current_slot->first;

  auto const key_success =
    slot_key.compare_exchange_strong(expected_key, insert_pair.first, memory_order_relaxed);

  if (key_success) {
    auto& slot_value = current_slot->second;
    slot_value.store(insert_pair.second, memory_order_relaxed);
    return insert_result::SUCCESS;
  }

  // our key was already present in the slot, so our key is a duplicate
  if (key_equal(insert_pair.first, expected_key)) { return insert_result::DUPLICATE; }

  return insert_result::CONTINUE;
}

template <typename Key, typename Value, cuda::thread_scope Scope, typename Allocator>
template <typename Hash, typename KeyEqual>
__device__ bool static_map<Key, Value, Scope, Allocator>::device_mutable_view::insert(
  value_type const& insert_pair, Hash hash, KeyEqual key_equal) noexcept
{
  auto current_slot{initial_slot(insert_pair.first, hash)};

  while (true) {
    key_type const existing_key = current_slot->first.load(cuda::std::memory_order_relaxed);
    // The user provide `key_equal` can never be used to compare against `empty_key_sentinel` as the
    // sentinel is not a valid key value. Therefore, first check for the sentinel
    auto const slot_is_empty =
      detail::bitwise_compare(existing_key, this->get_empty_key_sentinel());

    // the key we are trying to insert is already in the map, so we return with failure to insert
    if (not slot_is_empty and key_equal(existing_key, insert_pair.first)) { return false; }

    if (slot_is_empty) {
      auto const status = [&]() {
        // One single CAS operation if `value_type` is packable
        if constexpr (cuco::detail::is_packable<value_type>()) {
          return packed_cas(current_slot, insert_pair, key_equal);
        }

        if constexpr (not cuco::detail::is_packable<value_type>()) {
#if __CUDA_ARCH__ < 700
          return cas_dependent_write(current_slot, insert_pair, key_equal);
#else
          return back_to_back_cas(current_slot, insert_pair, key_equal);
#endif
        }
      }();

      // successful insert
      if (status == insert_result::SUCCESS) { return true; }
      // duplicate present during insert
      if (status == insert_result::DUPLICATE) { return false; }
    }

    // if we couldn't insert the key, but it wasn't a duplicate, then there must
    // have been some other key there, so we keep looking for a slot
    current_slot = next_slot(current_slot);
  }
}

template <typename Key, typename Value, cuda::thread_scope Scope, typename Allocator>
template <typename CG, typename Hash, typename KeyEqual>
__device__ bool static_map<Key, Value, Scope, Allocator>::device_mutable_view::insert(
  CG const& g, value_type const& insert_pair, Hash hash, KeyEqual key_equal) noexcept
{
  auto current_slot = initial_slot(g, insert_pair.first, hash);

  while (true) {
    key_type const existing_key = current_slot->first.load(cuda::std::memory_order_relaxed);

    // The user provide `key_equal` can never be used to compare against `empty_key_sentinel` as the
    // sentinel is not a valid key value. Therefore, first check for the sentinel
    auto const slot_is_empty =
      detail::bitwise_compare(existing_key, this->get_empty_key_sentinel());

    // the key we are trying to insert is already in the map, so we return with failure to insert
    if (g.any(not slot_is_empty and key_equal(existing_key, insert_pair.first))) { return false; }

    auto const window_contains_empty = g.ballot(slot_is_empty);

    // we found an empty slot, but not the key we are inserting, so this must
    // be an empty slot into which we can insert the key
    if (window_contains_empty) {
      // the first lane in the group with an empty slot will attempt the insert
      insert_result status{insert_result::CONTINUE};
      uint32_t src_lane = __ffs(window_contains_empty) - 1;

      if (g.thread_rank() == src_lane) {
        // One single CAS operation if `value_type` is packable
        if constexpr (cuco::detail::is_packable<value_type>()) {
          status = packed_cas(current_slot, insert_pair, key_equal);
        }
        // Otherwise, two back-to-back CAS operations
        else {
#if __CUDA_ARCH__ < 700
          status = cas_dependent_write(current_slot, insert_pair, key_equal);
#else
          status = back_to_back_cas(current_slot, insert_pair, key_equal);
#endif
        }
      }

      uint32_t res_status = g.shfl(static_cast<uint32_t>(status), src_lane);
      status              = static_cast<insert_result>(res_status);

      // successful insert
      if (status == insert_result::SUCCESS) { return true; }
      // duplicate present during insert
      if (status == insert_result::DUPLICATE) { return false; }
      // if we've gotten this far, a different key took our spot
      // before we could insert. We need to retry the insert on the
      // same window
    }
    // if there are no empty slots in the current window,
    // we move onto the next window
    else {
      current_slot = next_slot(g, current_slot);
    }
  }
}

template <typename Key, typename Value, cuda::thread_scope Scope, typename Allocator>
template <typename Hash, typename KeyEqual>
__device__ typename static_map<Key, Value, Scope, Allocator>::device_view::iterator
static_map<Key, Value, Scope, Allocator>::device_view::find(Key const& k,
                                                            Hash hash,
                                                            KeyEqual key_equal) noexcept
{
  auto current_slot = initial_slot(k, hash);

  while (true) {
    auto const existing_key = current_slot->first.load(cuda::std::memory_order_relaxed);
    // Key doesn't exist, return end()
    if (detail::bitwise_compare(existing_key, this->get_empty_key_sentinel())) {
      return this->end();
    }

    // Key exists, return iterator to location
    if (key_equal(existing_key, k)) { return current_slot; }

    current_slot = next_slot(current_slot);
  }
}

template <typename Key, typename Value, cuda::thread_scope Scope, typename Allocator>
template <typename Hash, typename KeyEqual>
__device__ typename static_map<Key, Value, Scope, Allocator>::device_view::const_iterator
static_map<Key, Value, Scope, Allocator>::device_view::find(Key const& k,
                                                            Hash hash,
                                                            KeyEqual key_equal) const noexcept
{
  auto current_slot = initial_slot(k, hash);

  while (true) {
    auto const existing_key = current_slot->first.load(cuda::std::memory_order_relaxed);
    // Key doesn't exist, return end()
    if (detail::bitwise_compare(existing_key, this->get_empty_key_sentinel())) {
      return this->end();
    }

    // Key exists, return iterator to location
    if (key_equal(existing_key, k)) { return current_slot; }

    current_slot = next_slot(current_slot);
  }
}

template <typename Key, typename Value, cuda::thread_scope Scope, typename Allocator>
template <typename CG, typename Hash, typename KeyEqual>
__device__ typename static_map<Key, Value, Scope, Allocator>::device_view::iterator
static_map<Key, Value, Scope, Allocator>::device_view::find(CG g,
                                                            Key const& k,
                                                            Hash hash,
                                                            KeyEqual key_equal) noexcept
{
  auto current_slot = initial_slot(g, k, hash);

  while (true) {
    auto const existing_key = current_slot->first.load(cuda::std::memory_order_relaxed);

    // The user provide `key_equal` can never be used to compare against `empty_key_sentinel` as
    // the sentinel is not a valid key value. Therefore, first check for the sentinel
    auto const slot_is_empty =
      detail::bitwise_compare(existing_key, this->get_empty_key_sentinel());

    // the key we were searching for was found by one of the threads,
    // so we return an iterator to the entry
    auto const exists = g.ballot(not slot_is_empty and key_equal(existing_key, k));
    if (exists) {
      uint32_t src_lane = __ffs(exists) - 1;
      // TODO: This shouldn't cast an iterator to an int to shuffle. Instead, get the index of the
      // current_slot and shuffle that instead.
      intptr_t res_slot = g.shfl(reinterpret_cast<intptr_t>(current_slot), src_lane);
      return reinterpret_cast<iterator>(res_slot);
    }

    // we found an empty slot, meaning that the key we're searching for isn't present
    if (g.ballot(slot_is_empty)) { return this->end(); }

    // otherwise, all slots in the current window are full with other keys, so we move onto the
    // next window
    current_slot = next_slot(g, current_slot);
  }
}

template <typename Key, typename Value, cuda::thread_scope Scope, typename Allocator>
template <typename CG, typename Hash, typename KeyEqual>
__device__ typename static_map<Key, Value, Scope, Allocator>::device_view::const_iterator
static_map<Key, Value, Scope, Allocator>::device_view::find(CG g,
                                                            Key const& k,
                                                            Hash hash,
                                                            KeyEqual key_equal) const noexcept
{
  auto current_slot = initial_slot(g, k, hash);

  while (true) {
    auto const existing_key = current_slot->first.load(cuda::std::memory_order_relaxed);

    // The user provide `key_equal` can never be used to compare against `empty_key_sentinel` as
    // the sentinel is not a valid key value. Therefore, first check for the sentinel
    auto const slot_is_empty =
      detail::bitwise_compare(existing_key, this->get_empty_key_sentinel());

    // the key we were searching for was found by one of the threads, so we return an iterator to
    // the entry
    auto const exists = g.ballot(not slot_is_empty and key_equal(existing_key, k));
    if (exists) {
      uint32_t src_lane = __ffs(exists) - 1;
      // TODO: This shouldn't cast an iterator to an int to shuffle. Instead, get the index of the
      // current_slot and shuffle that instead.
      intptr_t res_slot = g.shfl(reinterpret_cast<intptr_t>(current_slot), src_lane);
      return reinterpret_cast<const_iterator>(res_slot);
    }

    // we found an empty slot, meaning that the key we're searching
    // for isn't in this submap, so we should move onto the next one
    if (g.ballot(slot_is_empty)) { return this->end(); }

    // otherwise, all slots in the current window are full with other keys,
    // so we move onto the next window in the current submap

    current_slot = next_slot(g, current_slot);
  }
}

template <typename Key, typename Value, cuda::thread_scope Scope, typename Allocator>
template <typename Hash, typename KeyEqual>
__device__ bool static_map<Key, Value, Scope, Allocator>::device_view::contains(
  Key const& k, Hash hash, KeyEqual key_equal) const noexcept
{
  auto current_slot = initial_slot(k, hash);

  while (true) {
    auto const existing_key = current_slot->first.load(cuda::std::memory_order_relaxed);

    if (detail::bitwise_compare(existing_key, this->empty_key_sentinel_)) { return false; }

    if (key_equal(existing_key, k)) { return true; }

    current_slot = next_slot(current_slot);
  }
}

template <typename Key, typename Value, cuda::thread_scope Scope, typename Allocator>
template <typename CG, typename Hash, typename KeyEqual>
__device__ bool static_map<Key, Value, Scope, Allocator>::device_view::contains(
  CG g, Key const& k, Hash hash, KeyEqual key_equal) const noexcept
{
  auto current_slot = initial_slot(g, k, hash);

  while (true) {
    key_type const existing_key = current_slot->first.load(cuda::std::memory_order_relaxed);

    // The user provide `key_equal` can never be used to compare against `empty_key_sentinel` as
    // the sentinel is not a valid key value. Therefore, first check for the sentinel
    auto const slot_is_empty =
      detail::bitwise_compare(existing_key, this->get_empty_key_sentinel());

    // the key we were searching for was found by one of the threads, so we return an iterator to
    // the entry
    if (g.ballot(not slot_is_empty and key_equal(existing_key, k))) { return true; }

    // we found an empty slot, meaning that the key we're searching for isn't present
    if (g.ballot(slot_is_empty)) { return false; }

    // otherwise, all slots in the current window are full with other keys, so we move onto the
    // next window
    current_slot = next_slot(g, current_slot);
  }
}
}  // namespace cuco
