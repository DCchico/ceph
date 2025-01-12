// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Bitmap based in-memory allocator implementation.
 * Author: Igor Fedotov, ifedotov@suse.com
 *
 */

#include "fastbmap_allocator_impl.h"

uint64_t AllocatorLevel::l0_dives = 0;
uint64_t AllocatorLevel::l0_iterations = 0;
uint64_t AllocatorLevel::l0_inner_iterations = 0;
uint64_t AllocatorLevel::alloc_fragments = 0;
uint64_t AllocatorLevel::alloc_fragments_fast = 0;
uint64_t AllocatorLevel::l2_allocs = 0;

// Truncate interval<off, len> (both head and tail) to align with min_length 
inline interval_t _align2units(uint64_t offset, uint64_t len, uint64_t min_length)
{
  interval_t res;
  if (len >= min_length) {
    res.offset = p2roundup(offset, min_length);
    auto delta_off = res.offset - offset;
    if (len > delta_off) {
      res.length = len - delta_off;
      res.length = p2align<uint64_t>(res.length, min_length);
      if (res.length) {
	return res;
      }
    }
  }
  return interval_t();
}

interval_t AllocatorLevel01Loose::_get_longest_from_l0(uint64_t pos0,
  uint64_t pos1, uint64_t min_length, interval_t* tail) const
{
  interval_t res;
  if (pos0 >= pos1) {
    return res;
  }
  auto pos = pos0;

  interval_t res_candidate;
  if (tail->length != 0) {
    ceph_assert((tail->offset % l0_granularity) == 0);
    ceph_assert((tail->length % l0_granularity) == 0);
    res_candidate.offset = tail->offset / l0_granularity;
    res_candidate.length = tail->length / l0_granularity;
  }
  *tail = interval_t();

  auto d = CHILD_PER_SLOT_L0;
  slot_t bits = l0[pos / d];
  bits >>= ((pos % d) * L0_ENTRY_WIDTH);
  bool end_loop = false;
  auto min_granules = min_length / l0_granularity;

  do {
    if ((pos % d) == 0) {
      bits = l0[pos / d];
      if (pos1 - pos >= d) {
		  if (bits == all_slot_set) {
			  // slot is totally free
			  if (!res_candidate.length) {
				  res_candidate.offset = pos;
			  }
			  res_candidate.length += d;
			  pos += d;
			  end_loop = pos >= pos1;
			  if (end_loop) {
				  *tail = res_candidate;
				  res_candidate = _align2units(res_candidate.offset,
					  res_candidate.length, min_granules);
				  if (res.length < res_candidate.length) {
					  res = res_candidate;
				  }
			  }
			  continue;
		  } else if (_is_l0_slot_clear(bits)) {
				// slot is totally allocated
				res_candidate = _align2units(res_candidate.offset,
					res_candidate.length, min_granules);
				if (res.length < res_candidate.length) {
					res = res_candidate;
				}
				res_candidate = interval_t();
				pos += d;
				end_loop = pos >= pos1;
				continue;
		}
      }
    } //if ((pos % d) == 0)

    end_loop = ++pos >= pos1;
	if (!(~(bits | ~(slot_t(L0_ENTRY_MASK))))) {
      // item is free
      if (!res_candidate.length) {
	res_candidate.offset = pos - 1;
      }
      ++res_candidate.length;
      if (end_loop) {
	*tail = res_candidate;
	res_candidate = _align2units(res_candidate.offset,
	  res_candidate.length, min_granules);
	if (res.length < res_candidate.length) {
	  res = res_candidate;
	}
      }
    } else {
      res_candidate = _align2units(res_candidate.offset,
	res_candidate.length, min_granules);
      if (res.length < res_candidate.length) {
	res = res_candidate;
      }
      res_candidate = interval_t();
    }
    bits >>= L0_ENTRY_WIDTH;
  } while (!end_loop);
  res.offset *= l0_granularity;
  res.length *= l0_granularity;
  tail->offset *= l0_granularity;
  tail->length *= l0_granularity;
  return res;
}

void AllocatorLevel01Loose::_analyze_partials(uint64_t pos_start,
  uint64_t pos_end, uint64_t length, uint64_t min_length, int mode,
  search_ctx_t* ctx)
{
  auto d = CHILD_PER_SLOT;
  ceph_assert((pos_start % d) == 0);
  ceph_assert((pos_end % d) == 0);

  uint64_t l0_w = slotset_width * CHILD_PER_SLOT_L0;

  uint64_t l1_pos = pos_start;
  const interval_t empty_tail;
  interval_t prev_tail;

  uint64_t next_free_l1_pos = 0;
  for (auto pos = pos_start / d; pos < pos_end / d; ++pos) {
    slot_t slot_val = l1[pos];
    // FIXME minor: code below can be optimized to check slot_val against
    // all_slot_set(_clear) value

    for (auto c = 0; c < d; c++) {
      switch (slot_val & L1_ENTRY_MASK) {
      case L1_ENTRY_FREE:
        prev_tail  = empty_tail;
        if (!ctx->free_count) {
          ctx->free_l1_pos = l1_pos;
        } else if (l1_pos != next_free_l1_pos){
	  auto o = ctx->free_l1_pos * l1_granularity;
	  auto l = ctx->free_count * l1_granularity;
          // check if already found extent fits min_length after alignment
	  if (_align2units(o, l, min_length).length >= min_length) {
	    break;
	  }
	  // if not - proceed with the next one
          ctx->free_l1_pos = l1_pos;
          ctx->free_count = 0;
	}
	next_free_l1_pos = l1_pos + 1;
        ++ctx->free_count;
        if (mode == STOP_ON_EMPTY) {
          return;
        }
        break;
      case L1_ENTRY_FULL:
        prev_tail = empty_tail;
        break;
      case L1_ENTRY_PARTIAL:
	interval_t longest;
        ++ctx->partial_count;

        longest = _get_longest_from_l0(l1_pos * l0_w, (l1_pos + 1) * l0_w, min_length, &prev_tail);

        if (longest.length >= length) {
          if ((ctx->affordable_len == 0) ||
              ((ctx->affordable_len != 0) &&
                (longest.length < ctx->affordable_len))) {
            ctx->affordable_len = longest.length;
	    ctx->affordable_offs = longest.offset;
          }
        }
        if (longest.length >= min_length &&
	    (ctx->min_affordable_len == 0 ||
	      (longest.length < ctx->min_affordable_len))) {

          ctx->min_affordable_len = p2align<uint64_t>(longest.length, min_length);
	  ctx->min_affordable_offs = longest.offset;
        }
        if (mode == STOP_ON_PARTIAL) {
          return;
        }
        break;
      }
      slot_val >>= L1_ENTRY_WIDTH;
      ++l1_pos;
    }
  }
  ctx->fully_processed = true;
}


//Difei: bit expression
void AllocatorLevel01Loose::_mark_l1_on_l0(int64_t l0_pos, int64_t l0_pos_end)
{
  if (l0_pos == l0_pos_end) {
    return;
  }
  auto d0 = CHILD_PER_SLOT_L0;
  uint64_t l1_w = CHILD_PER_SLOT;
  // this should be aligned with slotset boundaries
  ceph_assert(0 == (l0_pos % d0));
  ceph_assert(0 == (l0_pos_end % d0));

  int64_t idx = l0_pos / CHILD_PER_SLOT_L0;
  int64_t idx_end = l0_pos_end / CHILD_PER_SLOT_L0;
  slot_t mask_to_apply = L1_ENTRY_NOT_USED;

  auto l1_pos = l0_pos / d0;

  while (idx < idx_end) {
    if (_is_l0_slot_clear(l0[idx])) {
      // if not all prev slots are allocated then no need to check the
      // current slot set, it's partial
      ++idx;
      if (mask_to_apply == L1_ENTRY_NOT_USED) {
	mask_to_apply = L1_ENTRY_FULL;
      } else if (mask_to_apply != L1_ENTRY_FULL) {
	idx = p2roundup(idx, int64_t(slotset_width));
        mask_to_apply = L1_ENTRY_PARTIAL;
      }
    } else if (l0[idx] == all_slot_set) {
      // if not all prev slots are free then no need to check the
      // current slot set, it's partial
      ++idx;
      if (mask_to_apply == L1_ENTRY_NOT_USED) {
	mask_to_apply = L1_ENTRY_FREE;
      } else if (mask_to_apply != L1_ENTRY_FREE) {
	idx = p2roundup(idx, int64_t(slotset_width));
        mask_to_apply = L1_ENTRY_PARTIAL;
      }
    } else {
      // no need to check the current slot set, it's partial
      mask_to_apply = L1_ENTRY_PARTIAL;
      ++idx;
      idx = p2roundup(idx, int64_t(slotset_width));
    }
    if ((idx % slotset_width) == 0) {
      ceph_assert(mask_to_apply != L1_ENTRY_NOT_USED);
      uint64_t shift = (l1_pos % l1_w) * L1_ENTRY_WIDTH;
      slot_t& slot_val = l1[l1_pos / l1_w];
      auto mask = slot_t(L1_ENTRY_MASK) << shift;			// mask = 0011 << shift

      slot_t old_mask = (slot_val & mask) >> shift;
      switch(old_mask) {
      case L1_ENTRY_FREE:
	unalloc_l1_count--;
	break;
      case L1_ENTRY_PARTIAL:
	partial_l1_count--;
	break;
      }
      slot_val &= ~mask;
      slot_val |= slot_t(mask_to_apply) << shift;
      switch(mask_to_apply) {
      case L1_ENTRY_FREE:
	unalloc_l1_count++;
	break;
      case L1_ENTRY_PARTIAL:
	partial_l1_count++;
	break;
      }
      mask_to_apply = L1_ENTRY_NOT_USED;
      ++l1_pos;
    }
  }
}

void AllocatorLevel01Loose::_mark_alloc_l0(int64_t l0_pos_start,
  int64_t l0_pos_end)
{
  auto d0 = CHILD_PER_SLOT_L0;

  int64_t pos = l0_pos_start;
  slot_t bits = (slot_t)L0_ENTRY_MASK << ((l0_pos_start % d0) * L0_ENTRY_WIDTH);
  slot_t* val_s = &l0[pos / d0];
  int64_t pos_e = std::min(l0_pos_end, p2roundup<int64_t>(l0_pos_start + 1, d0));
  while (pos < pos_e) {
    (*val_s) &= ~bits;
    bits <<= L0_ENTRY_WIDTH;
    pos++;
  }
  pos_e = std::min(l0_pos_end, p2align<int64_t>(l0_pos_end, d0));
  while (pos < pos_e) {
    *(++val_s) = all_slot_clear;
    pos += d0;
  }
  bits = L0_ENTRY_MASK;
  ++val_s;
  while (pos < l0_pos_end) {
    (*val_s) &= ~bits;
    bits <<= L0_ENTRY_WIDTH;
    pos++;
  }
}

interval_t AllocatorLevel01Loose::_allocate_l1_contiguous(uint64_t length,
  uint64_t min_length, uint64_t max_length,
  uint64_t pos_start, uint64_t pos_end)
{
  interval_t res = { 0, 0 };
  uint64_t l0_w = slotset_width * CHILD_PER_SLOT_L0;

  if (unlikely(length <= l0_granularity)) {
    search_ctx_t ctx;
    _analyze_partials(pos_start, pos_end, l0_granularity, l0_granularity,
      STOP_ON_PARTIAL, &ctx);

    // check partially free slot sets first (including neighboring),
    // full length match required.
    if (ctx.affordable_len) {
      // allocate as specified
      ceph_assert(ctx.affordable_len >= length);
      auto pos = ctx.affordable_offs / l0_granularity;
      _mark_alloc_l1_l0(pos, pos + 1);
      res = interval_t(ctx.affordable_offs, length);
      return res;
    }

    // allocate from free slot sets
    if (ctx.free_count) {
      auto l = std::min(length, ctx.free_count * l1_granularity);
      ceph_assert((l % l0_granularity) == 0);
      auto pos_end = ctx.free_l1_pos * l0_w + l / l0_granularity;

      _mark_alloc_l1_l0(ctx.free_l1_pos * l0_w, pos_end);
      res = interval_t(ctx.free_l1_pos * l1_granularity, l);
      return res;
    }
  } else if (unlikely(length == l1_granularity)) {
    search_ctx_t ctx;
    _analyze_partials(pos_start, pos_end, length, min_length, STOP_ON_EMPTY, &ctx);

    // allocate using contiguous extent found at l1 if any
    if (ctx.free_count) {

      auto l = std::min(length, ctx.free_count * l1_granularity);
      ceph_assert((l % l0_granularity) == 0);
      auto pos_end = ctx.free_l1_pos * l0_w + l / l0_granularity;

      _mark_alloc_l1_l0(ctx.free_l1_pos * l0_w, pos_end);
      res = interval_t(ctx.free_l1_pos * l1_granularity, l);

      return res;
    }

    // we can terminate earlier on free entry only
    ceph_assert(ctx.fully_processed);

    // check partially free slot sets first (including neighboring),
    // full length match required.
    if (ctx.affordable_len) {
      ceph_assert(ctx.affordable_len >= length);
      ceph_assert((length % l0_granularity) == 0);
      auto pos_start = ctx.affordable_offs / l0_granularity;
      auto pos_end = (ctx.affordable_offs + length) / l0_granularity;
      _mark_alloc_l1_l0(pos_start, pos_end);
      res = interval_t(ctx.affordable_offs, length);
      return res;
    }
    if (ctx.min_affordable_len) {
      auto pos_start = ctx.min_affordable_offs / l0_granularity;
      auto pos_end = (ctx.min_affordable_offs + ctx.min_affordable_len) / l0_granularity;
      _mark_alloc_l1_l0(pos_start, pos_end);
      return interval_t(ctx.min_affordable_offs, ctx.min_affordable_len);
    }
  } else {
    search_ctx_t ctx;
    _analyze_partials(pos_start, pos_end, length, min_length, NO_STOP, &ctx);
    ceph_assert(ctx.fully_processed);
    // check partially free slot sets first (including neighboring),
    // full length match required.
    if (ctx.affordable_len) {
      ceph_assert(ctx.affordable_len >= length);
      ceph_assert((length % l0_granularity) == 0);
      auto pos_start = ctx.affordable_offs / l0_granularity;
      auto pos_end = (ctx.affordable_offs + length) / l0_granularity;
      _mark_alloc_l1_l0(pos_start, pos_end);
      res = interval_t(ctx.affordable_offs, length);
      return res;
    }
    // allocate using contiguous extent found at l1 if affordable
    // align allocated extent with min_length
    if (ctx.free_count) {
      auto o = ctx.free_l1_pos * l1_granularity;
      auto l = ctx.free_count * l1_granularity;
      interval_t aligned_extent = _align2units(o, l, min_length);
      if (aligned_extent.length > 0) {
	aligned_extent.length = std::min(length,
	  uint64_t(aligned_extent.length));
	ceph_assert((aligned_extent.offset % l0_granularity) == 0);
	ceph_assert((aligned_extent.length % l0_granularity) == 0);

	auto pos_start = aligned_extent.offset / l0_granularity;
	auto pos_end = (aligned_extent.offset + aligned_extent.length) / l0_granularity;

	_mark_alloc_l1_l0(pos_start, pos_end);
	return aligned_extent;
      }
    }
    if (ctx.min_affordable_len) {
      auto pos_start = ctx.min_affordable_offs / l0_granularity;
      auto pos_end = (ctx.min_affordable_offs + ctx.min_affordable_len) / l0_granularity;
      _mark_alloc_l1_l0(pos_start, pos_end);
      return interval_t(ctx.min_affordable_offs, ctx.min_affordable_len);
    }
  }
  return res;
}

bool AllocatorLevel01Loose::_allocate_l1(uint64_t length,
  uint64_t min_length, uint64_t max_length,
  uint64_t l1_pos_start, uint64_t l1_pos_end,
  uint64_t* allocated,
  interval_vector_t* res)
{
  uint64_t d0 = CHILD_PER_SLOT_L0; //32
  uint64_t d1 = CHILD_PER_SLOT; //32

  ceph_assert(0 == (l1_pos_start % (slotset_width * d1)));
  ceph_assert(0 == (l1_pos_end % (slotset_width * d1)));
  if (min_length != l0_granularity) {
    // probably not the most effecient way but
    // don't care much about that at the moment
    bool has_space = true;
    while (length > *allocated && has_space) {
      interval_t i =
        _allocate_l1_contiguous(length - *allocated, min_length, max_length,
	  l1_pos_start, l1_pos_end);
      if (i.length == 0) {
        has_space = false;
      } else {
	_fragment_and_emplace(max_length, i.offset, i.length, res);
        *allocated += i.length;
      }
    }
  } else {
    cerr << "Begin allocate l1 as min_len = l0_granularity" << std::endl;
    uint64_t l0_w = slotset_width * d0;

    for (auto idx = l1_pos_start / d1;
      idx < l1_pos_end / d1 && length > *allocated;
      ++idx) {
      slot_t& slot_val = l1[idx];
      if (slot_val == all_slot_clear) {
        continue;
      } else if (slot_val == all_slot_set) {
        uint64_t to_alloc = std::min(length - *allocated,
          l1_granularity * d1);
        *allocated += to_alloc;
        ++alloc_fragments_fast;
	_fragment_and_emplace(max_length, idx * d1 * l1_granularity, to_alloc,
	  res);
        _mark_alloc_l1_l0(idx * d1 * l0_w,
	  idx * d1 * l0_w + to_alloc / l0_granularity);
        continue;
      }
      auto free_pos = find_next_set_bit(slot_val, 0);
      ceph_assert(free_pos < bits_per_slot);
      do {
        ceph_assert(length > *allocated);
	if (free_pos % 2) {
	  --free_pos;
	  cerr << "Difei: help free_pos -= 1" << std::endl;
	}
        bool empty;
        empty = _allocate_l0(length, max_length,
	  (idx * d1 + free_pos / L1_ENTRY_WIDTH) * l0_w,
          (idx * d1 + free_pos / L1_ENTRY_WIDTH + 1) * l0_w,
          allocated,
          res);

	auto mask = slot_t(L1_ENTRY_MASK) << free_pos;

	slot_t old_mask = (slot_val & mask) >> free_pos;
	switch(old_mask) {
	case L1_ENTRY_FREE:
	  unalloc_l1_count--;
	  break;
	case L1_ENTRY_PARTIAL:
	  partial_l1_count--;
	  break;
	}
        slot_val &= ~mask;
        if (empty) {
          // the next line is no op with the current L1_ENTRY_FULL but left
          // as-is for the sake of uniformity and to avoid potential errors
          // in future
          slot_val |= slot_t(L1_ENTRY_FULL) << free_pos;
        } else {
          slot_val |= slot_t(L1_ENTRY_PARTIAL) << free_pos;
	  partial_l1_count++;
        }
        if (length <= *allocated || slot_val == all_slot_clear) {
          break;
        }
	free_pos = find_next_set_bit(slot_val, free_pos + L1_ENTRY_WIDTH);
      } while (free_pos < bits_per_slot);
    }
  }
  return _is_empty_l1(l1_pos_start, l1_pos_end);
}

void AllocatorLevel01Loose::collect_stats(
  std::map<size_t, size_t>& bins_overall)
{
  size_t free_seq_cnt = 0;
  for (auto slot : l0) {
    if (slot == all_slot_set) {
      free_seq_cnt += CHILD_PER_SLOT_L0;
    } else if(!_is_l0_slot_clear(slot)) {
      size_t pos = 0;
      do {
	auto pos1 = find_next_set_bit_l0(slot, pos);
	if (pos1 == pos) {
	  free_seq_cnt++;
	  pos = pos1 + 1;
	} else {
	  if (free_seq_cnt) {
	    bins_overall[cbits(free_seq_cnt) - 1]++;
	    free_seq_cnt = 0;
	  }
	  if (pos1 < bits_per_slot) {
	    free_seq_cnt = 1;
	  }
          pos = pos1 + 1;
	}
      } while (pos < bits_per_slot);
    } else if (free_seq_cnt) {
      bins_overall[cbits(free_seq_cnt) - 1]++;
      free_seq_cnt = 0;
    }
  }
  if (free_seq_cnt) {
    bins_overall[cbits(free_seq_cnt) - 1]++;
  }
}
