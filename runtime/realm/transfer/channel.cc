/* Copyright 2020 Stanford University
 * Copyright 2020 Los Alamos National Laboratory
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

#include "realm/realm_config.h"

#ifdef REALM_ON_WINDOWS
#define NOMINMAX
#endif

#include "realm/transfer/channel.h"
#include "realm/transfer/channel_disk.h"
#include "realm/transfer/transfer.h"
#include "realm/utils.h"

#include <algorithm>

TYPE_IS_SERIALIZABLE(Realm::XferDesKind);

namespace Realm {

    Logger log_new_dma("new_dma");
    Logger log_request("request");
    Logger log_xd("xd");

      // TODO: currently we use dma_all_gpus to track the set of GPU* created
#ifdef REALM_USE_CUDA
      std::vector<Cuda::GPU*> dma_all_gpus;
#endif
      // we use a single queue for all xferDes
      XferDesQueue *xferDes_queue = 0;

      // we use a single manager to organize all channels
      static ChannelManager *channel_manager = 0;

  // fast memcpy stuff - uses std::copy instead of memcpy to communicate
  //  alignment guarantees to the compiler
  template <typename T>
  static void memcpy_1d_typed(uintptr_t dst_base, uintptr_t src_base,
			      size_t bytes)
  {
    std::copy(reinterpret_cast<const T *>(src_base),
	      reinterpret_cast<const T *>(src_base + bytes),
	      reinterpret_cast<T *>(dst_base));
  }

  template <typename T>
  static void memcpy_2d_typed(uintptr_t dst_base, uintptr_t dst_lstride,
			      uintptr_t src_base, uintptr_t src_lstride,
			      size_t bytes, size_t lines)
  {
    for(size_t i = 0; i < lines; i++) {
      std::copy(reinterpret_cast<const T *>(src_base),
		reinterpret_cast<const T *>(src_base + bytes),
		reinterpret_cast<T *>(dst_base));
      // manual strength reduction
      src_base += src_lstride;
      dst_base += dst_lstride;
    }
  }

  template <typename T>
  static void memcpy_3d_typed(uintptr_t dst_base, uintptr_t dst_lstride,
			      uintptr_t dst_pstride,
			      uintptr_t src_base, uintptr_t src_lstride,
			      uintptr_t src_pstride,
			      size_t bytes, size_t lines, size_t planes)
  {
    // adjust plane stride amounts to account for line strides (so we don't have
    //  to subtract the line strides back out in the loop)
    uintptr_t dst_pstride_adj = dst_pstride - (lines * dst_lstride);
    uintptr_t src_pstride_adj = src_pstride - (lines * src_lstride);

    for(size_t j = 0; j < planes; j++) {
      for(size_t i = 0; i < lines; i++) {
	std::copy(reinterpret_cast<const T *>(src_base),
		  reinterpret_cast<const T *>(src_base + bytes),
		  reinterpret_cast<T *>(dst_base));
	// manual strength reduction
	src_base += src_lstride;
	dst_base += dst_lstride;
      }
      src_base += src_pstride_adj;
      dst_base += dst_pstride_adj;
    }
  }

  // need types with various powers-of-2 size/alignment - we have up to
  //  uint64_t as builtins, but we need trivially-copyable 16B and 32B things
  struct dummy_16b_t { uint64_t a, b; };
  struct dummy_32b_t { uint64_t a, b, c, d; };
  REALM_ALIGNED_TYPE_CONST(aligned_16b_t, dummy_16b_t, 16);
  REALM_ALIGNED_TYPE_CONST(aligned_32b_t, dummy_32b_t, 32);

  static void memcpy_1d(uintptr_t dst_base, uintptr_t src_base,
			size_t bytes)
  {
    // by subtracting 1 from bases, strides, and lengths, we get LSBs set
    //  based on the common alignment of every parameter in the copy
    unsigned alignment = ((dst_base - 1) & (src_base - 1) &
			  (bytes - 1));
//define DEBUG_MEMCPYS
#ifdef DEBUG_MEMCPYS
    log_xd.print() << std::hex << "memcpy_1d: dst=" << dst_base
                   << " src=" << src_base
                   << std::dec << " bytes=" << bytes
                   << " align=" << (alignment & 31);
#endif
    // TODO: consider jump table approach?
    if((alignment & 31) == 31)
      memcpy_1d_typed<aligned_32b_t>(dst_base, src_base, bytes);
    else if((alignment & 15) == 15)
      memcpy_1d_typed<aligned_16b_t>(dst_base, src_base, bytes);
    else if((alignment & 7) == 7)
      memcpy_1d_typed<uint64_t>(dst_base, src_base, bytes);
    else if((alignment & 3) == 3)
      memcpy_1d_typed<uint32_t>(dst_base, src_base, bytes);
    else if((alignment & 1) == 1)
      memcpy_1d_typed<uint16_t>(dst_base, src_base, bytes);
    else
      memcpy_1d_typed<uint8_t>(dst_base, src_base, bytes);
  }

  static void memcpy_2d(uintptr_t dst_base, uintptr_t dst_lstride,
			uintptr_t src_base, uintptr_t src_lstride,
			size_t bytes, size_t lines)
  {
    // by subtracting 1 from bases, strides, and lengths, we get LSBs set
    //  based on the common alignment of every parameter in the copy
    unsigned alignment = ((dst_base - 1) & (dst_lstride - 1) &
			  (src_base - 1) & (src_lstride - 1) &
			  (bytes - 1));
#ifdef DEBUG_MEMCPYS
    log_xd.print() << std::hex << "memcpy_2d: dst=" << dst_base
                   << "+" << dst_lstride
                   << " src=" << src_base
                   << "+" << src_lstride
                   << std::dec << " bytes=" << bytes
                   << " lines=" << lines
                   << " align=" << (alignment & 31);
#endif
    // TODO: consider jump table approach?
    if((alignment & 31) == 31)
      memcpy_2d_typed<aligned_32b_t>(dst_base, dst_lstride,
				     src_base, src_lstride,
				     bytes, lines);
    else if((alignment & 15) == 15)
      memcpy_2d_typed<aligned_16b_t>(dst_base, dst_lstride,
				     src_base, src_lstride,
				     bytes, lines);
    else if((alignment & 7) == 7)
      memcpy_2d_typed<uint64_t>(dst_base, dst_lstride, src_base, src_lstride,
				bytes, lines);
    else if((alignment & 3) == 3)
      memcpy_2d_typed<uint32_t>(dst_base, dst_lstride, src_base, src_lstride,
				bytes, lines);
    else if((alignment & 1) == 1)
      memcpy_2d_typed<uint16_t>(dst_base, dst_lstride, src_base, src_lstride,
				bytes, lines);
    else
      memcpy_2d_typed<uint8_t>(dst_base, dst_lstride, src_base, src_lstride,
			       bytes, lines);
  }

  static void memcpy_3d(uintptr_t dst_base, uintptr_t dst_lstride,
			uintptr_t dst_pstride,
			uintptr_t src_base, uintptr_t src_lstride,
			uintptr_t src_pstride,
			size_t bytes, size_t lines, size_t planes)
  {
    // by subtracting 1 from bases, strides, and lengths, we get LSBs set
    //  based on the common alignment of every parameter in the copy
    unsigned alignment = ((dst_base - 1) & (dst_lstride - 1) &
			  (dst_pstride - 1) &
			  (src_base - 1) & (src_lstride - 1) &
			  (src_pstride - 1) &
			  (bytes - 1));
#ifdef DEBUG_MEMCPYS
    log_xd.print() << std::hex << "memcpy_3d: dst=" << dst_base
                   << "+" << dst_lstride << "+" << dst_pstride
                   << " src=" << src_base
                   << "+" << src_lstride << "+" << src_pstride
                   << std::dec << " bytes=" << bytes
                   << " lines=" << lines
                   << " planes=" << planes
                   << " align=" << (alignment & 31);
#endif
    // performance optimization for intel (and probably other) cpus: walk
    //  destination addresses as linearly as possible, even if that messes up
    //  the source address pattern (probably because writebacks are more
    //  expensive than cache fills?)
    if(dst_pstride < dst_lstride) {
      // switch lines and planes
      std::swap(dst_pstride, dst_lstride);
      std::swap(src_pstride, src_lstride);
      std::swap(planes, lines);
    }
    // TODO: consider jump table approach?
    if((alignment & 31) == 31)
      memcpy_3d_typed<aligned_32b_t>(dst_base, dst_lstride, dst_pstride,
				     src_base, src_lstride, src_pstride,
				     bytes, lines, planes);
    else if((alignment & 15) == 15)
      memcpy_3d_typed<aligned_16b_t>(dst_base, dst_lstride, dst_pstride,
				     src_base, src_lstride, src_pstride,
				     bytes, lines, planes);
    else if((alignment & 7) == 7)
      memcpy_3d_typed<uint64_t>(dst_base, dst_lstride, dst_pstride,
				src_base, src_lstride, src_pstride,
				bytes, lines, planes);
    else if((alignment & 3) == 3)
      memcpy_3d_typed<uint32_t>(dst_base, dst_lstride, dst_pstride,
				src_base, src_lstride, src_pstride,
				bytes, lines, planes);
    else if((alignment & 1) == 1)
      memcpy_3d_typed<uint16_t>(dst_base, dst_lstride, dst_pstride,
				src_base, src_lstride, src_pstride,
				bytes, lines, planes);
    else
      memcpy_3d_typed<uint8_t>(dst_base, dst_lstride, dst_pstride,
				src_base, src_lstride, src_pstride,
			       bytes, lines, planes);
  }

#if 0
      static inline bool cross_ib(off_t start, size_t nbytes, size_t buf_size)
      {
        return (nbytes > 0) && (start / buf_size < (start + nbytes - 1) / buf_size);
      }
#endif
    SequenceAssembler::SequenceAssembler(void)
      : contig_amount_x2(0)
      , first_noncontig((size_t)-1)
    {
      mutex = new Mutex;
    }

    SequenceAssembler::SequenceAssembler(const SequenceAssembler& copy_from)
      : contig_amount_x2(copy_from.contig_amount_x2)
      , first_noncontig(copy_from.first_noncontig)
      , spans(copy_from.spans)
    {
      mutex = new Mutex;
    }

    SequenceAssembler::~SequenceAssembler(void)
    {
      delete mutex;
    }

    void SequenceAssembler::swap(SequenceAssembler& other)
    {
      // NOT thread-safe - taking mutexes won't help
      std::swap(contig_amount_x2, other.contig_amount_x2);
      std::swap(first_noncontig, other.first_noncontig);
      spans.swap(other.spans);
    }

    // asks if a span exists - return value is number of bytes from the
    //  start that do
    size_t SequenceAssembler::span_exists(size_t start, size_t count)
    {
      // lock-free case 1: start < contig_amount
      size_t contig_sample_x2 = contig_amount_x2.load_acquire();
      if(start < (contig_sample_x2 >> 1)) {
	size_t max_avail = (contig_sample_x2 >> 1) - start;
	if(count < max_avail)
	  return count;
	else
	  return max_avail;
      }

      // lock-free case 2a: no noncontig ranges known
      if((contig_sample_x2 & 1) == 0)
	return 0;

      // lock-free case 2b: contig_amount <= start < first_noncontig
      size_t noncontig_sample = first_noncontig.load();
      if(start < noncontig_sample)
	return 0;

      // general case 3: take the lock and look through spans/etc.
      {
	AutoLock<> al(*mutex);

	// first, recheck the contig_amount, in case both it and the noncontig
	//  counters were bumped in between looking at the two of them
	size_t contig_sample = contig_amount_x2.load_acquire() >> 1;
	if(start < contig_sample) {
	  size_t max_avail = contig_sample - start;
	  if(count < max_avail)
	    return count;
	  else
	    return max_avail;
	}

	// recheck noncontig as well
	if(start < first_noncontig.load())
	  return 0;

	// otherwise find the first span after us and then back up one to find
	//  the one that might contain our 'start'
	std::map<size_t, size_t>::const_iterator it = spans.upper_bound(start);
	// this should never be the first span
	assert(it != spans.begin());
	--it;
	assert(it->first <= start);
	// does this span overlap us?
	if((it->first + it->second) > start) {
	  size_t max_avail = it->first + it->second - start;
	  while(max_avail < count) {
	    // try to get more - return the current 'max_avail' if we fail
	    if(++it == spans.end())
	      return max_avail; // no more
	    if(it->first > (start + max_avail))
	      return max_avail; // not contiguous
	    max_avail += it->second;
	  }
	  // got at least as much as we wanted
	  return count;
	} else
	  return 0;
      }
    }

    // returns the amount by which the contiguous range has been increased
    //  (i.e. from [pos, pos+retval) )
    size_t SequenceAssembler::add_span(size_t pos, size_t count)
    {
      // fastest case - try to bump the contig amount without a lock, assuming
      //  there's no noncontig spans
      size_t prev_x2 = pos << 1;
      size_t next_x2 = (pos + count) << 1;
      if(contig_amount_x2.compare_exchange(prev_x2, next_x2)) {
	// success - we bumped by exactly 'count'
	return count;
      }

      // second best case - the CAS failed, but only because there are
      //  noncontig spans...  assuming spans aren't getting too out of order
      //  in the common case, we take the mutex and pick up any other spans we
      //  connect with
      if((prev_x2 >> 1) == pos) {
	size_t span_end = pos + count;
	{
	  AutoLock<> al(*mutex);

	  size_t new_noncontig = size_t(-1);
	  while(!spans.empty()) {
	    std::map<size_t, size_t>::iterator it = spans.begin();
	    if(it->first == span_end) {
	      span_end += it->second;
	      spans.erase(it);
	    } else {
	      // stop here - this is the new first noncontig
	      new_noncontig = it->first;
	      break;
	    }
	  }

	  // to avoid false negatives in 'span_exists', update contig amount
	  //  before we bump first_noncontig
	  next_x2 = (span_end << 1) + (spans.empty() ? 0 : 1);
	  // this must succeed
	  bool ok = contig_amount_x2.compare_exchange(prev_x2, next_x2);
	  assert(ok);

	  first_noncontig.store(new_noncontig);
	}

	return (span_end - pos);
      }

      // worst case - our span doesn't appear to be contiguous, so we have to
      //  take the mutex and add to the noncontig list (we may end up being
      //  contiguous if we're the first noncontig and things have caught up)
      {
	AutoLock<> al(*mutex);

	spans[pos] = count;

	if(pos > first_noncontig.load()) {
	  // in this case, we also know that spans wasn't empty and somebody
	  //  else has already set the LSB of contig_amount_x2
	  return 0;
	} else {
	  // we need to re-check contig_amount_x2 and make sure the LSB is
	  //  set - do both with an atomic OR
	  prev_x2 = contig_amount_x2.fetch_or(1);

	  if((prev_x2 >> 1) == pos) {
	    // we've been caught, so gather up spans and do another bump
	    size_t span_end = pos;
	    size_t new_noncontig = size_t(-1);
	    while(!spans.empty()) {
	      std::map<size_t, size_t>::iterator it = spans.begin();
	      if(it->first == span_end) {
		span_end += it->second;
		spans.erase(it);
	      } else {
		// stop here - this is the new first noncontig
		new_noncontig = it->first;
		break;
	      }
	    }
	    assert(span_end > pos);

	    // to avoid false negatives in 'span_exists', update contig amount
	    //  before we bump first_noncontig
	    next_x2 = (span_end << 1) + (spans.empty() ? 0 : 1);
	    // this must succeed (as long as we remember we set the LSB)
	    prev_x2 |= 1;
	    bool ok = contig_amount_x2.compare_exchange(prev_x2, next_x2);
	    assert(ok);

	    first_noncontig.store(new_noncontig);

	    return (span_end - pos);
	  } else {
	    // not caught, so no forward progress to report
	    return 0;
	  }
	}
      }
    }


  ////////////////////////////////////////////////////////////////////////
  //
  // class AddressList
  //

  AddressList::AddressList()
    : total_bytes(0)
    , write_pointer(0)
    , read_pointer(0)
  {}

  size_t *AddressList::begin_nd_entry(int max_dim)
  {
    size_t entries_needed = max_dim * 2;

    size_t new_wp = write_pointer + entries_needed;
    if(new_wp > MAX_ENTRIES) {
      // have to wrap around
      if(read_pointer <= entries_needed)
	return 0;

      // fill remaining entries with 0's so reader skips over them
      while(write_pointer < MAX_ENTRIES)
	data[write_pointer++] = 0;

      write_pointer = 0;
    } else {
      // if the write pointer would cross over the read pointer, we have to wait
      if((write_pointer < read_pointer) && (new_wp >= read_pointer))
	return 0;

      // special case: if the write pointer would wrap and read is at 0, that'd
      //  be a collision too
      if((new_wp == MAX_ENTRIES) && (read_pointer == 0))
	return 0;
    }

    // all good - return a pointer to the first available entry
    return (data + write_pointer);
  }

  void AddressList::commit_nd_entry(int act_dim, size_t bytes)
  {
    size_t entries_used = act_dim * 2;

    write_pointer += entries_used;
    if(write_pointer >= MAX_ENTRIES) {
      assert(write_pointer == MAX_ENTRIES);
      write_pointer = 0;
    }

    total_bytes += bytes;
  }

  size_t AddressList::bytes_pending() const
  {
    return total_bytes;
  }

  const size_t *AddressList::read_entry()
  {
    assert(total_bytes > 0);
    if(read_pointer >= MAX_ENTRIES) {
      assert(read_pointer == MAX_ENTRIES);
      read_pointer = 0;
    }
    // skip trailing 0's
    if(data[read_pointer] == 0)
      read_pointer = 0;
    return (data + read_pointer);
  }
	 

  ////////////////////////////////////////////////////////////////////////
  //
  // class AddressListCursor
  //

  AddressListCursor::AddressListCursor()
    : addrlist(0)
    , partial(false)
  {
    for(int i = 0; i < MAX_DIM; i++)
      pos[i] = 0;
  }

  void AddressListCursor::set_addrlist(AddressList *_addrlist)
  {
    addrlist = _addrlist;
  }

  int AddressListCursor::get_dim()
  {
    assert(addrlist);
    // with partial progress, we restrict ourselves to just the rest of that dim
    if(partial) {
      return (partial_dim + 1);
    } else {
      const size_t *entry = addrlist->read_entry();
      int act_dim = (entry[0] & 15);
      return act_dim;
    }
  }

  uintptr_t AddressListCursor::get_offset()
  {
    const size_t *entry = addrlist->read_entry();
    int act_dim = (entry[0] & 15);
    uintptr_t ofs = entry[1];
    if(partial) {
      for(int i = partial_dim; i < act_dim; i++)
	if(i == 0) {
	  // dim 0 is counted in bytes
	  ofs += pos[0];
	} else {
	  // rest use the strides from the address list
	  ofs += pos[i] * entry[1 + (2 * i)];
	}
    }
    return ofs;
  }

  uintptr_t AddressListCursor::get_stride(int dim)
  {
    const size_t *entry = addrlist->read_entry();
    int act_dim = (entry[0] & 15);
    assert((dim > 0) && (dim < act_dim));
    return entry[2 * dim + 1];
  }

  size_t AddressListCursor::remaining(int dim)
  {
    const size_t *entry = addrlist->read_entry();
    int act_dim = (entry[0] & 15);
    assert(dim < act_dim);
    size_t r = entry[2 * dim];
    if(dim == 0) r >>= 4;
    if(partial) {
      if(dim > partial_dim) r = 1;
      if(dim == partial_dim) {
	assert(r > pos[dim]);
	r -= pos[dim];
      }
    }
    return r;
  }

  void AddressListCursor::advance(int dim, size_t amount)
  {
    const size_t *entry = addrlist->read_entry();
    int act_dim = (entry[0] & 15);
    assert(dim < act_dim);
    size_t r = entry[2 * dim];
    if(dim == 0) r >>= 4;

    size_t bytes = amount;
    if(dim > 0) {
#ifdef DEBUG_REALM
      for(int i = 0; i < dim; i++)
	assert(pos[i] == 0);
#endif
      bytes *= (entry[0] >> 4);
      for(int i = 1; i < dim; i++)
	bytes *= entry[2 * i];
    }
#ifdef DEBUG_REALM
    assert(addrlist->total_bytes >= bytes);
#endif
    addrlist->total_bytes -= bytes;
    
    if(!partial) {
      if((dim == (act_dim - 1)) && (amount == r)) {
	// simple case - we consumed the whole thing
	addrlist->read_pointer += 2 * act_dim;
	return;
      } else {
	// record partial consumption
	partial = true;
	partial_dim = dim;
	pos[partial_dim] = amount;
      }
    } else {
      // update a partial consumption in progress
      assert(dim <= partial_dim);
      partial_dim = dim;
      pos[partial_dim] += amount;
    }

    while(pos[partial_dim] == r) {
      pos[partial_dim++] = 0;
      if(partial_dim == act_dim) {
	// all done
	partial = false;
	addrlist->read_pointer += 2 * act_dim;
	break;
      } else {
	pos[partial_dim]++;  // carry into next dimension
	r = entry[2 * partial_dim]; // no shift because partial_dim > 0
      }
    }
  }

  void AddressListCursor::skip_bytes(size_t bytes)
  {
    while(bytes > 0) {
      int act_dim = get_dim();

      if(act_dim == 0) {
	assert(0);
      } else {
	size_t chunk = remaining(0);
	if(chunk <= bytes) {
	  int dim = 0;
	  size_t count = chunk;
	  while((dim + 1) < act_dim) {
	    dim++;
	    count = bytes / chunk;
	    assert(count > 0);
	    size_t r = remaining(dim + 1);
	    if(count < r) {
	      chunk *= count;
	      break;
	    } else {
	      count = r;
	      chunk *= count;
	    }
	  }
	  advance(dim, count);
	  bytes -= chunk;
	} else {
	  advance(0, bytes);
	  return;
	}
      }
    }
  }


      XferDes::XferDes(DmaRequest *_dma_request, NodeID _launch_node, XferDesID _guid,
		       const std::vector<XferDesPortInfo>& inputs_info,
		       const std::vector<XferDesPortInfo>& outputs_info,
		       bool _mark_start,
		       uint64_t _max_req_size, int _priority,
		       XferDesFence* _complete_fence)
        : dma_request(_dma_request), mark_start(_mark_start), launch_node(_launch_node),
	  iteration_completed(false),
	  transfer_completed(false),
          max_req_size(_max_req_size), priority(_priority),
          guid(_guid),
          channel(NULL), complete_fence(_complete_fence),
	  progress_counter(0), reference_count(1)
      {
	input_ports.resize(inputs_info.size());
	int gather_control_port = -1;
	int scatter_control_port = -1;
	for(size_t i = 0; i < inputs_info.size(); i++) {
	  XferPort& p = input_ports[i];
	  const XferDesPortInfo& ii = inputs_info[i];

	  p.mem = get_runtime()->get_memory_impl(ii.mem);
	  p.iter = ii.iter;
	  if(ii.serdez_id != 0) {
	    const CustomSerdezUntyped *op = get_runtime()->custom_serdez_table.get(ii.serdez_id, 0);
	    assert(op != 0);
	    p.serdez_op = op;
	  } else
	    p.serdez_op = 0;
	  p.peer_guid = ii.peer_guid;
	  p.peer_port_idx = ii.peer_port_idx;
	  p.indirect_port_idx = ii.indirect_port_idx;
	  p.is_indirect_port = false;  // we'll set these below as needed
	  p.needs_pbt_update.store(false);  // never needed for inputs
	  p.local_bytes_total = 0;
	  p.local_bytes_cons.store(0);
	  p.remote_bytes_total.store(size_t(-1));
	  p.ib_offset = ii.ib_offset;
	  p.ib_size = ii.ib_size;
	  p.addrcursor.set_addrlist(&p.addrlist);
	  switch(ii.port_type) {
	  case XferDesPortInfo::GATHER_CONTROL_PORT:
	    gather_control_port = i; break;
	  case XferDesPortInfo::SCATTER_CONTROL_PORT:
	    scatter_control_port = i; break;
	  default: break;
	  }
	}
	// connect up indirect input ports in a second pass
	for(size_t i = 0; i < inputs_info.size(); i++) {
	  XferPort& p = input_ports[i];
	  if(p.indirect_port_idx >= 0) {
	    p.iter->set_indirect_input_port(this, p.indirect_port_idx,
					    input_ports[p.indirect_port_idx].iter);
	    input_ports[p.indirect_port_idx].is_indirect_port = true;
	  }
	}
	if(gather_control_port >= 0) {
	  input_control.control_port_idx = gather_control_port;
	  input_control.current_io_port = 0;
	  input_control.remaining_count = 0;
	  input_control.eos_received = false;
	} else {
	  input_control.control_port_idx = -1;
	  input_control.current_io_port = 0;
	  input_control.remaining_count = size_t(-1);
	  input_control.eos_received = false;
	}

	output_ports.resize(outputs_info.size());
	for(size_t i = 0; i < outputs_info.size(); i++) {
	  XferPort& p = output_ports[i];
	  const XferDesPortInfo& oi = outputs_info[i];

	  p.mem = get_runtime()->get_memory_impl(oi.mem);
	  p.iter = oi.iter;
	  if(oi.serdez_id != 0) {
	    const CustomSerdezUntyped *op = get_runtime()->custom_serdez_table.get(oi.serdez_id, 0);
	    assert(op != 0);
	    p.serdez_op = op;
	  } else
	    p.serdez_op = 0;
	  p.peer_guid = oi.peer_guid;
	  p.peer_port_idx = oi.peer_port_idx;
	  p.indirect_port_idx = oi.indirect_port_idx;
	  p.is_indirect_port = false;  // outputs are never indirections
	  if(oi.indirect_port_idx >= 0) {
	    p.iter->set_indirect_input_port(this, oi.indirect_port_idx,
					    inputs_info[oi.indirect_port_idx].iter);
	    input_ports[p.indirect_port_idx].is_indirect_port = true;
	  }
	  // TODO: further refine this to exclude peers that can figure out
	  //  the end of a tranfer some othe way
	  p.needs_pbt_update.store(oi.peer_guid != XFERDES_NO_GUID);
	  p.local_bytes_total = 0;
	  p.local_bytes_cons.store(0);
	  p.remote_bytes_total.store(size_t(-1));
	  p.ib_offset = oi.ib_offset;
	  p.ib_size = oi.ib_size;
	  p.addrcursor.set_addrlist(&p.addrlist);

	  // if we're writing into an IB, the first 'ib_size' byte
	  //  locations can be freely written
	  if(p.ib_size > 0)
	    p.seq_remote.add_span(0, p.ib_size);
	}

	if(scatter_control_port >= 0) {
	  output_control.control_port_idx = scatter_control_port;
	  output_control.current_io_port = 0;
	  output_control.remaining_count = 0;
	  output_control.eos_received = false;
	} else {
	  output_control.control_port_idx = -1;
	  output_control.current_io_port = 0;
	  output_control.remaining_count = size_t(-1);
	  output_control.eos_received = false;
	}
      }

      XferDes::~XferDes() {
        // clear available_reqs
        while (!available_reqs.empty()) {
          available_reqs.pop();
        }
	for(std::vector<XferPort>::const_iterator it = input_ports.begin();
	    it != input_ports.end();
	    ++it)
	  delete it->iter;
	for(std::vector<XferPort>::const_iterator it = output_ports.begin();
	    it != output_ports.end();
	    ++it)
	  delete it->iter;
      };

      Event XferDes::request_metadata()
      {
	std::vector<Event> preconditions;
	for(std::vector<XferPort>::iterator it = input_ports.begin();
	    it != input_ports.end();
	    ++it) {
	  Event e = it->iter->request_metadata();
	  if(!e.has_triggered())
	    preconditions.push_back(e);
	}
	for(std::vector<XferPort>::iterator it = output_ports.begin();
	    it != output_ports.end();
	    ++it) {
	  Event e = it->iter->request_metadata();
	  if(!e.has_triggered())
	    preconditions.push_back(e);
	}
	return Event::merge_events(preconditions);
      }
  
      void XferDes::mark_completed() {
	for(std::vector<XferPort>::const_iterator it = input_ports.begin();
	    it != input_ports.end();
	    ++it)
	  if(it->ib_size > 0)
	    free_intermediate_buffer(dma_request,
				     it->mem->me,
				     it->ib_offset,
				     it->ib_size);

        // notify owning DmaRequest upon completion of this XferDes
        //printf("complete XD = %lu\n", guid);
        if (launch_node == Network::my_node_id) {
          complete_fence->mark_finished(true/*successful*/);
        } else {
          NotifyXferDesCompleteMessage::send_request(launch_node, complete_fence);
        }
      }

#if 0
      static inline off_t calc_mem_loc_ib(off_t alloc_offset,
                                          off_t field_start,
                                          int field_size,
                                          size_t elmt_size,
                                          size_t block_size,
                                          size_t buf_size,
                                          size_t domain_size,
                                          off_t index)
      {
        off_t idx2 = domain_size / block_size * block_size;
        off_t offset;
        if (index < idx2) {
          offset = Realm::calc_mem_loc(alloc_offset, field_start, field_size, elmt_size, block_size, index);
        } else {
          offset = (alloc_offset + field_start * domain_size + (elmt_size - field_start) * idx2 + (index - idx2) * field_size);
        }
	// the final step is to wrap the offset around within the buf_size
	//  (i.e. offset %= buf_size), but that is done by the caller after
	//  checking flow control limits
        return offset;
      }
#endif

#define MAX_GEN_REQS 3

      bool support_2d_xfers(XferDesKind kind)
      {
        return (kind == XFER_GPU_TO_FB)
               || (kind == XFER_GPU_FROM_FB)
               || (kind == XFER_GPU_IN_FB)
               || (kind == XFER_GPU_PEER_FB)
               || (kind == XFER_REMOTE_WRITE)
               || (kind == XFER_MEM_CPY);
      }

  size_t XferDes::update_control_info(ReadSequenceCache *rseqcache)
  {
    // pull control information if we need it
    if(input_control.remaining_count == 0) {
      XferPort& icp = input_ports[input_control.control_port_idx];
      size_t avail = icp.seq_remote.span_exists(icp.local_bytes_total,
						sizeof(unsigned));
      if(avail < sizeof(unsigned))
	return 0;  // no data right now

      TransferIterator::AddressInfo c_info;
      size_t amt = icp.iter->step(sizeof(unsigned), c_info, 0, false /*!tentative*/);
      assert(amt == sizeof(unsigned));
      const void *srcptr = icp.mem->get_direct_ptr(c_info.base_offset, amt);
      assert(srcptr != 0);
      unsigned cword;
      memcpy(&cword, srcptr, sizeof(unsigned));
      if(rseqcache != 0)
	rseqcache->add_span(input_control.control_port_idx,
			    icp.local_bytes_total, sizeof(unsigned));
      else
	update_bytes_read(input_control.control_port_idx,
			  icp.local_bytes_total, sizeof(unsigned));
      icp.local_bytes_total += sizeof(unsigned);
      input_control.remaining_count = cword >> 8;
      input_control.current_io_port = (cword & 0x7f) - 1;
      input_control.eos_received = (cword & 128) != 0;
      log_xd.info() << "input control: xd=" << std::hex << guid << std::dec
		    << " port=" << input_control.current_io_port
		    << " count=" << input_control.remaining_count
		    << " done=" << input_control.eos_received;
      // if count is still zero, we're done
      if(input_control.remaining_count == 0) {
	assert(input_control.eos_received);
	iteration_completed.store_release(true);
	return 0;
      }
    }

    if(output_control.remaining_count == 0) {
      // this looks wrong, but the port that controls the output is
      //  an input port! vvv
      XferPort& ocp = input_ports[output_control.control_port_idx];
      size_t avail = ocp.seq_remote.span_exists(ocp.local_bytes_total,
						sizeof(unsigned));
      if(avail < sizeof(unsigned))
	return 0;  // no data right now
      TransferIterator::AddressInfo c_info;
      size_t amt = ocp.iter->step(sizeof(unsigned), c_info, 0, false /*!tentative*/);
      assert(amt == sizeof(unsigned));
      const void *srcptr = ocp.mem->get_direct_ptr(c_info.base_offset, amt);
      assert(srcptr != 0);

      unsigned cword;
      memcpy(&cword, srcptr, sizeof(unsigned));
      if(rseqcache != 0)
	rseqcache->add_span(output_control.control_port_idx,
			    ocp.local_bytes_total, sizeof(unsigned));
      else
	update_bytes_read(output_control.control_port_idx,
			  ocp.local_bytes_total, sizeof(unsigned));
      ocp.local_bytes_total += sizeof(unsigned);
      assert(cword != 0);
      output_control.remaining_count = cword >> 8;
      output_control.current_io_port = (cword & 0x7f) - 1;
      output_control.eos_received = (cword & 128) != 0;
      log_xd.info() << "output control: xd=" << std::hex << guid << std::dec
		    << " port=" << output_control.current_io_port
		    << " count=" << output_control.remaining_count
		    << " done=" << output_control.eos_received;
      // if count is still zero, we're done
      if(output_control.remaining_count == 0) {
	assert(output_control.eos_received);
	iteration_completed.store_release(true);
	// give all output channels a chance to indicate completion
	for(size_t i = 0; i < output_ports.size(); i++)
	  update_bytes_write(i, output_ports[i].local_bytes_total, 0);
	return 0;
      }
    }

    return std::min(input_control.remaining_count,
		    output_control.remaining_count);
  }
  
  size_t XferDes::get_addresses(size_t min_xfer_size,
				ReadSequenceCache *rseqcache)
  {
    size_t control_count = update_control_info(rseqcache);
    if(control_count == 0)
      return 0;
    if(control_count < min_xfer_size)
      min_xfer_size = control_count;
    size_t max_bytes = control_count;

    // get addresses for the input, if it exists
    if(input_control.current_io_port >= 0) {
      XferPort *in_port = &input_ports[input_control.current_io_port];

      // do we need more addresses?
      size_t read_bytes_avail = in_port->addrlist.bytes_pending();
      if(read_bytes_avail < min_xfer_size) {
	if(in_port->iter->get_addresses(in_port->addrlist)) {
	  // adjust min size to flush as requested
	  min_xfer_size = std::min(min_xfer_size,
				   in_port->addrlist.bytes_pending());
	}
	read_bytes_avail = in_port->addrlist.bytes_pending();
      }

      // if we're not the first in the chain, respect flow control too
      if(in_port->peer_guid != XFERDES_NO_GUID) {
	read_bytes_avail = in_port->seq_remote.span_exists(in_port->local_bytes_total,
							   read_bytes_avail);
	size_t pbt_limit = (in_port->remote_bytes_total.load_acquire() -
			    in_port->local_bytes_total);
	min_xfer_size = std::min(min_xfer_size, pbt_limit);
      }

      // we'd like to wait until there's `min_xfer_size` bytes available on the
      //  input, but in gather copies with fork-joins in the dataflow, we
      //  can't be guaranteed that's possible, so move whatever we've got,
      //  relying on the upstream producer to be producing it in the largest
      //  chunks it can
      if((read_bytes_avail > 0) && (read_bytes_avail < min_xfer_size))
	min_xfer_size = read_bytes_avail;

      max_bytes = std::min(max_bytes, read_bytes_avail);
    }

    // get addresses for the output, if it exists
    if(output_control.current_io_port >= 0) {
      XferPort *out_port = &output_ports[output_control.current_io_port];

      // do we need more addresses?
      size_t write_bytes_avail = out_port->addrlist.bytes_pending();
      if(write_bytes_avail < min_xfer_size) {
	if(out_port->iter->get_addresses(out_port->addrlist)) {
	  // adjust min size to flush as requested
	  min_xfer_size = std::min(min_xfer_size,
				   out_port->addrlist.bytes_pending());
	}
	write_bytes_avail = out_port->addrlist.bytes_pending();
      }

      // if we're not the last in the chain, respect flow control too
      if(out_port->peer_guid != XFERDES_NO_GUID) {
	write_bytes_avail = out_port->seq_remote.span_exists(out_port->local_bytes_total,
							     write_bytes_avail);
      }

      max_bytes = std::min(max_bytes, write_bytes_avail);
    }

    if(min_xfer_size == 0) {
      // should only happen in the absence of control ports
      assert((input_control.control_port_idx == -1) &&
	     (output_control.control_port_idx == -1));
      iteration_completed.store_release(true);
      return 0;
    }

    // if we don't have a big enough chunk, wait for more to show up
    if(max_bytes < min_xfer_size)
      return 0;

    return max_bytes;
  }

  bool XferDes::record_address_consumption(size_t total_bytes)
  {
    bool in_done = false;
    if(input_control.current_io_port >= 0) {
      XferPort *in_port = &input_ports[input_control.current_io_port];

      in_port->local_bytes_total += total_bytes;
      in_port->local_bytes_cons.fetch_add(total_bytes);

      if(in_port->peer_guid == XFERDES_NO_GUID)
	in_done = ((in_port->addrlist.bytes_pending() == 0) &&
		   in_port->iter->done());
      else
	in_done = (in_port->local_bytes_total ==
		   in_port->remote_bytes_total.load_acquire());
    }

    bool out_done = false;
    if(output_control.current_io_port >= 0) {
      XferPort *out_port = &output_ports[output_control.current_io_port];

      out_port->local_bytes_total += total_bytes;
      out_port->local_bytes_cons.fetch_add(total_bytes);

      if(out_port->peer_guid == XFERDES_NO_GUID)
	out_done = ((out_port->addrlist.bytes_pending() == 0) &&
		    out_port->iter->done());
    }
	  
    input_control.remaining_count -= total_bytes;
    output_control.remaining_count -= total_bytes;

    // input or output controls override our notion of done-ness
    if(input_control.control_port_idx >= 0)
      in_done = ((input_control.remaining_count == 0) &&
		 input_control.eos_received);

    if(output_control.control_port_idx >= 0)
      out_done = ((output_control.remaining_count == 0) &&
		  output_control.eos_received);
	  
    if(in_done || out_done) {
      iteration_completed.store_release(true);
      return true;
    } else
      return false;
  }


    long XferDes::default_get_requests(Request** reqs, long nr,
				       unsigned flags)
      {
        long idx = 0;
	
	while((idx < nr) && request_available()) {
	  // TODO: we really shouldn't even be trying if the iteration
	  //   is already done
	  if(iteration_completed.load()) break;

	  // pull control information if we need it
	  if(input_control.remaining_count == 0) {
	    XferPort& icp = input_ports[input_control.control_port_idx];
	    size_t avail = icp.seq_remote.span_exists(icp.local_bytes_total,
						      sizeof(unsigned));
	    if(avail < sizeof(unsigned))
	      break;  // no data right now
	    TransferIterator::AddressInfo c_info;
	    size_t amt = icp.iter->step(sizeof(unsigned), c_info, 0, false /*!tentative*/);
	    assert(amt == sizeof(unsigned));
	    const void *srcptr = icp.mem->get_direct_ptr(c_info.base_offset, amt);
	    assert(srcptr != 0);
	    unsigned cword;
	    memcpy(&cword, srcptr, sizeof(unsigned));
	    update_bytes_read(input_control.control_port_idx,
                              icp.local_bytes_total, sizeof(unsigned));
	    icp.local_bytes_total += sizeof(unsigned);
	    input_control.remaining_count = cword >> 8;
	    input_control.current_io_port = (cword & 0x7f) - 1;
	    input_control.eos_received = (cword & 128) != 0;
	    log_xd.info() << "input control: xd=" << std::hex << guid << std::dec
			  << " port=" << input_control.current_io_port
			  << " count=" << input_control.remaining_count
			  << " done=" << input_control.eos_received;
	    // if count is still zero, we're done
	    if(input_control.remaining_count == 0) {
	      assert(input_control.eos_received);
	      iteration_completed.store_release(true);
	      break;
	    }
	  }
	  if(output_control.remaining_count == 0) {
	    // this looks wrong, but the port that controls the output is
	    //  an input port! vvv
	    XferPort& ocp = input_ports[output_control.control_port_idx];
	    size_t avail = ocp.seq_remote.span_exists(ocp.local_bytes_total,
						      sizeof(unsigned));
	    if(avail < sizeof(unsigned))
	      break;  // no data right now
	    TransferIterator::AddressInfo c_info;
	    size_t amt = ocp.iter->step(sizeof(unsigned), c_info, 0, false /*!tentative*/);
	    assert(amt == sizeof(unsigned));
	    const void *srcptr = ocp.mem->get_direct_ptr(c_info.base_offset, amt);
	    assert(srcptr != 0);

	    unsigned cword;
	    memcpy(&cword, srcptr, sizeof(unsigned));
	    update_bytes_read(output_control.control_port_idx,
                              ocp.local_bytes_total, sizeof(unsigned));
	    ocp.local_bytes_total += sizeof(unsigned);
	    assert(cword != 0);
	    output_control.remaining_count = cword >> 8;
	    output_control.current_io_port = (cword & 0x7f) - 1;
	    output_control.eos_received = (cword & 128) != 0;
	    log_xd.info() << "output control: xd=" << std::hex << guid << std::dec
			  << " port=" << output_control.current_io_port
			  << " count=" << output_control.remaining_count
			  << " done=" << output_control.eos_received;
	    // if count is still zero, we're done
	    if(output_control.remaining_count == 0) {
	      assert(output_control.eos_received);
	      iteration_completed.store_release(true);
	      // give all output channels a chance to indicate completion
	      for(size_t i = 0; i < output_ports.size(); i++)
		update_bytes_write(i, output_ports[i].local_bytes_total, 0);
	      break;
	    }
	  }

	  XferPort *in_port = ((input_control.current_io_port >= 0) ?
			         &input_ports[input_control.current_io_port] :
			         0);
	  XferPort *out_port = ((output_control.current_io_port >= 0) ?
				  &output_ports[output_control.current_io_port] :
				  0);

	  // special cases for OOR scatter/gather
	  if(!in_port) {
	    if(!out_port) {
	      // no input or output?  just skip the count?
	      assert(0);
	    } else {
	      // no valid input, so no write to the destination -
	      //  just step the output transfer iterator if it's a real target
	      //  but barf if it's an IB
	      assert((out_port->peer_guid == XferDes::XFERDES_NO_GUID) &&
		     !out_port->serdez_op);
	      TransferIterator::AddressInfo dummy;
	      size_t skip_bytes = out_port->iter->step(std::min(input_control.remaining_count,
								output_control.remaining_count),
						       dummy,
						       flags & TransferIterator::DST_FLAGMASK,
						       false /*!tentative*/);
	      log_xd.debug() << "skipping " << skip_bytes << " bytes of output";
	      assert(skip_bytes > 0);
	      input_control.remaining_count -= skip_bytes;
	      output_control.remaining_count -= skip_bytes;
	      // TODO: pull this code out to a common place?
	      if(((input_control.remaining_count == 0) && input_control.eos_received) ||
		 ((output_control.remaining_count == 0) && output_control.eos_received)) {
		log_xd.info() << "iteration completed via control port: xd=" << std::hex << guid << std::dec;
		iteration_completed.store_release(true);
		// give all output channels a chance to indicate completion
		for(size_t i = 0; i < output_ports.size(); i++)
		  update_bytes_write(i, output_ports[i].local_bytes_total, 0);
		break;
	      }
	      continue;  // try again
	    }
	  } else if(!out_port) {
	    // valid input that we need to throw away
	    assert(!in_port->serdez_op);
	    TransferIterator::AddressInfo dummy;
	    // although we're not reading the IB input data ourselves, we need
	    //  to wait until it's ready before not-reading it to avoid WAW
	    //  races on the producer side
	    size_t skip_bytes = std::min(input_control.remaining_count,
					 output_control.remaining_count);
	    if(in_port->peer_guid != XferDes::XFERDES_NO_GUID) {
	      skip_bytes = in_port->seq_remote.span_exists(in_port->local_bytes_total,
							   skip_bytes);
	      if(skip_bytes == 0) break;
	    }
	    skip_bytes = in_port->iter->step(skip_bytes,
					     dummy,
					     flags & TransferIterator::SRC_FLAGMASK,
					     false /*!tentative*/);
	    log_xd.debug() << "skipping " << skip_bytes << " bytes of input";
	    assert(skip_bytes > 0);
	    update_bytes_read(input_control.current_io_port,
			      in_port->local_bytes_total,
			      skip_bytes);
	    in_port->local_bytes_total += skip_bytes;
	    input_control.remaining_count -= skip_bytes;
	    output_control.remaining_count -= skip_bytes;
	    // TODO: pull this code out to a common place?
	    if(((input_control.remaining_count == 0) && input_control.eos_received) ||
	       ((output_control.remaining_count == 0) && output_control.eos_received)) {
	      log_xd.info() << "iteration completed via control port: xd=" << std::hex << guid << std::dec;
	      iteration_completed.store_release(true);
	      // give all output channels a chance to indicate completion
	      for(size_t i = 0; i < output_ports.size(); i++)
		update_bytes_write(i, output_ports[i].local_bytes_total, 0);
	      break;
	    }
	    continue;  // try again
	  }
	  
	  // there are several variables that can change asynchronously to
	  //  the logic here:
	  //   pre_bytes_total - the max bytes we'll ever see from the input IB
	  //   read_bytes_cons - conservative estimate of bytes we've read
	  //   write_bytes_cons - conservative estimate of bytes we've written
	  //
	  // to avoid all sorts of weird race conditions, sample all three here
	  //  and only use them in the code below (exception: atomic increments
	  //  of rbc or wbc, for which we adjust the snapshot by the same)
	  size_t pbt_snapshot = in_port->remote_bytes_total.load_acquire();
	  size_t rbc_snapshot = in_port->local_bytes_cons.load_acquire();
	  size_t wbc_snapshot = out_port->local_bytes_cons.load_acquire();

	  // normally we detect the end of a transfer after initiating a
	  //  request, but empty iterators and filtered streams can cause us
	  //  to not realize the transfer is done until we are asking for
	  //  the next request (i.e. now)
	  if((in_port->peer_guid == XFERDES_NO_GUID) ?
	       in_port->iter->done() :
	       (in_port->local_bytes_total == pbt_snapshot)) {
	    if(in_port->local_bytes_total == 0)
	      log_request.info() << "empty xferdes: " << guid;
	    // TODO: figure out how to eliminate false positives from these
	    //  checks with indirection and/or multiple remote inputs
#if 0
	    assert((out_port->peer_guid != XFERDES_NO_GUID) ||
		   out_port->iter->done());
#endif

	    iteration_completed.store_release(true);

	    // give all output channels a chance to indicate completion
	    for(size_t i = 0; i < output_ports.size(); i++)
	      update_bytes_write(i, output_ports[i].local_bytes_total, 0);
	    break;
	  }
	  
	  TransferIterator::AddressInfo src_info, dst_info;
	  size_t read_bytes, write_bytes, read_seq, write_seq;
	  size_t write_pad_bytes = 0;
	  size_t read_pad_bytes = 0;

	  // handle serialization-only and deserialization-only cases 
	  //  specially, because they have uncertainty in how much data
	  //  they write or read
	  if(in_port->serdez_op && !out_port->serdez_op) {
	    // serialization only - must be into an IB
	    assert(in_port->peer_guid == XFERDES_NO_GUID);
	    assert(out_port->peer_guid != XFERDES_NO_GUID);

	    // when serializing, we don't know how much output space we're
	    //  going to consume, so do not step the dst_iter here
	    // instead, see what we can get from the source and conservatively
	    //  check flow control on the destination and let the stepping
	    //  of dst_iter happen in the actual execution of the request

	    // if we don't have space to write a single worst-case
	    //  element, try again later
	    if(out_port->seq_remote.span_exists(wbc_snapshot,
						in_port->serdez_op->max_serialized_size) <
	       in_port->serdez_op->max_serialized_size)
	      break;

	    size_t max_bytes = max_req_size;

	    size_t src_bytes = in_port->iter->step(max_bytes, src_info,
						   flags & TransferIterator::SRC_FLAGMASK,
						   true /*tentative*/);

	    size_t num_elems = src_bytes / in_port->serdez_op->sizeof_field_type;
	    // no input data?  try again later
	    if(num_elems == 0)
	      break;
	    assert((num_elems * in_port->serdez_op->sizeof_field_type) == src_bytes);
	    size_t max_dst_bytes = num_elems * in_port->serdez_op->max_serialized_size;

	    // if we have an output control, restrict the max number of
	    //  elements
	    if(output_control.control_port_idx >= 0) {
	      if(num_elems > output_control.remaining_count) {
		log_xd.info() << "scatter/serialize clamp: " << num_elems << " -> " << output_control.remaining_count;
		num_elems = output_control.remaining_count;
	      }
	    }

	    size_t clamp_dst_bytes = num_elems * in_port->serdez_op->max_serialized_size;
	    // test for space using our conserative bytes written count
	    size_t dst_bytes_avail = out_port->seq_remote.span_exists(wbc_snapshot,
								      clamp_dst_bytes);

	    if(dst_bytes_avail == max_dst_bytes) {
	      // enough space - confirm the source step
	      in_port->iter->confirm_step();
	    } else {
	      // not enough space - figure out how many elements we can
	      //  actually take and adjust the source step
	      size_t act_elems = dst_bytes_avail / in_port->serdez_op->max_serialized_size;
	      // if there was a remainder in the division, get rid of it
	      dst_bytes_avail = act_elems * in_port->serdez_op->max_serialized_size;
	      size_t new_src_bytes = act_elems * in_port->serdez_op->sizeof_field_type;
	      in_port->iter->cancel_step();
	      src_bytes = in_port->iter->step(new_src_bytes, src_info,
					      flags & TransferIterator::SRC_FLAGMASK,
					 false /*!tentative*/);
	      // this can come up shorter than we expect if the source
	      //  iterator is 2-D or 3-D - if that happens, re-adjust the
	      //  dest bytes again
	      if(src_bytes < new_src_bytes) {
		if(src_bytes == 0) break;

		num_elems = src_bytes / in_port->serdez_op->sizeof_field_type;
		assert((num_elems * in_port->serdez_op->sizeof_field_type) == src_bytes);

		// no need to recheck seq_next_read
		dst_bytes_avail = num_elems * in_port->serdez_op->max_serialized_size;
	      }
	    }

	    // since the dst_iter will be stepped later, the dst_info is a 
	    //  don't care, so copy the source so that lines/planes/etc match
	    //  up
	    dst_info = src_info;

	    read_seq = in_port->local_bytes_total;
	    read_bytes = src_bytes;
	    in_port->local_bytes_total += src_bytes;

	    write_seq = 0; // filled in later
	    write_bytes = dst_bytes_avail;
	    out_port->local_bytes_cons.fetch_add(dst_bytes_avail);
	    wbc_snapshot += dst_bytes_avail;
	  } else
	  if(!in_port->serdez_op && out_port->serdez_op) {
	    // deserialization only - must be from an IB
	    assert(in_port->peer_guid != XFERDES_NO_GUID);
	    assert(out_port->peer_guid == XFERDES_NO_GUID);

	    // when deserializing, we don't know how much input data we need
	    //  for each element, so do not step the src_iter here
	    //  instead, see what the destination wants
	    // if the transfer is still in progress (i.e. pre_bytes_total
	    //  hasn't been set), we have to be conservative about how many
	    //  elements we can get from partial data

	    // input data is done only if we know the limit AND we have all
	    //  the remaining bytes (if any) up to that limit
	    bool input_data_done = ((pbt_snapshot != size_t(-1)) &&
				    ((rbc_snapshot >= pbt_snapshot) ||
				     (in_port->seq_remote.span_exists(rbc_snapshot,
								      pbt_snapshot - rbc_snapshot) ==
				      (pbt_snapshot - rbc_snapshot))));
	    // if we're using an input control and it's not at the end of the
	    //  stream, the above checks may not be precise
	    if((input_control.control_port_idx >= 0) &&
	       !input_control.eos_received)
	      input_data_done = false;

	    // this done-ness overrides many checks based on the conservative
	    //  out_port->serdez_op->max_serialized_size
	    if(!input_data_done) {
	      // if we don't have enough input data for a single worst-case
	      //  element, try again later
	      if((in_port->seq_remote.span_exists(rbc_snapshot,
						  out_port->serdez_op->max_serialized_size) <
		  out_port->serdez_op->max_serialized_size)) {
		break;
	      }
	    }

	    size_t max_bytes = max_req_size;

	    size_t dst_bytes = out_port->iter->step(max_bytes, dst_info,
						    flags & TransferIterator::DST_FLAGMASK,
						    !input_data_done);

	    size_t num_elems = dst_bytes / out_port->serdez_op->sizeof_field_type;
	    if(num_elems == 0) break;
	    assert((num_elems * out_port->serdez_op->sizeof_field_type) == dst_bytes);
	    size_t max_src_bytes = num_elems * out_port->serdez_op->max_serialized_size;
	    // if we have an input control, restrict the max number of
	    //  elements
	    if(input_control.control_port_idx >= 0) {
	      if(num_elems > input_control.remaining_count) {
		log_xd.info() << "gather/deserialize clamp: " << num_elems << " -> " << input_control.remaining_count;
		num_elems = input_control.remaining_count;
	      }
	    }

	    size_t clamp_src_bytes = num_elems * out_port->serdez_op->max_serialized_size;
	    size_t src_bytes_avail;
	    if(input_data_done) {
	      // we're certainty to have all the remaining data, so keep
	      //  the limit at max_src_bytes - we won't actually overshoot
	      //  (unless the serialized data is corrupted)
	      src_bytes_avail = max_src_bytes;
	    } else {
	      // test for space using our conserative bytes read count
	      src_bytes_avail = in_port->seq_remote.span_exists(rbc_snapshot,
								clamp_src_bytes);

	      if(src_bytes_avail == max_src_bytes) {
		// enough space - confirm the dest step
		out_port->iter->confirm_step();
	      } else {
		log_request.info() << "pred limits deserialize: " << max_src_bytes << " -> " << src_bytes_avail;
		// not enough space - figure out how many elements we can
		//  actually read and adjust the dest step
		size_t act_elems = src_bytes_avail / out_port->serdez_op->max_serialized_size;
		// if there was a remainder in the division, get rid of it
		src_bytes_avail = act_elems * out_port->serdez_op->max_serialized_size;
		size_t new_dst_bytes = act_elems * out_port->serdez_op->sizeof_field_type;
		out_port->iter->cancel_step();
		dst_bytes = out_port->iter->step(new_dst_bytes, dst_info,
						 flags & TransferIterator::SRC_FLAGMASK,
						 false /*!tentative*/);
		// this can come up shorter than we expect if the destination
		//  iterator is 2-D or 3-D - if that happens, re-adjust the
		//  source bytes again
		if(dst_bytes < new_dst_bytes) {
		  if(dst_bytes == 0) break;

		  num_elems = dst_bytes / out_port->serdez_op->sizeof_field_type;
		  assert((num_elems * out_port->serdez_op->sizeof_field_type) == dst_bytes);

		  // no need to recheck seq_pre_write
		  src_bytes_avail = num_elems * out_port->serdez_op->max_serialized_size;
		}
	      }
	    }

	    // since the src_iter will be stepped later, the src_info is a 
	    //  don't care, so copy the source so that lines/planes/etc match
	    //  up
	    src_info = dst_info;

	    read_seq = 0; // filled in later
	    read_bytes = src_bytes_avail;
	    in_port->local_bytes_cons.fetch_add(src_bytes_avail);
	    rbc_snapshot += src_bytes_avail;

	    write_seq = out_port->local_bytes_total;
	    write_bytes = dst_bytes;
	    out_port->local_bytes_total += dst_bytes;
	    out_port->local_bytes_cons.store(out_port->local_bytes_total); // completion detection uses this
	  } else {
	    // either no serialization or simultaneous serdez

	    // limit transfer based on the max request size, or the largest
	    //  amount of data allowed by the control port(s)
	    size_t max_bytes = std::min(size_t(max_req_size),
					std::min(input_control.remaining_count,
						 output_control.remaining_count));

	    // if we're not the first in the chain, and we know the total bytes
	    //  written by the predecessor, don't exceed that
	    if(in_port->peer_guid != XFERDES_NO_GUID) {
	      size_t pre_max = pbt_snapshot - in_port->local_bytes_total;
	      if(pre_max == 0) {
		// should not happen with snapshots
		assert(0);
		// due to unsynchronized updates to pre_bytes_total, this path
		//  can happen for an empty transfer reading from an intermediate
		//  buffer - handle it by looping around and letting the check
		//  at the top of the loop notice it the second time around
		if(in_port->local_bytes_total == 0)
		  continue;
		// otherwise, this shouldn't happen - we should detect this case
		//  on the the transfer of those last bytes
		assert(0);
		iteration_completed.store_release(true);
		break;
	      }
	      if(pre_max < max_bytes) {
		log_request.info() << "pred limits xfer: " << max_bytes << " -> " << pre_max;
		max_bytes = pre_max;
	      }

	      // further limit by bytes we've actually received
	      max_bytes = in_port->seq_remote.span_exists(in_port->local_bytes_total, max_bytes);
	      if(max_bytes == 0) {
		// TODO: put this XD to sleep until we do have data
		break;
	      }
	    }

	    if(out_port->peer_guid != XFERDES_NO_GUID) {
	      // if we're writing to an intermediate buffer, make sure to not
	      //  overwrite previously written data that has not been read yet
	      max_bytes = out_port->seq_remote.span_exists(out_port->local_bytes_total, max_bytes);
	      if(max_bytes == 0) {
		// TODO: put this XD to sleep until we do have data
		break;
	      }
	    }

	    // tentatively get as much as we can from the source iterator
	    size_t src_bytes = in_port->iter->step(max_bytes, src_info,
						   flags & TransferIterator::SRC_FLAGMASK,
						   true /*tentative*/);
	    if(src_bytes == 0) {
	      // not enough space for even one element
	      // TODO: put this XD to sleep until we do have data
	      break;
	    }

	    // destination step must be tentative for an non-IB source or
	    //  target that might collapse dimensions differently
	    bool dimension_mismatch_possible = (((in_port->peer_guid == XFERDES_NO_GUID) ||
						 (out_port->peer_guid == XFERDES_NO_GUID)) &&
						((flags & TransferIterator::LINES_OK) != 0));

	    size_t dst_bytes = out_port->iter->step(src_bytes, dst_info,
						    flags & TransferIterator::DST_FLAGMASK,
						    dimension_mismatch_possible);
	    if(dst_bytes == 0) {
	      // not enough space for even one element

	      // if this happens when the input is an IB, the output is not,
	      //  and the input doesn't seem to be limited by max_bytes, this
	      //  is (probably?) the case that requires padding on the input
	      //  side
	      if((in_port->peer_guid != XFERDES_NO_GUID) &&
		 (out_port->peer_guid == XFERDES_NO_GUID) &&
		 (src_bytes < max_bytes)) {
		log_xd.info() << "padding input buffer by " << src_bytes << " bytes";
		src_info.bytes_per_chunk = 0;
		src_info.num_lines = 1;
		src_info.num_planes = 1;
		dst_info.bytes_per_chunk = 0;
		dst_info.num_lines = 1;
		dst_info.num_planes = 1;
		read_pad_bytes = src_bytes;
		src_bytes = 0;
		dimension_mismatch_possible = false;
		// src iterator will be confirmed below
		//in_port->iter->confirm_step();
		// dst didn't actually take a step, so we don't need to cancel it
	      } else {
		in_port->iter->cancel_step();
		// TODO: put this XD to sleep until we do have data
		break;
	      }
	    }

	    // does source now need to be shrunk?
	    if(dst_bytes < src_bytes) {
	      // cancel the src step and try to just step by dst_bytes
	      in_port->iter->cancel_step();
	      // this step must still be tentative if a dimension mismatch is
	      //  posisble
	      src_bytes = in_port->iter->step(dst_bytes, src_info,
					      flags & TransferIterator::SRC_FLAGMASK,
					      dimension_mismatch_possible);
	      if(src_bytes == 0) {
		// corner case that should occur only with a destination 
		//  intermediate buffer - no transfer, but pad to boundary
		//  destination wants as long as we're not being limited by
		//  max_bytes
		assert((in_port->peer_guid == XFERDES_NO_GUID) &&
		       (out_port->peer_guid != XFERDES_NO_GUID));
		if(dst_bytes < max_bytes) {
		  log_xd.info() << "padding output buffer by " << dst_bytes << " bytes";
		  src_info.bytes_per_chunk = 0;
		  src_info.num_lines = 1;
		  src_info.num_planes = 1;
		  dst_info.bytes_per_chunk = 0;
		  dst_info.num_lines = 1;
		  dst_info.num_planes = 1;
		  write_pad_bytes = dst_bytes;
		  dst_bytes = 0;
		  dimension_mismatch_possible = false;
		  // src didn't actually take a step, so we don't need to cancel it
		  out_port->iter->confirm_step();
		} else {
		  // retry later
		  // src didn't actually take a step, so we don't need to cancel it
		  out_port->iter->cancel_step();
		  break;
		}
	      }
	      // a mismatch is still possible if the source is 2+D and the
	      //  destination wants to stop mid-span
	      if(src_bytes < dst_bytes) {
		assert(dimension_mismatch_possible);
		out_port->iter->cancel_step();
		dst_bytes = out_port->iter->step(src_bytes, dst_info,
						 flags & TransferIterator::DST_FLAGMASK,
						 true /*tentative*/);
	      }
	      // byte counts now must match
	      assert(src_bytes == dst_bytes);
	    } else {
	      // in the absense of dimension mismatches, it's safe now to confirm
	      //  the source step
	      if(!dimension_mismatch_possible)
		in_port->iter->confirm_step();
	    }

	    // when 2D transfers are allowed, it is possible that the
	    // bytes_per_chunk don't match, and we need to add an extra
	    //  dimension to one side or the other
	    // NOTE: this transformation can cause the dimensionality of the
	    //  transfer to grow.  Allow this to happen and detect it at the
	    //  end.
	    if(!dimension_mismatch_possible) {
	      assert(src_info.bytes_per_chunk == dst_info.bytes_per_chunk);
	      assert(src_info.num_lines == 1);
	      assert(src_info.num_planes == 1);
	      assert(dst_info.num_lines == 1);
	      assert(dst_info.num_planes == 1);
	    } else {
	      // track how much of src and/or dst is "lost" into a 4th
	      //  dimension
	      size_t src_4d_factor = 1;
	      size_t dst_4d_factor = 1;
	      if(src_info.bytes_per_chunk < dst_info.bytes_per_chunk) {
		size_t ratio = dst_info.bytes_per_chunk / src_info.bytes_per_chunk;
		assert((src_info.bytes_per_chunk * ratio) == dst_info.bytes_per_chunk);
		dst_4d_factor *= dst_info.num_planes; // existing planes lost
		dst_info.num_planes = dst_info.num_lines;
		dst_info.plane_stride = dst_info.line_stride;
		dst_info.num_lines = ratio;
		dst_info.line_stride = src_info.bytes_per_chunk;
		dst_info.bytes_per_chunk = src_info.bytes_per_chunk;
	      }
	      if(dst_info.bytes_per_chunk < src_info.bytes_per_chunk) {
		size_t ratio = src_info.bytes_per_chunk / dst_info.bytes_per_chunk;
		assert((dst_info.bytes_per_chunk * ratio) == src_info.bytes_per_chunk);
		src_4d_factor *= src_info.num_planes; // existing planes lost
		src_info.num_planes = src_info.num_lines;
		src_info.plane_stride = src_info.line_stride;
		src_info.num_lines = ratio;
		src_info.line_stride = dst_info.bytes_per_chunk;
		src_info.bytes_per_chunk = dst_info.bytes_per_chunk;
	      }
	  
	      // similarly, if the number of lines doesn't match, we need to promote
	      //  one of the requests from 2D to 3D
	      if(src_info.num_lines < dst_info.num_lines) {
		size_t ratio = dst_info.num_lines / src_info.num_lines;
		assert((src_info.num_lines * ratio) == dst_info.num_lines);
		dst_4d_factor *= dst_info.num_planes; // existing planes lost
		dst_info.num_planes = ratio;
		dst_info.plane_stride = dst_info.line_stride * src_info.num_lines;
		dst_info.num_lines = src_info.num_lines;
	      }
	      if(dst_info.num_lines < src_info.num_lines) {
		size_t ratio = src_info.num_lines / dst_info.num_lines;
		assert((dst_info.num_lines * ratio) == src_info.num_lines);
		src_4d_factor *= src_info.num_planes; // existing planes lost
		src_info.num_planes = ratio;
		src_info.plane_stride = src_info.line_stride * dst_info.num_lines;
		src_info.num_lines = dst_info.num_lines;
	      }

	      // sanity-checks: src/dst should match on lines/planes and we
	      //  shouldn't have multiple planes if we don't have multiple lines
	      assert(src_info.num_lines == dst_info.num_lines);
	      assert((src_info.num_planes * src_4d_factor) == 
		     (dst_info.num_planes * dst_4d_factor));
	      assert((src_info.num_lines > 1) || (src_info.num_planes == 1));
	      assert((dst_info.num_lines > 1) || (dst_info.num_planes == 1));

	      // only do as many planes as both src and dst can manage
	      if(src_info.num_planes > dst_info.num_planes)
		src_info.num_planes = dst_info.num_planes;
	      else
		dst_info.num_planes = src_info.num_planes;

	      // if 3D isn't allowed, set num_planes back to 1
	      if((flags & TransferIterator::PLANES_OK) == 0) {
		src_info.num_planes = 1;
		dst_info.num_planes = 1;
	      }

	      // now figure out how many bytes we're actually able to move and
	      //  if it's less than what we got from the iterators, try again
	      size_t act_bytes = (src_info.bytes_per_chunk *
				  src_info.num_lines *
				  src_info.num_planes);
	      if(act_bytes == src_bytes) {
		// things match up - confirm the steps
		in_port->iter->confirm_step();
		out_port->iter->confirm_step();
	      } else {
		//log_request.info() << "dimension mismatch! " << act_bytes << " < " << src_bytes << " (" << bytes_total << ")";
		TransferIterator::AddressInfo dummy_info;
		in_port->iter->cancel_step();
		src_bytes = in_port->iter->step(act_bytes, dummy_info,
						flags & TransferIterator::SRC_FLAGMASK,
						false /*!tentative*/);
		assert(src_bytes == act_bytes);
		out_port->iter->cancel_step();
		dst_bytes = out_port->iter->step(act_bytes, dummy_info,
						 flags & TransferIterator::DST_FLAGMASK,
						 false /*!tentative*/);
		assert(dst_bytes == act_bytes);
	      }
	    }

	    size_t act_bytes = (src_info.bytes_per_chunk *
				src_info.num_lines *
				src_info.num_planes);
	    read_seq = in_port->local_bytes_total;
	    read_bytes = act_bytes + read_pad_bytes;

	    // update bytes read unless we're using indirection
	    if(in_port->indirect_port_idx < 0) 
	      in_port->local_bytes_total += read_bytes;

	    write_seq = out_port->local_bytes_total;
	    write_bytes = act_bytes + write_pad_bytes;
	    out_port->local_bytes_total += write_bytes;
	    out_port->local_bytes_cons.store(out_port->local_bytes_total); // completion detection uses this
	  }

	  Request* new_req = dequeue_request();
	  new_req->src_port_idx = input_control.current_io_port;
	  new_req->dst_port_idx = output_control.current_io_port;
	  new_req->read_seq_pos = read_seq;
	  new_req->read_seq_count = read_bytes;
	  new_req->write_seq_pos = write_seq;
	  new_req->write_seq_count = write_bytes;
	  new_req->dim = ((src_info.num_planes == 1) ?
			  ((src_info.num_lines == 1) ? Request::DIM_1D :
			                               Request::DIM_2D) :
			                              Request::DIM_3D);
	  new_req->src_off = src_info.base_offset;
	  new_req->dst_off = dst_info.base_offset;
	  new_req->nbytes = src_info.bytes_per_chunk;
	  new_req->nlines = src_info.num_lines;
	  new_req->src_str = src_info.line_stride;
	  new_req->dst_str = dst_info.line_stride;
	  new_req->nplanes = src_info.num_planes;
	  new_req->src_pstr = src_info.plane_stride;
	  new_req->dst_pstr = dst_info.plane_stride;

	  // we can actually hit the end of an intermediate buffer input
	  //  even if our initial pbt_snapshot was (size_t)-1 because
	  //  we use the asynchronously-updated seq_pre_write, so if
	  //  we think we might be done, go ahead and resample here if
	  //  we still have -1
	  if((in_port->peer_guid != XFERDES_NO_GUID) &&
	     (pbt_snapshot == (size_t)-1))
	    pbt_snapshot = in_port->remote_bytes_total.load_acquire();

	  // if we have control ports, they tell us when we're done
	  if((input_control.control_port_idx >= 0) ||
	     (output_control.control_port_idx >= 0)) {
	    // update control port counts, which may also flag a completed iteration
	    size_t input_count = read_bytes - read_pad_bytes;
	    size_t output_count = write_bytes - write_pad_bytes;
	    // if we're serializing or deserializing, we count in elements,
	    //  not bytes
	    if(in_port->serdez_op != 0) {
	      // serializing impacts output size
	      assert((output_count % in_port->serdez_op->max_serialized_size) == 0);
	      output_count /= in_port->serdez_op->max_serialized_size;
	    }
	    if(out_port->serdez_op != 0) {
	      // and deserializing impacts input size
	      assert((input_count % out_port->serdez_op->max_serialized_size) == 0);
	      input_count /= out_port->serdez_op->max_serialized_size;
	    }
	    assert(input_control.remaining_count >= input_count);
	    assert(output_control.remaining_count >= output_count);
	    input_control.remaining_count -= input_count;
	    output_control.remaining_count -= output_count;
	    if(((input_control.remaining_count == 0) && input_control.eos_received) ||
	       ((output_control.remaining_count == 0) && output_control.eos_received)) {
	      log_xd.info() << "iteration completed via control port: xd=" << std::hex << guid << std::dec;
	      iteration_completed.store_release(true);

	      // give all output channels a chance to indicate completion
	      for(size_t i = 0; i < output_ports.size(); i++)
		if(int(i) != output_control.current_io_port)
		  update_bytes_write(i, output_ports[i].local_bytes_total, 0);
#if 0
	      // non-ib iterators should end at the same time?
	      for(size_t i = 0; i < input_ports.size(); i++)
		assert((input_ports[i].peer_guid != XFERDES_NO_GUID) ||
		       input_ports[i].iter->done());
	      for(size_t i = 0; i < output_ports.size(); i++)
		assert((output_ports[i].peer_guid != XFERDES_NO_GUID) ||
		       output_ports[i].iter->done());
#endif
	    }
	  } else {
	    // otherwise, we go by our iterators
	    if(in_port->iter->done() || out_port->iter->done() ||
	       (in_port->local_bytes_total == pbt_snapshot)) {
	      assert(!iteration_completed.load());
	      iteration_completed.store_release(true);
	    
	      // give all output channels a chance to indicate completion
	      for(size_t i = 0; i < output_ports.size(); i++)
		if(int(i) != output_control.current_io_port)
		  update_bytes_write(i, output_ports[i].local_bytes_total, 0);

	      // TODO: figure out how to eliminate false positives from these
	      //  checks with indirection and/or multiple remote inputs
#if 0
	      // non-ib iterators should end at the same time
	      assert((in_port->peer_guid != XFERDES_NO_GUID) || in_port->iter->done());
	      assert((out_port->peer_guid != XFERDES_NO_GUID) || out_port->iter->done());
#endif

	      if(!in_port->serdez_op && out_port->serdez_op) {
		// ok to be over, due to the conservative nature of
		//  deserialization reads
		assert((rbc_snapshot >= pbt_snapshot) ||
		       (pbt_snapshot == size_t(-1)));
	      } else {
		// TODO: this check is now too aggressive because the previous
		//  xd doesn't necessarily know when it's emitting its last
		//  data, which means the update of local_bytes_total might
		//  be delayed
#if 0
		assert((in_port->peer_guid == XFERDES_NO_GUID) ||
		       (pbt_snapshot == in_port->local_bytes_total));
#endif
	      }
	    }
	  }

	  switch(new_req->dim) {
	  case Request::DIM_1D:
	    {
	      log_request.info() << "request: guid=" << std::hex << guid << std::dec
				 << " ofs=" << new_req->src_off << "->" << new_req->dst_off
				 << " len=" << new_req->nbytes;
	      break;
	    }
	  case Request::DIM_2D:
	    {
	      log_request.info() << "request: guid=" << std::hex << guid << std::dec
				 << " ofs=" << new_req->src_off << "->" << new_req->dst_off
				 << " len=" << new_req->nbytes
				 << " lines=" << new_req->nlines << "(" << new_req->src_str << "," << new_req->dst_str << ")";
	      break;
	    }
	  case Request::DIM_3D:
	    {
	      log_request.info() << "request: guid=" << std::hex << guid << std::dec
				 << " ofs=" << new_req->src_off << "->" << new_req->dst_off
				 << " len=" << new_req->nbytes
				 << " lines=" << new_req->nlines << "(" << new_req->src_str << "," << new_req->dst_str << ")"
				 << " planes=" << new_req->nplanes << "(" << new_req->src_pstr << "," << new_req->dst_pstr << ")";
	      break;
	    }
	  }
	  reqs[idx++] = new_req;
	}
#if 0
        coord_t src_idx, dst_idx, todo, src_str, dst_str;
        size_t nitems, nlines;
        while (idx + MAX_GEN_REQS <= nr && offset_idx < oas_vec.size()
        && MAX_GEN_REQS <= available_reqs.size()) {
          if (DIM == 0) {
            todo = std::min((coord_t)(max_req_size / oas_vec[offset_idx].size),
                       me->continuous_steps(src_idx, dst_idx));
            nitems = src_str = dst_str = todo;
            nlines = 1;
          }
          else
            todo = std::min((coord_t)(max_req_size / oas_vec[offset_idx].size),
                       li->continuous_steps(src_idx, dst_idx,
                                            src_str, dst_str,
                                            nitems, nlines));
          coord_t src_in_block = src_buf.block_size
                               - src_idx % src_buf.block_size;
          coord_t dst_in_block = dst_buf.block_size
                               - dst_idx % dst_buf.block_size;
          todo = std::min(todo, std::min(src_in_block, dst_in_block));
          if (todo == 0)
            break;
          coord_t src_start, dst_start;
          if (src_buf.is_ib) {
            src_start = calc_mem_loc_ib(0,
                                        oas_vec[offset_idx].src_offset,
                                        oas_vec[offset_idx].size,
                                        src_buf.elmt_size,
                                        src_buf.block_size,
                                        src_buf.buf_size,
                                        domain.get_volume(), src_idx);
            todo = std::min(todo, std::max((coord_t)0,
					   (coord_t)(pre_bytes_write - src_start))
                                    / oas_vec[offset_idx].size);
	    // wrap src_start around within src_buf if needed
	    src_start %= src_buf.buf_size;
          } else {
            src_start = Realm::calc_mem_loc(0,
                                     oas_vec[offset_idx].src_offset,
                                     oas_vec[offset_idx].size,
                                     src_buf.elmt_size,
                                     src_buf.block_size, src_idx);
          }
          if (dst_buf.is_ib) {
            dst_start = calc_mem_loc_ib(0,
                                        oas_vec[offset_idx].dst_offset,
                                        oas_vec[offset_idx].size,
                                        dst_buf.elmt_size,
                                        dst_buf.block_size,
                                        dst_buf.buf_size,
                                        domain.get_volume(), dst_idx);
            todo = std::min(todo, std::max((coord_t)0,
					   (coord_t)(next_bytes_read + dst_buf.buf_size - dst_start))
                                    / oas_vec[offset_idx].size);
	    // wrap dst_start around within dst_buf if needed
	    dst_start %= dst_buf.buf_size;
          } else {
            dst_start = Realm::calc_mem_loc(0,
                                     oas_vec[offset_idx].dst_offset,
                                     oas_vec[offset_idx].size,
                                     dst_buf.elmt_size,
                                     dst_buf.block_size, dst_idx);
          }
          if (todo == 0)
            break;
          bool cross_src_ib = false, cross_dst_ib = false;
          if (src_buf.is_ib)
            cross_src_ib = cross_ib(src_start,
                                    todo * oas_vec[offset_idx].size,
                                    src_buf.buf_size);
          if (dst_buf.is_ib)
            cross_dst_ib = cross_ib(dst_start,
                                    todo * oas_vec[offset_idx].size,
                                    dst_buf.buf_size);
          // We are crossing ib, fallback to 1d case
          // We don't support 2D, fallback to 1d case
          if (cross_src_ib || cross_dst_ib || !support_2d_xfers(kind))
            todo = std::min(todo, (coord_t)nitems);
          if ((size_t)todo <= nitems) {
            // fallback to 1d case
            nitems = (size_t)todo;
            nlines = 1;
          } else {
            nlines = todo / nitems;
            todo = nlines * nitems;
          }
          if (nlines == 1) {
            // 1D case
            size_t nbytes = todo * oas_vec[offset_idx].size;
            while (nbytes > 0) {
              size_t req_size = nbytes;
              Request* new_req = dequeue_request();
              new_req->dim = Request::DIM_1D;
              if (src_buf.is_ib) {
                src_start = src_start % src_buf.buf_size;
                req_size = std::min(req_size, (size_t)(src_buf.buf_size - src_start));
              }
              if (dst_buf.is_ib) {
                dst_start = dst_start % dst_buf.buf_size;
                req_size = std::min(req_size, (size_t)(dst_buf.buf_size - dst_start));
              }
              new_req->src_off = src_start;
              new_req->dst_off = dst_start;
              new_req->nbytes = req_size;
              new_req->nlines = 1;
              log_request.info("[1D] guid(%llx) src_off(%lld) dst_off(%lld)"
                               " nbytes(%zu) offset_idx(%u)",
                               guid, src_start, dst_start, req_size, offset_idx);
              reqs[idx++] = new_req;
              nbytes -= req_size;
              src_start += req_size;
              dst_start += req_size;
            }
          } else {
            // 2D case
            Request* new_req = dequeue_request();
            new_req->dim = Request::DIM_2D;
            new_req->src_off = src_start;
            new_req->dst_off = dst_start;
            new_req->src_str = src_str * oas_vec[offset_idx].size;
            new_req->dst_str = dst_str * oas_vec[offset_idx].size;
            new_req->nbytes = nitems * oas_vec[offset_idx].size;
            new_req->nlines = nlines;
            reqs[idx++] = new_req;
          }
          if (DIM == 0) {
            me->move(todo);
            if (!me->any_left()) {
              me->reset();
              offset_idx ++;
            }
          } else {
            li->move(todo);
            if (!li->any_left()) {
              li->reset();
              offset_idx ++;
            }
          }
        } // while
#endif
        return idx;
      }


    bool XferDes::is_completed(void)
    {
      // check below is a bit expensive, do don't do it more than once
      if(transfer_completed.load()) return true;
      // to be complete, we need to have finished iterating (which may have been
      //  achieved by getting a pre_bytes_total update) and finished all of our
      //  writes
      // use the conservative byte write count here to make sure we don't
      //  trigger early when serializing
      if(!iteration_completed.load_acquire()) return false;
      for(std::vector<XferPort>::iterator it = output_ports.begin();
	  it != output_ports.end();
	  ++it) {
	// see if we still need to send the total bytes
	if(it->needs_pbt_update.load()) {
#ifdef DEBUG_REALM
	  assert(it->peer_guid != XFERDES_NO_GUID);
#endif
	  // exchange sets the flag to false and tells us previous value
	  if(it->needs_pbt_update.exchange(false))
	    xferDes_queue->update_pre_bytes_total(it->peer_guid,
						  it->peer_port_idx,
						  it->local_bytes_total);
	}
	size_t lbc_snapshot = it->local_bytes_cons.load();
	if(it->seq_local.span_exists(0, lbc_snapshot) != lbc_snapshot)
	  return false;
      }
      transfer_completed.store(true);
      return true;
    }

      void XferDes::update_bytes_read(int port_idx, size_t offset, size_t size)
      {
	XferPort *in_port = &input_ports[port_idx];
	size_t inc_amt = in_port->seq_local.add_span(offset, size);
	log_xd.info() << "bytes_read: " << std::hex << guid << std::dec
		      << "(" << port_idx << ") " << offset << "+" << size << " -> " << inc_amt;
	if(in_port->peer_guid != XFERDES_NO_GUID) {
	  if(inc_amt > 0) {
	    // we're actually telling the previous XD which offsets are ok to
	    //  overwrite, so adjust our offset by our (circular) IB size
            xferDes_queue->update_next_bytes_read(in_port->peer_guid,
						  in_port->peer_port_idx,
						  offset + in_port->ib_size,
						  inc_amt);
	  } else {
	    // TODO: mode to send non-contiguous updates?
	  }
	}
      }

#if 0
      inline void XferDes::simple_update_bytes_read(int64_t offset, uint64_t size)
        //printf("update_read[%lx]: offset = %ld, size = %lu, pre = %lx, next = %lx\n", guid, offset, size, pre_xd_guid, next_xd_guid);
        if (pre_xd_guid != XFERDES_NO_GUID) {
          bool update = false;
          if ((int64_t)(bytes_read % src_buf.buf_size) == offset) {
            bytes_read += size;
            update = true;
          }
          else {
            //printf("[%lx] insert: key = %ld, value = %lu\n", guid, offset, size);
            segments_read[offset] = size;
          }
          std::map<int64_t, uint64_t>::iterator it;
          while (true) {
            it = segments_read.find(bytes_read % src_buf.buf_size);
            if (it == segments_read.end())
              break;
            bytes_read += it->second;
            update = true;
            //printf("[%lx] erase: key = %ld, value = %lu\n", guid, it->first, it->second);
            segments_read.erase(it);
          }
          if (update) {
            xferDes_queue->update_next_bytes_read(pre_xd_guid, bytes_read);
          }
        }
        else {
          bytes_read += size;
        }
      }
#endif

      void XferDes::update_bytes_write(int port_idx, size_t offset, size_t size)
      {
	XferPort *out_port = &output_ports[port_idx];
	size_t inc_amt = out_port->seq_local.add_span(offset, size);
	log_xd.info() << "bytes_write: " << std::hex << guid << std::dec
		      << "(" << port_idx << ") " << offset << "+" << size << " -> " << inc_amt;
	// if our oldest write was ack'd, update progress in case the xd
	//  is just waiting for all writes to complete
	if(inc_amt > 0) update_progress();
	if(out_port->peer_guid != XFERDES_NO_GUID) {
	  // update bytes total if needed (and available)
	  if(out_port->needs_pbt_update.load() &&
	     iteration_completed.load_acquire()) {
	    // exchange sets the flag to false and tells us previous value
	    if(out_port->needs_pbt_update.exchange(false))
	      xferDes_queue->update_pre_bytes_total(out_port->peer_guid,
						    out_port->peer_port_idx,
						    out_port->local_bytes_total);
	  }
	  // we can skip an update if this was empty
	  if(inc_amt > 0) {
            xferDes_queue->update_pre_bytes_write(out_port->peer_guid,
						  out_port->peer_port_idx,
						  offset,
						  inc_amt);
	  } else {
	    // TODO: mode to send non-contiguous updates?
	  }
	}
      }

#if 0
      inline void XferDes::simple_update_bytes_write(int64_t offset, uint64_t size)
      {
        log_request.info(
            "update_write: guid(%llx) off(%zd) size(%zu) pre(%llx) next(%llx)",
            guid, (ssize_t)offset, (size_t)size, pre_xd_guid, next_xd_guid);

	
        if (next_xd_guid != XFERDES_NO_GUID) {
          bool update = false;
          if ((int64_t)(bytes_write % dst_buf.buf_size) == offset) {
            bytes_write += size;
            update = true;
          } else {
            segments_write[offset] = size;
          }
          std::map<int64_t, uint64_t>::iterator it;
          while (true) {
            it = segments_write.find(bytes_write % dst_buf.buf_size);
            if (it == segments_write.end())
              break;
            bytes_write += it->second;
            update = true;
            segments_write.erase(it);
          }
          if (update) {
            xferDes_queue->update_pre_bytes_write(next_xd_guid, bytes_write);
          }
        }
        else {
          bytes_write += size;
        }
        //printf("[%d] offset(%ld), size(%lu), bytes_writes(%lx): %ld\n", Network::my_node_id, offset, size, guid, bytes_write);
      }
#endif

      void XferDes::update_pre_bytes_write(int port_idx, size_t offset, size_t size)
      {
	XferPort *in_port = &input_ports[port_idx];

	size_t inc_amt = in_port->seq_remote.add_span(offset, size);
	log_xd.info() << "pre_write: " << std::hex << guid << std::dec
		      << "(" << port_idx << ") " << offset << "+" << size << " -> " << inc_amt << " (" << in_port->remote_bytes_total.load() << ")";
	// if we got new data at the current pointer OR if we now know the
	//  total incoming bytes, update progress
	if(inc_amt > 0) update_progress();
      }

      void XferDes::update_pre_bytes_total(int port_idx, size_t pre_bytes_total)
      {
	XferPort *in_port = &input_ports[port_idx];

	// should always be exchanging -1 -> (not -1)
#ifdef DEBUG_REALM
	size_t oldval =
#endif
	  in_port->remote_bytes_total.exchange(pre_bytes_total);
#ifdef DEBUG_REALM
	assert((oldval == size_t(-1)) && (pre_bytes_total != size_t(-1)));
#endif
	log_xd.info() << "pre_total: " << std::hex << guid << std::dec
		      << "(" << port_idx << ") = " << pre_bytes_total;
	// this may unblock an xd that has consumed all input but didn't
	//  realize there was no more
	update_progress();
      }

      void XferDes::update_next_bytes_read(int port_idx, size_t offset, size_t size)
      {
	XferPort *out_port = &output_ports[port_idx];

	size_t inc_amt = out_port->seq_remote.add_span(offset, size);
	log_xd.info() << "next_read: "  << std::hex << guid << std::dec
		      << "(" << port_idx << ") " << offset << "+" << size << " -> " << inc_amt;
	// if we got new room at the current pointer, update progress
	if(inc_amt > 0) update_progress();
      }

      void XferDes::default_notify_request_read_done(Request* req)
      {  
        req->is_read_done = true;
	update_bytes_read(req->src_port_idx,
			  req->read_seq_pos, req->read_seq_count);
#if 0
        if (req->dim == Request::DIM_1D)
          simple_update_bytes_read(req->src_off, req->nbytes);
        else
          simple_update_bytes_read(req->src_off, req->nbytes * req->nlines);
#endif
      }

      void XferDes::default_notify_request_write_done(Request* req)
      {
        req->is_write_done = true;
	// calling update_bytes_write can cause the transfer descriptor to
	//  be destroyed, so enqueue the request first, and cache the values
	//  we need
	int dst_port_idx = req->dst_port_idx;
	size_t write_seq_pos = req->write_seq_pos;
	size_t write_seq_count = req->write_seq_count;
	update_bytes_write(dst_port_idx, write_seq_pos, write_seq_count);
#if 0
        if (req->dim == Request::DIM_1D)
          simple_update_bytes_write(req->dst_off, req->nbytes);
        else
          simple_update_bytes_write(req->dst_off, req->nbytes * req->nlines);
#endif
        enqueue_request(req);
      }

      MemcpyXferDes::MemcpyXferDes(DmaRequest *_dma_request, NodeID _launch_node, XferDesID _guid,
				   const std::vector<XferDesPortInfo>& inputs_info,
				   const std::vector<XferDesPortInfo>& outputs_info,
				   bool _mark_start,
				   uint64_t _max_req_size, long max_nr, int _priority,
				   XferDesFence* _complete_fence)
	: XferDes(_dma_request, _launch_node, _guid,
		  inputs_info, outputs_info,
		  _mark_start,
		  _max_req_size, _priority,
		  _complete_fence)
	, memcpy_req_in_use(false)
      {
        channel = channel_manager->get_memcpy_channel();
	kind = XFER_MEM_CPY;

	// scan input and output ports to see if any use serdez ops
	has_serdez = false;
	for(size_t i = 0; i < inputs_info.size(); i++)
	  if(inputs_info[i].serdez_id != 0)
	    has_serdez = true;
	for(size_t i = 0; i < outputs_info.size(); i++)
	  if(outputs_info[i].serdez_id != 0)
	    has_serdez = true;

	// ignore requested max_nr and always use 1
	memcpy_req.xd = this;
      }

      long MemcpyXferDes::get_requests(Request** requests, long nr)
      {
        MemcpyRequest** reqs = (MemcpyRequest**) requests;
	// allow 2D and 3D copies
	unsigned flags = (TransferIterator::LINES_OK |
			  TransferIterator::PLANES_OK);
        long new_nr = default_get_requests(requests, nr, flags);
        for (long i = 0; i < new_nr; i++)
        {
	  bool src_is_serdez = (input_ports[reqs[i]->src_port_idx].serdez_op != 0);
	  bool dst_is_serdez = (output_ports[reqs[i]->dst_port_idx].serdez_op != 0);
          if(!src_is_serdez && dst_is_serdez) {
            // source offset is determined later - not safe to call get_direct_ptr now
            reqs[i]->src_base = 0;
          } else {
	    reqs[i]->src_base = input_ports[reqs[i]->src_port_idx].mem->get_direct_ptr(reqs[i]->src_off,
										       reqs[i]->nbytes);
	    assert(reqs[i]->src_base != 0);
          }
          if(src_is_serdez && !dst_is_serdez) {
            // dest offset is determined later - not safe to call get_direct_ptr now
            reqs[i]->dst_base = 0;
          } else {
	    reqs[i]->dst_base = output_ports[reqs[i]->dst_port_idx].mem->get_direct_ptr(reqs[i]->dst_off,
											reqs[i]->nbytes);
	    assert(reqs[i]->dst_base != 0);
          }
        }
        return new_nr;

#ifdef TO_BE_DELETE
        long idx = 0;
        while (idx < nr && !available_reqs.empty() && offset_idx < oas_vec.size()) {
          off_t src_start, dst_start;
          size_t nbytes;
          if (DIM == 0) {
            simple_get_mask_request(src_start, dst_start, nbytes, me, offset_idx, min(available_reqs.size(), nr - idx));
          } else {
            simple_get_request<DIM>(src_start, dst_start, nbytes, li, offset_idx, min(available_reqs.size(), nr - idx));
          }
          if (nbytes == 0)
            break;
          //printf("[MemcpyXferDes] guid = %lx, offset_idx = %lld, oas_vec.size() = %lu, nbytes = %lu\n", guid, offset_idx, oas_vec.size(), nbytes);
          while (nbytes > 0) {
            size_t req_size = nbytes;
            if (src_buf.is_ib) {
              src_start = src_start % src_buf.buf_size;
              req_size = std::min(req_size, (size_t)(src_buf.buf_size - src_start));
            }
            if (dst_buf.is_ib) {
              dst_start = dst_start % dst_buf.buf_size;
              req_size = std::min(req_size, (size_t)(dst_buf.buf_size - dst_start));
            }
            mem_cpy_reqs[idx] = (MemcpyRequest*) available_reqs.front();
            available_reqs.pop();
            //printf("[MemcpyXferDes] src_start = %ld, dst_start = %ld, nbytes = %lu\n", src_start, dst_start, nbytes);
            mem_cpy_reqs[idx]->is_read_done = false;
            mem_cpy_reqs[idx]->is_write_done = false;
            mem_cpy_reqs[idx]->src_buf = (char*)(src_buf_base + src_start);
            mem_cpy_reqs[idx]->dst_buf = (char*)(dst_buf_base + dst_start);
            mem_cpy_reqs[idx]->nbytes = req_size;
            src_start += req_size; // here we don't have to mod src_buf.buf_size since it will be performed in next loop
            dst_start += req_size; //
            nbytes -= req_size;
            idx++;
          }
        }
        return idx;
#endif
      }

      void MemcpyXferDes::notify_request_read_done(Request* req)
      {
        default_notify_request_read_done(req);
      }

      void MemcpyXferDes::notify_request_write_done(Request* req)
      {
        default_notify_request_write_done(req);
      }

      void MemcpyXferDes::flush()
      {
      }

      bool MemcpyXferDes::request_available()
      {
	return !memcpy_req_in_use;
      }

      Request* MemcpyXferDes::dequeue_request()
      {
	assert(!memcpy_req_in_use);
	memcpy_req_in_use = true;
	memcpy_req.is_read_done = false;
	memcpy_req.is_write_done = false;
	// memcpy request is handled in-thread, so no need to mess with refcount
        return &memcpy_req;
      }

      void MemcpyXferDes::enqueue_request(Request* req)
      {
	assert(memcpy_req_in_use);
	assert(req == &memcpy_req);
	memcpy_req_in_use = false;
      }

      bool MemcpyXferDes::progress_xd(MemcpyChannel *channel,
				      TimeLimit work_until)
      {
	if(has_serdez) {
	  Request *rq;
	  bool did_work = false;
	  do {
	    long count = get_requests(&rq, 1);
	    if(count > 0) {
	      channel->submit(&rq, count);
	      did_work = true;
	    } else
	      break;
	  } while(!work_until.is_expired());

	  return did_work;
	}

	// fast path - assumes no serdez
	bool did_work = false;
	ReadSequenceCache rseqcache(this, 2 << 20);  // flush after 2MB
	WriteSequenceCache wseqcache(this, 2 << 20);

	while(true) {
	  size_t min_xfer_size = 4096;  // TODO: make controllable
	  size_t max_bytes = get_addresses(min_xfer_size, &rseqcache);
	  if(max_bytes == 0)
	    break;

	  XferPort *in_port = 0, *out_port = 0;
	  size_t in_span_start = 0, out_span_start = 0;
	  if(input_control.current_io_port >= 0) {
	    in_port = &input_ports[input_control.current_io_port];
	    in_span_start = in_port->local_bytes_total;
	  }
	  if(output_control.current_io_port >= 0) {
	    out_port = &output_ports[output_control.current_io_port];
	    out_span_start = out_port->local_bytes_total;
	  }

	  size_t total_bytes = 0;
	  if(in_port != 0) {
	    if(out_port != 0) {
	      // input and output both exist - transfer what we can
	      log_xd.info() << "memcpy chunk: min=" << min_xfer_size
			    << " max=" << max_bytes;

	      uintptr_t in_base = reinterpret_cast<uintptr_t>(in_port->mem->get_direct_ptr(0, 0));
	      uintptr_t out_base = reinterpret_cast<uintptr_t>(out_port->mem->get_direct_ptr(0, 0));

	      while(total_bytes < max_bytes) {
		AddressListCursor& in_alc = in_port->addrcursor;
		AddressListCursor& out_alc = out_port->addrcursor;

		uintptr_t in_offset = in_alc.get_offset();
		uintptr_t out_offset = out_alc.get_offset();

		// the reported dim is reduced for partially consumed address
		//  ranges - whatever we get can be assumed to be regular
		int in_dim = in_alc.get_dim();
		int out_dim = out_alc.get_dim();

		size_t bytes = 0;
		size_t bytes_left = max_bytes - total_bytes;
		// memcpys don't need to be particularly big to achieve
		//  peak efficiency, so trim to something that takes
		//  10's of us to be responsive to the time limit
		bytes_left = std::min(bytes_left, size_t(256 << 10));

		if(in_dim > 0) {
		  if(out_dim > 0) {
		    size_t icount = in_alc.remaining(0);
		    size_t ocount = out_alc.remaining(0);

		    // contig bytes is always the min of the first dimensions
		    size_t contig_bytes = std::min(std::min(icount, ocount),
						   bytes_left);

		    // catch simple 1D case first
		    if((contig_bytes == bytes_left) ||
		       ((contig_bytes == icount) && (in_dim == 1)) ||
		       ((contig_bytes == ocount) && (out_dim == 1))) {
		      bytes = contig_bytes;
		      memcpy_1d(out_base + out_offset,
				in_base + in_offset,
				bytes);
		      in_alc.advance(0, bytes);
		      out_alc.advance(0, bytes);
		    } else {
		      // grow to a 2D copy
		      int id;
		      int iscale;
		      uintptr_t in_lstride;
		      if(contig_bytes < icount) {
			// second input dim comes from splitting first
			id = 0;
			in_lstride = contig_bytes;
			size_t ilines = icount / contig_bytes;
			if((ilines * contig_bytes) != icount)
			  in_dim = 1;  // leftover means we can't go beyond this
			icount = ilines;
			iscale = contig_bytes;
		      } else {
			assert(in_dim > 1);
			id = 1;
			icount = in_alc.remaining(id);
			in_lstride = in_alc.get_stride(id);
			iscale = 1;
		      }

		      int od;
		      int oscale;
		      uintptr_t out_lstride;
		      if(contig_bytes < ocount) {
			// second output dim comes from splitting first
			od = 0;
			out_lstride = contig_bytes;
			size_t olines = ocount / contig_bytes;
			if((olines * contig_bytes) != ocount)
			  out_dim = 1;  // leftover means we can't go beyond this
			ocount = olines;
			oscale = contig_bytes;
		      } else {
			assert(out_dim > 1);
			od = 1;
			ocount = out_alc.remaining(od);
			out_lstride = out_alc.get_stride(od);
			oscale = 1;
		      }

		      size_t lines = std::min(std::min(icount, ocount),
					      bytes_left / contig_bytes);

		      // see if we need to stop at 2D
		      if(((contig_bytes * lines) == bytes_left) ||
			 ((lines == icount) && (id == (in_dim - 1))) ||
			 ((lines == ocount) && (od == (out_dim - 1)))) {
			bytes = contig_bytes * lines;
			memcpy_2d(out_base + out_offset, out_lstride,
				  in_base + in_offset, in_lstride,
				  contig_bytes, lines);
			in_alc.advance(id, lines * iscale);
			out_alc.advance(od, lines * oscale);
		      } else {
			uintptr_t in_pstride;
			if(lines < icount) {
			  // third input dim comes from splitting current
			  in_pstride = in_lstride * lines;
			  size_t iplanes = icount / lines;
			  // check for leftovers here if we go beyond 3D!
			  icount = iplanes;
			  iscale *= lines;
			} else {
			  id++;
			  assert(in_dim > id);
			  icount = in_alc.remaining(id);
			  in_pstride = in_alc.get_stride(id);
			  iscale = 1;
			}

			uintptr_t out_pstride;
			if(lines < ocount) {
			  // third output dim comes from splitting current
			  out_pstride = out_lstride * lines;
			  size_t oplanes = ocount / lines;
			  // check for leftovers here if we go beyond 3D!
			  ocount = oplanes;
			  oscale *= lines;
			} else {
			  od++;
			  assert(out_dim > od);
			  ocount = out_alc.remaining(od);
			  out_pstride = out_alc.get_stride(od);
			  oscale = 1;
			}

			size_t planes = std::min(std::min(icount, ocount),
						 (bytes_left /
						  (contig_bytes * lines)));

			bytes = contig_bytes * lines * planes;
			memcpy_3d(out_base + out_offset, out_lstride, out_pstride,
				  in_base + in_offset, in_lstride, in_pstride,
				  contig_bytes, lines, planes);
			in_alc.advance(id, planes * iscale);
			out_alc.advance(od, planes * oscale);
		      }
		    }
		  } else {
		    // scatter adddress list
		    assert(0);
		  }
		} else {
		  if(out_dim > 0) {
		    // gather address list
		    assert(0);
		  } else {
		    // gather and scatter
		    assert(0);
		  }
		}

#ifdef DEBUG_REALM
		assert(bytes <= bytes_left);
#endif
		total_bytes += bytes;

		// stop if it's been too long, but make sure we do at least the
		//  minimum number of bytes
		if((total_bytes >= min_xfer_size) && work_until.is_expired()) break;
	      }
	    } else {
	      // input but no output, so skip input bytes
	      total_bytes = max_bytes;
	      in_port->addrcursor.skip_bytes(total_bytes);
	    }
	  } else {
	    if(out_port != 0) {
	      // output but no input, so skip output bytes
	      total_bytes = max_bytes;
	      out_port->addrcursor.skip_bytes(total_bytes);
	    } else {
	      // skipping both input and output is possible for simultaneous
	      //  gather+scatter
	      total_bytes = max_bytes;
	    }
	  }

	  // memcpy is always immediate, so handle both skip and copy with the
	  //  same code
	  rseqcache.add_span(input_control.current_io_port,
			     in_span_start, total_bytes);
	  in_span_start += total_bytes;
	  wseqcache.add_span(output_control.current_io_port,
			     out_span_start, total_bytes);
	  out_span_start += total_bytes;

	  bool done = record_address_consumption(total_bytes);

	  did_work = true;

	  if(done || work_until.is_expired())
	    break;
	}

	rseqcache.flush();
	wseqcache.flush();

	return did_work;
      }


      GASNetXferDes::GASNetXferDes(DmaRequest *_dma_request, NodeID _launch_node, XferDesID _guid,
				   const std::vector<XferDesPortInfo>& inputs_info,
				   const std::vector<XferDesPortInfo>& outputs_info,
				   bool _mark_start,
				   uint64_t _max_req_size, long max_nr, int _priority,
				   XferDesFence* _complete_fence)
	: XferDes(_dma_request, _launch_node, _guid,
		  inputs_info, outputs_info,
		  _mark_start,
		  _max_req_size, _priority,
		  _complete_fence)
      {
	if((inputs_info.size() >= 1) &&
	   (input_ports[0].mem->kind == MemoryImpl::MKIND_GLOBAL)) {
	  kind = XFER_GASNET_READ;
	  channel = get_channel_manager()->get_gasnet_read_channel();
	} else if((outputs_info.size() >= 1) &&
		  (output_ports[0].mem->kind == MemoryImpl::MKIND_GLOBAL)) {
	  kind = XFER_GASNET_WRITE;
	  channel = get_channel_manager()->get_gasnet_write_channel();
	} else {
	  assert(0 && "neither source nor dest of GASNetXferDes is gasnet!?");
	}
        gasnet_reqs = (GASNetRequest*) calloc(max_nr, sizeof(GASNetRequest));
        for (int i = 0; i < max_nr; i++) {
          gasnet_reqs[i].xd = this;
	  available_reqs.push(&gasnet_reqs[i]);
        }
      }

      long GASNetXferDes::get_requests(Request** requests, long nr)
      {
        GASNetRequest** reqs = (GASNetRequest**) requests;
        long new_nr = default_get_requests(requests, nr);
        switch (kind) {
          case XFER_GASNET_READ:
          {
            for (long i = 0; i < new_nr; i++) {
              reqs[i]->gas_off = /*src_buf.alloc_offset +*/ reqs[i]->src_off;
              //reqs[i]->mem_base = (char*)(buf_base + reqs[i]->dst_off);
	      reqs[i]->mem_base = output_ports[reqs[i]->dst_port_idx].mem->get_direct_ptr(reqs[i]->dst_off,
											  reqs[i]->nbytes);
	      assert(reqs[i]->mem_base != 0);
            }
            break;
          }
          case XFER_GASNET_WRITE:
          {
            for (long i = 0; i < new_nr; i++) {
              //reqs[i]->mem_base = (char*)(buf_base + reqs[i]->src_off);
	      reqs[i]->mem_base = input_ports[reqs[i]->src_port_idx].mem->get_direct_ptr(reqs[i]->src_off,
											 reqs[i]->nbytes);
	      assert(reqs[i]->mem_base != 0);
              reqs[i]->gas_off = /*dst_buf.alloc_offset +*/ reqs[i]->dst_off;
            }
            break;
          }
          default:
            assert(0);
        }
        return new_nr;
      }

      bool GASNetXferDes::progress_xd(GASNetChannel *channel,
				      TimeLimit work_until)
      {
	Request *rq;
	bool did_work = false;
	do {
	  long count = get_requests(&rq, 1);
	  if(count > 0) {
	    channel->submit(&rq, count);
	    did_work = true;
	  } else
	    break;
	} while(!work_until.is_expired());

	return did_work;
      }

      void GASNetXferDes::notify_request_read_done(Request* req)
      {
        default_notify_request_read_done(req);
      }

      void GASNetXferDes::notify_request_write_done(Request* req)
      {
        default_notify_request_write_done(req);
      }

      void GASNetXferDes::flush()
      {
      }

      RemoteWriteXferDes::RemoteWriteXferDes(DmaRequest *_dma_request, NodeID _launch_node, XferDesID _guid,
					     const std::vector<XferDesPortInfo>& inputs_info,
					     const std::vector<XferDesPortInfo>& outputs_info,
					     bool _mark_start,
					     uint64_t _max_req_size, long max_nr, int _priority,
					     XferDesFence* _complete_fence)
      : XferDes(_dma_request, _launch_node, _guid,
		inputs_info, outputs_info,
		_mark_start,
		_max_req_size, _priority,
                _complete_fence)
      {
        channel = channel_manager->get_remote_write_channel();
	kind = XFER_REMOTE_WRITE;
        requests = (RemoteWriteRequest*) calloc(max_nr, sizeof(RemoteWriteRequest));
        for (int i = 0; i < max_nr; i++) {
          requests[i].xd = this;
          //requests[i].dst_node = dst_node;
          available_reqs.push(&requests[i]);
        }
      }

      long RemoteWriteXferDes::get_requests(Request** requests, long nr)
      {
	xd_lock.lock();
        RemoteWriteRequest** reqs = (RemoteWriteRequest**) requests;
	// remote writes allow 2D on source, but not destination
	unsigned flags = TransferIterator::SRC_LINES_OK;
        long new_nr = default_get_requests(requests, nr, flags);
        for (long i = 0; i < new_nr; i++)
        {
          //reqs[i]->src_base = (char*)(src_buf_base + reqs[i]->src_off);
	  reqs[i]->src_base = input_ports[reqs[i]->src_port_idx].mem->get_direct_ptr(reqs[i]->src_off,
										     reqs[i]->nbytes);
	  assert(reqs[i]->src_base != 0);
	  //RemoteMemory *remote = checked_cast<RemoteMemory *>(output_ports[reqs[i]->dst_port_idx].mem);
	  //reqs[i]->dst_base = static_cast<char *>(remote->get_remote_addr(reqs[i]->dst_off));
	  //assert(reqs[i]->dst_base != 0);
        }
	xd_lock.unlock();
        return new_nr;
      }

      // callbacks for updating read/write spans
      class ReadBytesUpdater {
      public:
	ReadBytesUpdater(XferDes *_xd, int _port_idx,
			 size_t _offset, size_t _size)
	  : xd(_xd), port_idx(_port_idx), offset(_offset), size(_size)
	{}

	void operator()() const
	{
	  xd->update_bytes_read(port_idx, offset, size);
	  xd->remove_reference();
	}

      protected:
	XferDes *xd;
	int port_idx;
	size_t offset, size;
      };

      class WriteBytesUpdater {
      public:
	WriteBytesUpdater(XferDes *_xd, int _port_idx,
			  size_t _offset, size_t _size)
	  : xd(_xd), port_idx(_port_idx), offset(_offset), size(_size)
	{}

	void operator()() const
	{
	  xd->update_bytes_write(port_idx, offset, size);
	}

      protected:
	XferDes *xd;
	int port_idx;
	size_t offset, size;
      };

      bool RemoteWriteXferDes::progress_xd(RemoteWriteChannel *channel,
					   TimeLimit work_until)
      {
#if 0
	Request *rq;
	bool did_work = false;
	do {
	  long count = get_requests(&rq, 1);
	  if(count > 0) {
	    channel->submit(&rq, count);
	    did_work = true;
	  } else
	    break;
	} while(!work_until.is_expired());

	return did_work;
#else
	bool did_work = false;
	// immediate acks for reads happen when we assemble or skip input,
	//  while immediate acks for writes happen only if we skip output
	ReadSequenceCache rseqcache(this);
	WriteSequenceCache wseqcache(this);

	const size_t MAX_ASSEMBLY_SIZE = 4096;
	while(true) {
	  size_t min_xfer_size = 4096;  // TODO: make controllable
	  size_t max_bytes = get_addresses(min_xfer_size, &rseqcache);
	  if(max_bytes == 0)
	    break;

	  XferPort *in_port = 0, *out_port = 0;
	  size_t in_span_start = 0, out_span_start = 0;
	  if(input_control.current_io_port >= 0) {
	    in_port = &input_ports[input_control.current_io_port];
	    in_span_start = in_port->local_bytes_total;
	  }
	  if(output_control.current_io_port >= 0) {
	    out_port = &output_ports[output_control.current_io_port];
	    out_span_start = out_port->local_bytes_total;
	  }

	  size_t total_bytes = 0;
	  if(in_port != 0) {
	    if(out_port != 0) {
	      // input and output both exist - transfer what we can
	      log_xd.info() << "remote write chunk: min=" << min_xfer_size
			    << " max=" << max_bytes;

	      while(total_bytes < max_bytes) {
		AddressListCursor& in_alc = in_port->addrcursor;
		AddressListCursor& out_alc = out_port->addrcursor;
		int in_dim = in_alc.get_dim();
		int out_dim = out_alc.get_dim();
		size_t icount = in_alc.remaining(0);
		size_t ocount = out_alc.remaining(0);

		size_t bytes = 0;
		size_t bytes_left = max_bytes - total_bytes;

		// look at the output first, because that controls the message
		//  size
		size_t dst_1d_maxbytes = ((out_dim > 0) ?
					    std::min(bytes_left, ocount) :
					    0);
		size_t dst_2d_maxbytes = (((out_dim > 1) &&
					   (ocount <= (MAX_ASSEMBLY_SIZE / 2))) ?
					    (ocount * std::min(MAX_ASSEMBLY_SIZE / ocount,
							       out_alc.remaining(1))) :
					    0);
		// would have to scan forward through the dst address list to
		//  get the exact number of bytes that we can fit into
		//  MAX_ASSEMBLY_SIZE after considering address info overhead,
		//  but this is a last resort anyway, so just use a probably-
		//  pessimistic estimate;
		size_t dst_sc_maxbytes = std::min(bytes_left,
						  MAX_ASSEMBLY_SIZE / 4);
		// TODO: actually implement 2d and sc
		dst_2d_maxbytes = 0;
		dst_sc_maxbytes = 0;

		// favor 1d >> 2d >> sc
		if((dst_1d_maxbytes >= dst_2d_maxbytes) &&
		   (dst_1d_maxbytes >= dst_sc_maxbytes)) {
		  // 1D target
		  NodeID dst_node = ID(out_port->mem->me).memory_owner_node();
		  RemoteAddress dst_buf;
		  bool ok = out_port->mem->get_remote_addr(out_alc.get_offset(),
							   dst_buf);
		  assert(ok);

		  // now look at the input
		  const void *src_buf = in_port->mem->get_direct_ptr(in_alc.get_offset(), icount);
		  size_t src_1d_maxbytes = 0;
		  if(in_dim > 0) {
		    size_t rec_bytes = ActiveMessage<Write1DMessage>::recommended_max_payload(dst_node,
											      src_buf, icount, 1, 0,
											      dst_buf,
											      true /*w/ congestion*/);
		    src_1d_maxbytes = std::min({ dst_1d_maxbytes,
					         icount,
					         rec_bytes });
		  }

		  size_t src_2d_maxbytes = 0;
		  if(in_dim > 1) {
		    size_t lines = in_alc.remaining(1);
		    size_t rec_bytes = ActiveMessage<Write1DMessage>::recommended_max_payload(dst_node,
											      src_buf, icount,
											      lines,
											      in_alc.get_stride(1),
											      dst_buf,
											      true /*w/ congestion*/);
		    // round the recommendation down to a multiple of the line size
		    rec_bytes -= (rec_bytes % icount);
		    src_2d_maxbytes = std::min({ dst_1d_maxbytes,
			                         icount * lines,
			                         rec_bytes });
		  }
		  size_t src_ga_maxbytes = 0;
		  {
		    // a gather will assemble into a buffer provided by the network
		    size_t rec_bytes = ActiveMessage<Write1DMessage>::recommended_max_payload(dst_node,
											      dst_buf,
											      true /*w/ congestion*/);
		    src_ga_maxbytes = std::min({ dst_1d_maxbytes,
					         bytes_left,
					         rec_bytes });
		  }

		  // source also favors 1d >> 2d >> gather
		  if((src_1d_maxbytes >= src_2d_maxbytes) &&
		     (src_1d_maxbytes >= src_ga_maxbytes)) {
		    // 1D source
		    bytes = src_1d_maxbytes;
		    //log_xd.info() << "remote write 1d: guid=" << guid
		    //              << " src=" << src_buf << " dst=" << dst_buf
		    //              << " bytes=" << bytes;
		    ActiveMessage<Write1DMessage> amsg(dst_node,
						       src_buf, bytes,
						       dst_buf);
		    amsg->next_xd_guid = out_port->peer_guid;
		    amsg->next_port_idx = out_port->peer_port_idx;
		    amsg->span_start = out_span_start;

		    // reads aren't consumed until local completion, but
		    //  only ask if we have a previous xd that's going to
		    //  care
		    if(in_port->peer_guid != XFERDES_NO_GUID) {
		      // a ReadBytesUpdater holds a reference to the xd
		      add_reference();
		      amsg.add_local_completion(ReadBytesUpdater(this,
								 input_control.current_io_port,
								 in_span_start,
								 bytes));
		    }
		    in_span_start += bytes;
		    // the write isn't complete until it's ack'd by the target
		    amsg.add_remote_completion(WriteBytesUpdater(this,
								 output_control.current_io_port,
								 out_span_start,
								 bytes));
		    out_span_start += bytes;

		    amsg.commit();
		    in_alc.advance(0, bytes);
		    out_alc.advance(0, bytes);
		  }
		  else if(src_2d_maxbytes >= src_ga_maxbytes) {
		    // 2D source
		    size_t bytes_per_line = icount;
		    size_t lines = src_2d_maxbytes / icount;
		    bytes = bytes_per_line * lines;
		    assert(bytes == src_2d_maxbytes);
		    size_t src_stride = in_alc.get_stride(1);
		    //log_xd.info() << "remote write 2d: guid=" << guid
		    //              << " src=" << src_buf << " dst=" << dst_buf
		    //              << " bytes=" << bytes << " lines=" << lines
		    //              << " stride=" << src_stride;
		    ActiveMessage<Write1DMessage> amsg(dst_node,
						       src_buf, bytes_per_line,
						       lines, src_stride,
						       dst_buf);
		    amsg->next_xd_guid = out_port->peer_guid;
		    amsg->next_port_idx = out_port->peer_port_idx;
		    amsg->span_start = out_span_start;

		    // reads aren't consumed until local completion, but
		    //  only ask if we have a previous xd that's going to
		    //  care
		    if(in_port->peer_guid != XFERDES_NO_GUID) {
		      // a ReadBytesUpdater holds a reference to the xd
		      add_reference();
		      amsg.add_local_completion(ReadBytesUpdater(this,
								 input_control.current_io_port,
								 in_span_start,
								 bytes));
		    }
		    in_span_start += bytes;
		    // the write isn't complete until it's ack'd by the target
		    amsg.add_remote_completion(WriteBytesUpdater(this,
								 output_control.current_io_port,
								 out_span_start,
								 bytes));
		    out_span_start += bytes;

		    amsg.commit();
		    in_alc.advance(1, lines);
		    out_alc.advance(0, bytes);
		  } else {
		    // gather: assemble data
		    bytes = src_ga_maxbytes;
		    ActiveMessage<Write1DMessage> amsg(dst_node,
						       bytes,
						       dst_buf);
		    amsg->next_xd_guid = out_port->peer_guid;
		    amsg->next_port_idx = out_port->peer_port_idx;
		    amsg->span_start = out_span_start;

		    size_t todo = bytes;
		    while(true) {
		      if(in_dim > 0) {
			if((icount >= todo/2) || (in_dim == 1)) {
			  size_t chunk = std::min(todo, icount);
			  uintptr_t src = reinterpret_cast<uintptr_t>(in_port->mem->get_direct_ptr(in_alc.get_offset(), chunk));
			  uintptr_t dst = reinterpret_cast<uintptr_t>(amsg.payload_ptr(chunk));
			  memcpy_1d(dst, src, chunk);
			  in_alc.advance(0, chunk);
			  todo -= chunk;
			} else {
			  size_t lines = std::min(todo / icount,
						  in_alc.remaining(1));

			  if(((icount * lines) >= todo/2) || (in_dim == 2)) {
			    uintptr_t src = reinterpret_cast<uintptr_t>(in_port->mem->get_direct_ptr(in_alc.get_offset(), icount));
			    uintptr_t dst = reinterpret_cast<uintptr_t>(amsg.payload_ptr(icount * lines));
			    memcpy_2d(dst, icount /*lstride*/,
				      src, in_alc.get_stride(1),
				      icount, lines);
			    in_alc.advance(1, lines);
			    todo -= icount * lines;
			  } else {
			    size_t planes = std::min(todo / (icount * lines),
						     in_alc.remaining(2));
			    uintptr_t src = reinterpret_cast<uintptr_t>(in_port->mem->get_direct_ptr(in_alc.get_offset(), icount));
			    uintptr_t dst = reinterpret_cast<uintptr_t>(amsg.payload_ptr(icount * lines * planes));
			    memcpy_3d(dst,
				      icount /*lstride*/,
				      (icount * lines) /*pstride*/,
				      src,
				      in_alc.get_stride(1),
				      in_alc.get_stride(2),
				      icount, lines, planes);
			    in_alc.advance(2, planes);
			    todo -= icount * lines * planes;
			  }
			}
		      } else {
			assert(0);
		      }

		      if(todo == 0) break;

		      // read next entry
		      in_dim = in_alc.get_dim();
		      icount = in_alc.remaining(0);
		    }

		    // the write isn't complete until it's ack'd by the target
		    amsg.add_remote_completion(WriteBytesUpdater(this,
								 output_control.current_io_port,
								 out_span_start,
								 bytes));
		    out_span_start += bytes;

		    // assembly complete - send message
		    amsg.commit();

		    // we made a copy of input data, so "read" is complete
		    rseqcache.add_span(input_control.current_io_port,
				       in_span_start, bytes);
		    in_span_start += bytes;

		    out_alc.advance(0, bytes);
		  }
		}
		else if(dst_2d_maxbytes >= dst_sc_maxbytes) {
		  // 2D target
		  assert(0);
		} else {
		  // scatter target
		  assert(0);
		}

#ifdef DEBUG_REALM
		assert((bytes > 0) && (bytes <= bytes_left));
#endif
		total_bytes += bytes;

		// stop if it's been too long, but make sure we do at least the
		//  minimum number of bytes
		if((total_bytes >= min_xfer_size) && work_until.is_expired()) break;
	      }
	    } else {
	      // input but no output, so skip input bytes
	      total_bytes = max_bytes;
	      in_port->addrcursor.skip_bytes(total_bytes);
	      rseqcache.add_span(input_control.current_io_port,
				 in_span_start, total_bytes);
	      in_span_start += total_bytes;
	    }
	  } else {
	    if(out_port != 0) {
	      // output but no input, so skip output bytes
	      total_bytes = max_bytes;
	      out_port->addrcursor.skip_bytes(total_bytes);
	      wseqcache.add_span(output_control.current_io_port,
				 out_span_start, total_bytes);
	      out_span_start += total_bytes;
	    } else {
	      // skipping both input and output is possible for simultaneous
	      //  gather+scatter
	      total_bytes = max_bytes;
	    }
	  }

	  bool done = record_address_consumption(total_bytes);

	  did_work = true;

	  if(done || work_until.is_expired())
	    break;
	}

	rseqcache.flush();
	wseqcache.flush();

	return did_work;
#endif
      }

      void RemoteWriteXferDes::notify_request_read_done(Request* req)
      {
        xd_lock.lock();
        default_notify_request_read_done(req);
        xd_lock.unlock();
      }

      void RemoteWriteXferDes::notify_request_write_done(Request* req)
      {
        xd_lock.lock();
        default_notify_request_write_done(req);
        xd_lock.unlock();
      }

      void RemoteWriteXferDes::flush()
      {
        //xd_lock.lock();
        //xd_lock.unlock();
      }

      // doesn't do pre_bytes_write updates, since the remote write message
      //  takes care of it with lower latency (except for zero-byte
      //  termination updates)
      void RemoteWriteXferDes::update_bytes_write(int port_idx, size_t offset, size_t size)
      {
	XferPort *out_port = &output_ports[port_idx];
	size_t inc_amt = out_port->seq_local.add_span(offset, size);
	log_xd.info() << "bytes_write: " << std::hex << guid << std::dec
		      << "(" << port_idx << ") " << offset << "+" << size << " -> " << inc_amt;
	// if our oldest write was ack'd, update progress in case the xd
	//  is just waiting for all writes to complete
	if(inc_amt > 0) update_progress();
	// pre_bytes_write update was handled in the remote AM handler
      }

      /*static*/
      void RemoteWriteXferDes::Write1DMessage::handle_message(NodeID sender,
							      const RemoteWriteXferDes::Write1DMessage &args,
							      const void *data,
							      size_t datalen)
      {
        // assert data copy is in right position
        //assert(data == args.dst_buf);

	log_xd.info() << "remote write recieved: next=" << args.next_xd_guid
		      << " start=" << args.span_start
		      << " size=" << datalen;

	// if requested, notify (probably-local) next XD
	if(args.next_xd_guid != XferDes::XFERDES_NO_GUID)
	  xferDes_queue->update_pre_bytes_write(args.next_xd_guid,
						args.next_port_idx,
						args.span_start,
						datalen);
      }

      /*static*/ bool RemoteWriteXferDes::Write1DMessage::handle_inline(NodeID sender,
									const RemoteWriteXferDes::Write1DMessage &args,
									const void *data,
									size_t datalen,
									TimeLimit work_until)
      {
	handle_message(sender, args, data, datalen);
	return true;
      }

#ifdef REALM_USE_CUDA
      GPUXferDes::GPUXferDes(DmaRequest *_dma_request, NodeID _launch_node, XferDesID _guid,
				   const std::vector<XferDesPortInfo>& inputs_info,
				   const std::vector<XferDesPortInfo>& outputs_info,
				   bool _mark_start,
				   uint64_t _max_req_size, long max_nr, int _priority,
				   XferDesFence* _complete_fence)
	: XferDes(_dma_request, _launch_node, _guid,
		  inputs_info, outputs_info,
		  _mark_start,
		  _max_req_size, _priority,
		  _complete_fence)
      {
	if((inputs_info.size() >= 1) &&
	   (input_ports[0].mem->kind == MemoryImpl::MKIND_GPUFB)) {
	  // all input ports should agree on which gpu they target
	  src_gpu = ((Cuda::GPUFBMemory*)(input_ports[0].mem))->gpu;
	  for(size_t i = 1; i < input_ports.size(); i++) {
	    // exception: control and indirect ports should be readable from cpu
	    if((int(i) == input_control.control_port_idx) ||
	       (int(i) == output_control.control_port_idx) ||
	       input_ports[i].is_indirect_port) {
	      assert((input_ports[i].mem->kind == MemoryImpl::MKIND_SYSMEM) ||
		     (input_ports[i].mem->kind == MemoryImpl::MKIND_ZEROCOPY));
	      continue;
	    }
	    assert(input_ports[i].mem == input_ports[0].mem);
	  }
	} else
	  src_gpu = 0;

	if((outputs_info.size() >= 1) &&
	   (output_ports[0].mem->kind == MemoryImpl::MKIND_GPUFB)) {
	  // all output ports should agree on which gpu they target
	  dst_gpu = ((Cuda::GPUFBMemory*)(output_ports[0].mem))->gpu;
	  for(size_t i = 1; i < output_ports.size(); i++)
	    assert(output_ports[i].mem == output_ports[0].mem);
	} else
	  dst_gpu = 0;

	// if we're doing a multi-hop copy, we'll dial down the request
	//  sizes to improve pipelining
	bool multihop_copy = false;
	for(size_t i = 1; i < input_ports.size(); i++)
	  if(input_ports[i].peer_guid != XFERDES_NO_GUID)
	    multihop_copy = true;
	for(size_t i = 1; i < output_ports.size(); i++)
	  if(output_ports[i].peer_guid != XFERDES_NO_GUID)
	    multihop_copy = true;

	if(src_gpu != 0) {
	  if(dst_gpu != 0) {
	    if(src_gpu == dst_gpu) {
	      kind = XFER_GPU_IN_FB;
	      channel = channel_manager->get_gpu_in_fb_channel(src_gpu);
	      // ignore max_req_size value passed in - it's probably too small
	      max_req_size = 1 << 30;
	    } else {
	      kind = XFER_GPU_PEER_FB;
	      channel = channel_manager->get_gpu_peer_fb_channel(src_gpu);
	      // ignore max_req_size value passed in - it's probably too small
	      max_req_size = 256 << 20;
	    }
	  } else {
	    kind = XFER_GPU_FROM_FB;
	    channel = channel_manager->get_gpu_from_fb_channel(src_gpu);
	    if(multihop_copy)
	      max_req_size = 4 << 20;
	  }
	} else {
	  if(dst_gpu != 0) {
	    kind = XFER_GPU_TO_FB;
	    channel = channel_manager->get_gpu_to_fb_channel(dst_gpu);
	    if(multihop_copy)
	      max_req_size = 4 << 20;
	  } else {
	    assert(0);
	  }
	}

        for (int i = 0; i < max_nr; i++) {
          GPURequest* gpu_req = new GPURequest;
          gpu_req->xd = this;
	  gpu_req->event.req = gpu_req;
          available_reqs.push(gpu_req);
        }
      }
	
      long GPUXferDes::get_requests(Request** requests, long nr)
      {
        GPURequest** reqs = (GPURequest**) requests;
	// TODO: add support for 3D CUDA copies (just 1D and 2D for now)
	unsigned flags = (TransferIterator::LINES_OK |
			  TransferIterator::PLANES_OK);
        long new_nr = default_get_requests(requests, nr, flags);
        for (long i = 0; i < new_nr; i++) {
          switch (kind) {
            case XFER_GPU_TO_FB:
            {
              //reqs[i]->src_base = src_buf_base + reqs[i]->src_off;
	      reqs[i]->src_base = input_ports[reqs[i]->src_port_idx].mem->get_direct_ptr(reqs[i]->src_off,
											 reqs[i]->nbytes);
	      assert(reqs[i]->src_base != 0);
              //reqs[i]->dst_gpu_off = /*dst_buf.alloc_offset +*/ reqs[i]->dst_off;
              break;
            }
            case XFER_GPU_FROM_FB:
            {
              //reqs[i]->src_gpu_off = /*src_buf.alloc_offset +*/ reqs[i]->src_off;
              //reqs[i]->dst_base = dst_buf_base + reqs[i]->dst_off;
	      reqs[i]->dst_base = output_ports[reqs[i]->dst_port_idx].mem->get_direct_ptr(reqs[i]->dst_off,
											  reqs[i]->nbytes);
	      assert(reqs[i]->dst_base != 0);
              break;
            }
            case XFER_GPU_IN_FB:
            {
              //reqs[i]->src_gpu_off = /*src_buf.alloc_offset*/ + reqs[i]->src_off;
              //reqs[i]->dst_gpu_off = /*dst_buf.alloc_offset*/ + reqs[i]->dst_off;
              break;
            }
            case XFER_GPU_PEER_FB:
            {
              //reqs[i]->src_gpu_off = /*src_buf.alloc_offset +*/ reqs[i]->src_off;
              //reqs[i]->dst_gpu_off = /*dst_buf.alloc_offset +*/ reqs[i]->dst_off;
              // also need to set dst_gpu for peer xfer
              reqs[i]->dst_gpu = dst_gpu;
              break;
            }
            default:
              assert(0);
          }
        }
        return new_nr;
      }

      bool GPUXferDes::progress_xd(GPUChannel *channel,
				   TimeLimit work_until)
      {
	Request *rq;
	bool did_work = false;
	do {
	  long count = get_requests(&rq, 1);
	  if(count > 0) {
	    channel->submit(&rq, count);
	    did_work = true;
	  } else
	    break;
	} while(!work_until.is_expired());

	return did_work;
      }

      void GPUXferDes::notify_request_read_done(Request* req)
      {
        default_notify_request_read_done(req);
      }

      void GPUXferDes::notify_request_write_done(Request* req)
      {
        default_notify_request_write_done(req);
      }

      void GPUXferDes::flush()
      {
      }
#endif

#ifdef REALM_USE_HDF5
      HDF5XferDes::HDF5XferDes(DmaRequest *_dma_request, NodeID _launch_node, XferDesID _guid,
			       const std::vector<XferDesPortInfo>& inputs_info,
			       const std::vector<XferDesPortInfo>& outputs_info,
			       bool _mark_start,
			       uint64_t _max_req_size, long max_nr, int _priority,
			       XferDesFence* _complete_fence)
	: XferDes(_dma_request, _launch_node, _guid,
		  inputs_info, outputs_info,
		  _mark_start,
		  _max_req_size, _priority,
		  _complete_fence)
	, req_in_use(false)
      {
	channel = get_channel_manager()->get_hdf5_channel();
	if((inputs_info.size() >= 1) &&
	   (input_ports[0].mem->kind == MemoryImpl::MKIND_HDF)) {
	  kind = XFER_HDF5_READ;
	} else if((outputs_info.size() >= 1) &&
		  (output_ports[0].mem->kind == MemoryImpl::MKIND_HDF)) {
	  kind = XFER_HDF5_WRITE;
	} else {
	  assert(0 && "neither source nor dest of HDFXferDes is hdf5!?");
	}

	hdf5_req.xd = this;
      }

      bool HDF5XferDes::request_available()
      {
	return !req_in_use;
      }

      Request* HDF5XferDes::dequeue_request()
      {
	assert(!req_in_use);
	req_in_use = true;
	hdf5_req.is_read_done = false;
	hdf5_req.is_write_done = false;
	// HDF5Request is handled by another thread, so must hold a reference
	add_reference();
        return &hdf5_req;
      }

      void HDF5XferDes::enqueue_request(Request* req)
      {
	assert(req_in_use);
	assert(req == &hdf5_req);
	req_in_use = false;
	remove_reference();
      }

     extern Logger log_hdf5;

      long HDF5XferDes::get_requests(Request** requests, long nr)
      {
        long idx = 0;
	
	while((idx < nr) && request_available()) {	  
	  // TODO: use control stream to determine which input/output ports
	  //  to use
	  int in_port_idx = 0;
	  int out_port_idx = 0;

	  XferPort *in_port = &input_ports[in_port_idx];
	  XferPort *out_port = &output_ports[out_port_idx];

	  // is our iterator done?
	  if(in_port->iter->done() || out_port->iter->done()) {
	    // non-ib iterators should end at the same time
	    assert((in_port->peer_guid != XFERDES_NO_GUID) || in_port->iter->done());
	    assert((out_port->peer_guid != XFERDES_NO_GUID) || out_port->iter->done());
	    iteration_completed.store_release(true);
	    break;
	  }

	  // no support for serdez ops
	  assert(in_port->serdez_op == 0);
	  assert(out_port->serdez_op == 0);

	  size_t max_bytes = max_req_size;

	  // if we're not the first in the chain, and we know the total bytes
	  //  written by the predecessor, don't exceed that
	  if(in_port->peer_guid != XFERDES_NO_GUID) {
	    size_t pre_max = in_port->remote_bytes_total.load() - in_port->local_bytes_total;
	    if(pre_max == 0) {
	      // due to unsynchronized updates to pre_bytes_total, this path
	      //  can happen for an empty transfer reading from an intermediate
	      //  buffer - handle it by looping around and letting the check
	      //  at the top of the loop notice it the second time around
	      if(in_port->local_bytes_total == 0)
		continue;
	      // otherwise, this shouldn't happen - we should detect this case
	      //  on the the transfer of those last bytes
	      assert(0);
	      iteration_completed.store_release(true);
	      break;
	    }
	    if(pre_max < max_bytes) {
	      log_request.info() << "pred limits xfer: " << max_bytes << " -> " << pre_max;
	      max_bytes = pre_max;
	    }

	    // further limit based on data that has actually shown up
	    max_bytes = in_port->seq_remote.span_exists(in_port->local_bytes_total, max_bytes);
	    if(max_bytes == 0)
	      break;
	  }

	  // similarly, limit our max transfer size based on the amount the
	  //  destination IB buffer can take (assuming there is an IB)
	  if(out_port->peer_guid != XFERDES_NO_GUID) {
	    max_bytes = out_port->seq_remote.span_exists(out_port->local_bytes_total, max_bytes);
	    if(max_bytes == 0)
	      break;
	  }

	  // HDF5 uses its own address info, instead of src/dst, we
	  //  distinguish between hdf5 and mem
	  TransferIterator *hdf5_iter = ((kind == XFER_HDF5_READ) ?
					   in_port->iter :
					   out_port->iter);
	  TransferIterator *mem_iter = ((kind == XFER_HDF5_READ) ?
					  out_port->iter :
					  in_port->iter);

	  TransferIterator::AddressInfo mem_info;
	  TransferIterator::AddressInfoHDF5 hdf5_info;

	  // always ask the HDF5 size for a step first
	  size_t hdf5_bytes = hdf5_iter->step_hdf5(max_bytes, hdf5_info,
						   true /*tentative*/);
          if(hdf5_bytes == 0) {
            // not enough space for even a single element - try again later
            break;
          }
	  // TODO: support 2D/3D for memory side of an HDF transfer?
	  size_t mem_bytes = mem_iter->step(hdf5_bytes, mem_info, 0,
					    true /*tentative*/);
	  if(mem_bytes == hdf5_bytes) {
	    // looks good - confirm the steps
	    hdf5_iter->confirm_step();
	    mem_iter->confirm_step();
	  } else {
	    // cancel the hdf5 step and try to just step by mem_bytes
	    assert(mem_bytes < hdf5_bytes);  // should never be larger
	    hdf5_iter->cancel_step();
	    hdf5_bytes = hdf5_iter->step_hdf5(mem_bytes, hdf5_info);
	    // multi-dimensional hdf5 iterators may round down the size,
	    //  so re-check the mem bytes
	    if(hdf5_bytes == mem_bytes) {
	      mem_iter->confirm_step();
	    } else {
	      mem_iter->cancel_step();
	      mem_bytes = mem_iter->step(hdf5_bytes, mem_info, 0);
	      // now must match
	      assert(hdf5_bytes == mem_bytes);
	    }
	  }

	  HDF5Request* new_req = (HDF5Request *)(dequeue_request());
	  new_req->src_port_idx = in_port_idx;
	  new_req->dst_port_idx = out_port_idx;
	  new_req->dim = Request::DIM_1D;
	  new_req->mem_base = ((kind == XFER_HDF5_READ) ?
			         out_port->mem :
			         in_port->mem)->get_direct_ptr(mem_info.base_offset,
							       mem_info.bytes_per_chunk);
	  // we'll open datasets on the first touch in this transfer
	  // (TODO: pre-open at instance attach time, but in thread-safe way)
	  HDF5::HDF5Dataset *dset;
	  {
	    std::map<FieldID, HDF5::HDF5Dataset *>::const_iterator it = datasets.find(hdf5_info.field_id);
	    if(it != datasets.end()) {
	      dset = it->second;
	    } else {
	      dset = HDF5::HDF5Dataset::open(hdf5_info.filename->c_str(),
					     hdf5_info.dsetname->c_str(),
					     (kind == XFER_HDF5_READ));
	      assert(dset != 0);
	      assert(hdf5_info.extent.size() == size_t(dset->ndims));
	      datasets[hdf5_info.field_id] = dset;
	    }
	  }

	  new_req->dataset_id = dset->dset_id;
	  new_req->datatype_id = dset->dtype_id;

	  std::vector<hsize_t> mem_dims = hdf5_info.extent;
	  CHECK_HDF5( new_req->mem_space_id = H5Screate_simple(mem_dims.size(), mem_dims.data(), NULL) );
	  //std::vector<hsize_t> mem_start(DIM, 0);
	  //CHECK_HDF5( H5Sselect_hyperslab(new_req->mem_space_id, H5S_SELECT_SET, ms_start, NULL, count, NULL) );

	  CHECK_HDF5( new_req->file_space_id = H5Screate_simple(hdf5_info.dset_bounds.size(), hdf5_info.dset_bounds.data(), 0) );
	  CHECK_HDF5( H5Sselect_hyperslab(new_req->file_space_id, H5S_SELECT_SET, hdf5_info.offset.data(), 0, hdf5_info.extent.data(), 0) );

	  new_req->nbytes = hdf5_bytes;

	  new_req->read_seq_pos = in_port->local_bytes_total;
	  new_req->read_seq_count = hdf5_bytes;

	  // update bytes read unless we're using indirection
	  if(in_port->indirect_port_idx < 0) 
	    in_port->local_bytes_total += hdf5_bytes;

	  new_req->write_seq_pos = out_port->local_bytes_total;
	  new_req->write_seq_count = hdf5_bytes;
	  out_port->local_bytes_total += hdf5_bytes;

	  requests[idx++] = new_req;

	  // make sure iteration_completed is set appropriately before we
	  //  process the request (so that multi-hop successors are notified
	  //  properly)
	  if(hdf5_iter->done())
	    iteration_completed.store_release(true);
	}

	return idx;
      }

      bool HDF5XferDes::progress_xd(HDF5Channel *channel, TimeLimit work_until)
      {
	Request *rq;
	bool did_work = false;
	do {
	  long count = get_requests(&rq, 1);
	  if(count > 0) {
	    channel->submit(&rq, count);
	    did_work = true;
	  } else
	    break;
	} while(!work_until.is_expired());

	return did_work;
      }

      void HDF5XferDes::notify_request_read_done(Request* req)
      {
	default_notify_request_read_done(req);
      }

      void HDF5XferDes::notify_request_write_done(Request* req)
      {
        HDF5Request* hdf_req = (HDF5Request*) req;
        //pthread_rwlock_wrlock(&hdf_metadata->hdf_memory->rwlock);
        CHECK_HDF5( H5Sclose(hdf_req->mem_space_id) );
        CHECK_HDF5( H5Sclose(hdf_req->file_space_id) );
        //pthread_rwlock_unlock(&hdf_metadata->hdf_memory->rwlock);

	default_notify_request_write_done(req);
      }

      void HDF5XferDes::flush()
      {
        if (kind == XFER_HDF5_READ) {
        } else {
          assert(kind == XFER_HDF5_WRITE);
	  //CHECK_HDF5( H5Fflush(hdf_metadata->file_id, H5F_SCOPE_LOCAL) );
          // for (fit = oas_vec.begin(); fit != oas_vec.end(); fit++) {
          //   off_t hdf_idx = fit->dst_offset;
          //   hid_t dataset_id = hdf_metadata->dataset_ids[hdf_idx];
          //   //TODO: I am not sure if we need a lock here to protect HDFflush
          //   H5Fflush(dataset_id, H5F_SCOPE_LOCAL);
          // }
        }

	for(std::map<FieldID, HDF5::HDF5Dataset *>::const_iterator it = datasets.begin();
	    it != datasets.end();
	    ++it)
	  it->second->close();
	datasets.clear();
      }
#endif

      std::ostream& operator<<(std::ostream& os, const Channel::SupportedPath& p)
      {
	switch(p.src_type) {
	case Channel::SupportedPath::SPECIFIC_MEMORY:
	  { os << "src=" << p.src_mem; break; }
	case Channel::SupportedPath::LOCAL_KIND:
	  { os << "src=" << p.src_kind << "(lcl)"; break; }
	case Channel::SupportedPath::GLOBAL_KIND:
	  { os << "src=" << p.src_kind << "(gbl)"; break; }
	case Channel::SupportedPath::LOCAL_RDMA:
	  { os << "src=rdma(lcl)"; break; }
	case Channel::SupportedPath::REMOTE_RDMA:
	  { os << "src=rdma(rem)"; break; }
	default:
	  assert(0);
	}
	switch(p.dst_type) {
	case Channel::SupportedPath::SPECIFIC_MEMORY:
	  { os << " dst=" << p.dst_mem; break; }
	case Channel::SupportedPath::LOCAL_KIND:
	  { os << " dst=" << p.dst_kind << "(lcl)"; break; }
	case Channel::SupportedPath::GLOBAL_KIND:
	  { os << " dst=" << p.dst_kind << "(gbl)"; break; }
	case Channel::SupportedPath::LOCAL_RDMA:
	  { os << " dst=rdma(lcl)"; break; }
	case Channel::SupportedPath::REMOTE_RDMA:
	  { os << " dst=rdma(rem)"; break; }
	default:
	  assert(0);
	}
	os << " bw=" << p.bandwidth << " lat=" << p.latency;
	if(p.serdez_allowed)
	  os << " serdez";
	if(p.redops_allowed)
	  os << " redop";
	return os;
      }
	  
      void Channel::print(std::ostream& os) const
      {
	os << "channel{ node=" << node << " kind=" << kind << " paths=[";
	if(!paths.empty()) {
	  for(std::vector<SupportedPath>::const_iterator it = paths.begin();
	      it != paths.end();
	      ++it)
	    os << "\n    " << *it;
	  os << "\n";
	}
	os << "] }";
      }

      const std::vector<Channel::SupportedPath>& Channel::get_paths(void) const
      {
	return paths;
      }
	  
      bool Channel::supports_path(Memory src_mem, Memory dst_mem,
				  CustomSerdezID src_serdez_id,
				  CustomSerdezID dst_serdez_id,
				  ReductionOpID redop_id,
				  XferDesKind *kind_ret /*= 0*/,
				  unsigned *bw_ret /*= 0*/,
				  unsigned *lat_ret /*= 0*/)
      {
	for(std::vector<SupportedPath>::const_iterator it = paths.begin();
	    it != paths.end();
	    ++it) {
	  if(!it->serdez_allowed && ((src_serdez_id != 0) ||
				     (dst_serdez_id != 0)))
	    continue;
	  if(!it->redops_allowed && (redop_id != 0))
	    continue;

	  bool src_ok = false;
	  switch(it->src_type) {
	    case SupportedPath::SPECIFIC_MEMORY: {
	      src_ok = (src_mem == it->src_mem);
	      break;
	    }
	    case SupportedPath::LOCAL_KIND: {
	      src_ok = ((src_mem.kind() == it->src_kind) &&
			(NodeID(ID(src_mem).memory_owner_node()) == node));
	      break;
	    }
	    case SupportedPath::GLOBAL_KIND: {
	      src_ok = (src_mem.kind() == it->src_kind) ;
	      break;
	    }
	    case SupportedPath::LOCAL_RDMA: {
	      if(NodeID(ID(src_mem).memory_owner_node()) == node) {
		MemoryImpl *src_impl = get_runtime()->get_memory_impl(src_mem);
		// detection of rdma-ness depends on whether memory is
		//  local/remote to us, not the channel
		if(NodeID(ID(src_mem).memory_owner_node()) == Network::my_node_id) {
		  src_ok = (src_impl->get_rdma_info(Network::single_network) != nullptr);
		} else {
		  RemoteAddress dummy;
		  src_ok = src_impl->get_remote_addr(0, dummy);
		}
	      }
	      break;
	    }
	    case SupportedPath::REMOTE_RDMA: {
	      if(NodeID(ID(src_mem).memory_owner_node()) != node) {
		MemoryImpl *src_impl = get_runtime()->get_memory_impl(src_mem);
		// detection of rdma-ness depends on whether memory is
		//  local/remote to us, not the channel
		if(NodeID(ID(src_mem).memory_owner_node()) == Network::my_node_id) {
		  src_ok = (src_impl->get_rdma_info(Network::single_network) != nullptr);
		} else {
		  RemoteAddress dummy;
		  src_ok = src_impl->get_remote_addr(0, dummy);
		}
	      }
	      break;
	    }
	  }
	  if(!src_ok)
	    continue;

	  bool dst_ok = false;
	  switch(it->dst_type) {
	    case SupportedPath::SPECIFIC_MEMORY: {
	      dst_ok = (dst_mem == it->dst_mem);
	      break;
	    }
	    case SupportedPath::LOCAL_KIND: {
	      dst_ok = ((dst_mem.kind() == it->dst_kind) &&
			(NodeID(ID(dst_mem).memory_owner_node()) == node));
	      break;
	    }
	    case SupportedPath::GLOBAL_KIND: {
	      dst_ok = (dst_mem.kind() == it->dst_kind) ;
	      break;
	    }
	    case SupportedPath::LOCAL_RDMA: {
	      if(NodeID(ID(dst_mem).memory_owner_node()) == node) {
		MemoryImpl *dst_impl = get_runtime()->get_memory_impl(dst_mem);
		// detection of rdma-ness depends on whether memory is
		//  local/remote to us, not the channel
		if(NodeID(ID(dst_mem).memory_owner_node()) == Network::my_node_id) {
		  dst_ok = (dst_impl->get_rdma_info(Network::single_network) != nullptr);
		} else {
		  RemoteAddress dummy;
		  dst_ok = dst_impl->get_remote_addr(0, dummy);
		}
	      }
	      break;
	    }
	    case SupportedPath::REMOTE_RDMA: {
	      if(NodeID(ID(dst_mem).memory_owner_node()) != node) {
		MemoryImpl *dst_impl = get_runtime()->get_memory_impl(dst_mem);
		// detection of rdma-ness depends on whether memory is
		//  local/remote to us, not the channel
		if(NodeID(ID(dst_mem).memory_owner_node()) == Network::my_node_id) {
		  dst_ok = (dst_impl->get_rdma_info(Network::single_network) != nullptr);
		} else {
		  RemoteAddress dummy;
		  dst_ok = dst_impl->get_remote_addr(0, dummy);
		}
	      }
	      break;
	    }
	  }
	  if(!dst_ok)
	    continue;

	  // match
	  if(kind_ret) *kind_ret = it->xd_kind;
	  if(bw_ret) *bw_ret = it->bandwidth;
	  if(lat_ret) *lat_ret = it->latency;
	  return true;
	}

	return false;
      }

      void Channel::add_path(Memory src_mem, Memory dst_mem,
			     unsigned bandwidth, unsigned latency,
			     bool redops_allowed, bool serdez_allowed,
			     XferDesKind xd_kind)
      {
	size_t idx = paths.size();
	paths.resize(idx + 1);
	SupportedPath &p = paths[idx];
	p.src_type = SupportedPath::SPECIFIC_MEMORY;
	p.src_mem = src_mem;
	p.dst_type = SupportedPath::SPECIFIC_MEMORY;
	p.dst_mem = dst_mem;
	p.bandwidth = bandwidth;
	p.latency = latency;
	p.redops_allowed = redops_allowed;
	p.serdez_allowed = serdez_allowed;
	p.xd_kind = xd_kind;
      }

      void Channel::add_path(Memory src_mem, Memory::Kind dst_kind, bool dst_global,
			     unsigned bandwidth, unsigned latency,
			     bool redops_allowed, bool serdez_allowed,
			     XferDesKind xd_kind)
      {
	size_t idx = paths.size();
	paths.resize(idx + 1);
	SupportedPath &p = paths[idx];
	p.src_type = SupportedPath::SPECIFIC_MEMORY;
	p.src_mem = src_mem;
	p.dst_type = (dst_global ? SupportedPath::GLOBAL_KIND :
		                   SupportedPath::LOCAL_KIND);
	p.dst_kind = dst_kind;
	p.bandwidth = bandwidth;
	p.latency = latency;
	p.redops_allowed = redops_allowed;
	p.serdez_allowed = serdez_allowed;
	p.xd_kind = xd_kind;
      }

      void Channel::add_path(Memory::Kind src_kind, bool src_global,
			     Memory::Kind dst_kind, bool dst_global,
			     unsigned bandwidth, unsigned latency,
			     bool redops_allowed, bool serdez_allowed,
			     XferDesKind xd_kind)
      {
	size_t idx = paths.size();
	paths.resize(idx + 1);
	SupportedPath &p = paths[idx];
	p.src_type = (src_global ? SupportedPath::GLOBAL_KIND :
		                   SupportedPath::LOCAL_KIND);
	p.src_kind = src_kind;
	p.dst_type = (dst_global ? SupportedPath::GLOBAL_KIND :
		                   SupportedPath::LOCAL_KIND);
	p.dst_kind = dst_kind;
	p.bandwidth = bandwidth;
	p.latency = latency;
	p.redops_allowed = redops_allowed;
	p.serdez_allowed = serdez_allowed;
	p.xd_kind = xd_kind;
      }

      // TODO: allow rdma path to limit by kind?
      void Channel::add_path(bool local_loopback,
			     unsigned bandwidth, unsigned latency,
			     bool redops_allowed, bool serdez_allowed,
			     XferDesKind xd_kind)
      {
	size_t idx = paths.size();
	paths.resize(idx + 1);
	SupportedPath &p = paths[idx];
	p.src_type = SupportedPath::LOCAL_RDMA;
	p.dst_type = (local_loopback ? SupportedPath::LOCAL_RDMA :
		                       SupportedPath::REMOTE_RDMA);
	p.bandwidth = bandwidth;
	p.latency = latency;
	p.redops_allowed = redops_allowed;
	p.serdez_allowed = serdez_allowed;
	p.xd_kind = xd_kind;
      }

      long Channel::progress_xd(XferDes *xd, long max_nr)
      {
	const long MAX_NR = 8;
	Request *requests[MAX_NR];
	long nr_got = xd->get_requests(requests, std::min(max_nr, MAX_NR));
	if(nr_got == 0) return 0;
	long nr_submitted = submit(requests, nr_got);
	assert(nr_got == nr_submitted);
	return nr_submitted;
      }

      RemoteChannel::RemoteChannel(void)
	: Channel(XFER_NONE)
      {}

      void RemoteChannel::shutdown()
      {}

      long RemoteChannel::submit(Request** requests, long nr)
      {
	assert(0);
	return 0;
      }

      void RemoteChannel::pull()
      {
	assert(0);
      }

      long RemoteChannel::available()
      {
	assert(0);
	return 0;
      }

      bool RemoteChannel::supports_path(Memory src_mem, Memory dst_mem,
					CustomSerdezID src_serdez_id,
					CustomSerdezID dst_serdez_id,
					ReductionOpID redop_id,
					XferDesKind *kind_ret /*= 0*/,
					unsigned *bw_ret /*= 0*/,
					unsigned *lat_ret /*= 0*/)
      {
	// simultaneous serialization/deserialization not
	//  allowed anywhere right now
	if((src_serdez_id != 0) && (dst_serdez_id != 0))
	  return false;

	// fall through to normal checks
	return Channel::supports_path(src_mem, dst_mem,
				      src_serdez_id, dst_serdez_id,
				      redop_id,
				      kind_ret, bw_ret, lat_ret);
      }

      static const Memory::Kind cpu_mem_kinds[] = { Memory::SYSTEM_MEM,
						    Memory::REGDMA_MEM,
						    Memory::Z_COPY_MEM,
                                                    Memory::SOCKET_MEM };
      static const size_t num_cpu_mem_kinds = sizeof(cpu_mem_kinds) / sizeof(cpu_mem_kinds[0]);

      MemcpyChannel::MemcpyChannel(BackgroundWorkManager *bgwork)
	: SingleXDQChannel<MemcpyChannel,MemcpyXferDes>(bgwork,
							XFER_MEM_CPY,
							"memcpy channel")
      {
        //cbs = (MemcpyRequest**) calloc(max_nr, sizeof(MemcpyRequest*));
	unsigned bw = 0; // TODO
	unsigned latency = 0;
	// any combination of SYSTEM/REGDMA/Z_COPY/SOCKET_MEM
	for(size_t i = 0; i < num_cpu_mem_kinds; i++)
	  for(size_t j = 0; j < num_cpu_mem_kinds; j++)
	    add_path(cpu_mem_kinds[i], false,
		     cpu_mem_kinds[j], false,
		     bw, latency, true, true, XFER_MEM_CPY);

	xdq.add_to_manager(bgwork);
      }

      MemcpyChannel::~MemcpyChannel()
      {
        //free(cbs);
      }

      bool MemcpyChannel::supports_path(Memory src_mem, Memory dst_mem,
					CustomSerdezID src_serdez_id,
					CustomSerdezID dst_serdez_id,
					ReductionOpID redop_id,
					XferDesKind *kind_ret /*= 0*/,
					unsigned *bw_ret /*= 0*/,
					unsigned *lat_ret /*= 0*/)
      {
	// simultaneous serialization/deserialization not
	//  allowed anywhere right now
	if((src_serdez_id != 0) && (dst_serdez_id != 0))
	  return false;

	// fall through to normal checks
	return Channel::supports_path(src_mem, dst_mem,
				      src_serdez_id, dst_serdez_id,
				      redop_id,
				      kind_ret, bw_ret, lat_ret);
      }

      long MemcpyChannel::submit(Request** requests, long nr)
      {
        MemcpyRequest** mem_cpy_reqs = (MemcpyRequest**) requests;
        for (long i = 0; i < nr; i++) {
          MemcpyRequest* req = mem_cpy_reqs[i];
	  // handle 1-D, 2-D, and 3-D in a single loop
	  switch(req->dim) {
	  case Request::DIM_1D:
	    assert(req->nplanes == 1);
	    assert(req->nlines == 1);
	    break;
	  case Request::DIM_2D:
	    assert(req->nplanes == 1);
	    break;
	  case Request::DIM_3D:
	    // nothing to check
	    break;
	  default:
	    assert(0);
	  }
	  size_t rewind_src = 0;
	  size_t rewind_dst = 0;
	  XferDes::XferPort *in_port = &req->xd->input_ports[req->src_port_idx];
	  XferDes::XferPort *out_port = &req->xd->output_ports[req->dst_port_idx];
	  const CustomSerdezUntyped *src_serdez_op = in_port->serdez_op;
	  const CustomSerdezUntyped *dst_serdez_op = out_port->serdez_op;
	  if(src_serdez_op && !dst_serdez_op) {
	    // we manage write_bytes_total, write_seq_{pos,count}
	    req->write_seq_pos = out_port->local_bytes_total;
	  }
	  if(!src_serdez_op && dst_serdez_op) {
	    // we manage read_bytes_total, read_seq_{pos,count}
	    req->read_seq_pos = in_port->local_bytes_total;
	  }
	  {
	    char *wrap_buffer = 0;
	    bool wrap_buffer_malloced = false;
	    const size_t ALLOCA_LIMIT = 4096;
	    const char *src_p = (const char *)(req->src_base);
	    char *dst_p = (char *)(req->dst_base);
	    for (size_t j = 0; j < req->nplanes; j++) {
	      const char *src = src_p;
	      char *dst = dst_p;
	      for (size_t i = 0; i < req->nlines; i++) {
		if(src_serdez_op) {
		  if(dst_serdez_op) {
		    // serialization AND deserialization
		    assert(0);
		  } else {
		    // serialization
		    size_t field_size = src_serdez_op->sizeof_field_type;
		    size_t num_elems = req->nbytes / field_size;
		    assert((num_elems * field_size) == req->nbytes);
		    size_t maxser_size = src_serdez_op->max_serialized_size;
		    size_t max_bytes = num_elems * maxser_size;
		    // ask the dst iterator (which should be a
		    //  WrappingFIFOIterator for enough space to write all the
		    //  serialized data in the worst case
		    TransferIterator::AddressInfo dst_info;
		    size_t bytes_avail = out_port->iter->step(max_bytes,
							      dst_info,
							      0,
							      true /*tentative*/);
		    size_t bytes_used;
		    if(bytes_avail == max_bytes) {
		      // got enough space to do it all in one go
		      void *dst = out_port->mem->get_direct_ptr(dst_info.base_offset,
								bytes_avail);
		      assert(dst != 0);
		      bytes_used = src_serdez_op->serialize(src,
							    field_size,
							    num_elems,
							    dst);
		      if(bytes_used == max_bytes) {
			out_port->iter->confirm_step();
		      } else {
			out_port->iter->cancel_step();
			bytes_avail = out_port->iter->step(bytes_used,
							   dst_info,
							   0,
							   false /*!tentative*/);
			assert(bytes_avail == bytes_used);
		      }
		    } else {
		      // we didn't get the worst case amount, but it might be
		      //  enough
		      void *dst = out_port->mem->get_direct_ptr(dst_info.base_offset,
								bytes_avail);
		      assert(dst != 0);
		      size_t elems_done = 0;
		      size_t bytes_left = bytes_avail;
		      bytes_used = 0;
		      while((elems_done < num_elems) &&
			    (bytes_left >= maxser_size)) {
			size_t todo = std::min(num_elems - elems_done,
					       bytes_left / maxser_size);
			size_t amt = src_serdez_op->serialize(((const char *)src) + (elems_done * field_size),
							      field_size,
							      todo,
							      dst);
			assert(amt <= bytes_left);
			elems_done += todo;
			bytes_left -= amt;
			dst = ((char *)dst) + amt;
			bytes_used += amt;
		      }
		      if(elems_done == num_elems) {
			// we ended up getting all we needed without wrapping
			if(bytes_used == bytes_avail) {
			  out_port->iter->confirm_step();
			} else {
			  out_port->iter->cancel_step();
			  bytes_avail = out_port->iter->step(bytes_used,
							     dst_info,
							     0,
							     false /*!tentative*/);
			  assert(bytes_avail == bytes_used);
			}
		      } else {
			// did we get lucky and finish on the wrap boundary?
			if(bytes_left == 0) {
			  out_port->iter->confirm_step();
			} else {
			  // need a temp buffer to deal with wraparound
			  if(!wrap_buffer) {
			    if(maxser_size > ALLOCA_LIMIT) {
			      wrap_buffer_malloced = true;
			      wrap_buffer = (char *)malloc(maxser_size);
			    } else {
			      wrap_buffer = (char *)alloca(maxser_size);
			    }
			  }
			  while((elems_done < num_elems) && (bytes_left > 0)) {
			    // serialize one element into our buffer
			    size_t amt = src_serdez_op->serialize(((const char *)src) + (elems_done * field_size),
								  wrap_buffer);
			    if(amt < bytes_left) {
			      memcpy(dst, wrap_buffer, amt);
			      bytes_left -= amt;
			      dst = ((char *)dst) + amt;
			    } else {
			      memcpy(dst, wrap_buffer, bytes_left);
			      out_port->iter->confirm_step();
			      if(amt > bytes_left) {
				size_t amt2 = out_port->iter->step(amt - bytes_left,
								      dst_info,
								      0,
								      false /*!tentative*/);
				assert(amt2 == (amt - bytes_left));
				void *dst = out_port->mem->get_direct_ptr(dst_info.base_offset,
									     amt2);
				assert(dst != 0);
				memcpy(dst, wrap_buffer+bytes_left, amt2);
			      }
			      bytes_left = 0;
			    }
			    elems_done++;
			    bytes_used += amt;
			  }
			  // if we still finished with bytes left over, give 
			  //  them back to the iterator
			  if(bytes_left > 0) {
			    assert(elems_done == num_elems);
			    out_port->iter->cancel_step();
			    size_t amt = out_port->iter->step(bytes_used,
							      dst_info,
							      0,
							      false /*!tentative*/);
			    assert(amt == bytes_used);
			  }
			}

			// now that we're after the wraparound, any remaining
			//  elements are fairly straightforward
			if(elems_done < num_elems) {
			  size_t max_remain = ((num_elems - elems_done) * maxser_size);
			  size_t amt = out_port->iter->step(max_remain,
							    dst_info,
							    0,
							    true /*tentative*/);
			  assert(amt == max_remain); // no double-wrap
			  void *dst = out_port->mem->get_direct_ptr(dst_info.base_offset,
								    amt);
			  assert(dst != 0);
			  size_t amt2 = src_serdez_op->serialize(((const char *)src) + (elems_done * field_size),
								 field_size,
								 num_elems - elems_done,
								 dst);
			  bytes_used += amt2;
			  if(amt2 == max_remain) {
			    out_port->iter->confirm_step();
			  } else {
			    out_port->iter->cancel_step();
			    size_t amt3 = out_port->iter->step(amt2,
							       dst_info,
							       0,
							       false /*!tentative*/);
			    assert(amt3 == amt2);
			  }
			}
		      }
		    }
		    assert(bytes_used <= max_bytes);
		    if(bytes_used < max_bytes)
		      rewind_dst += (max_bytes - bytes_used);
		    out_port->local_bytes_total += bytes_used;
		  }
		} else {
		  if(dst_serdez_op) {
		    // deserialization
		    size_t field_size = dst_serdez_op->sizeof_field_type;
		    size_t num_elems = req->nbytes / field_size;
		    assert((num_elems * field_size) == req->nbytes);
		    size_t maxser_size = dst_serdez_op->max_serialized_size;
		    size_t max_bytes = num_elems * maxser_size;
		    // ask the srct iterator (which should be a
		    //  WrappingFIFOIterator for enough space to read all the
		    //  serialized data in the worst case
		    TransferIterator::AddressInfo src_info;
		    size_t bytes_avail = in_port->iter->step(max_bytes,
							     src_info,
							     0,
							     true /*tentative*/);
		    size_t bytes_used;
		    if(bytes_avail == max_bytes) {
		      // got enough space to do it all in one go
		      const void *src = in_port->mem->get_direct_ptr(src_info.base_offset,
								     bytes_avail);
		      assert(src != 0);
		      bytes_used = dst_serdez_op->deserialize(dst,
							      field_size,
							      num_elems,
							      src);
		      if(bytes_used == max_bytes) {
			in_port->iter->confirm_step();
		      } else {
			in_port->iter->cancel_step();
			bytes_avail = in_port->iter->step(bytes_used,
							  src_info,
							  0,
							  false /*!tentative*/);
			assert(bytes_avail == bytes_used);
		      }
		    } else {
		      // we didn't get the worst case amount, but it might be
		      //  enough
		      const void *src = in_port->mem->get_direct_ptr(src_info.base_offset,
								     bytes_avail);
		      assert(src != 0);
		      size_t elems_done = 0;
		      size_t bytes_left = bytes_avail;
		      bytes_used = 0;
		      while((elems_done < num_elems) &&
			    (bytes_left >= maxser_size)) {
			size_t todo = std::min(num_elems - elems_done,
					       bytes_left / maxser_size);
			size_t amt = dst_serdez_op->deserialize(((char *)dst) + (elems_done * field_size),
								field_size,
								todo,
								src);
			assert(amt <= bytes_left);
			elems_done += todo;
			bytes_left -= amt;
			src = ((const char *)src) + amt;
			bytes_used += amt;
		      }
		      if(elems_done == num_elems) {
			// we ended up getting all we needed without wrapping
			if(bytes_used == bytes_avail) {
			  in_port->iter->confirm_step();
			} else {
			  in_port->iter->cancel_step();
			  bytes_avail = in_port->iter->step(bytes_used,
							    src_info,
							    0,
							    false /*!tentative*/);
			  assert(bytes_avail == bytes_used);
			}
		      } else {
			// did we get lucky and finish on the wrap boundary?
			if(bytes_left == 0) {
			  in_port->iter->confirm_step();
			} else {
			  // need a temp buffer to deal with wraparound
			  if(!wrap_buffer) {
			    if(maxser_size > ALLOCA_LIMIT) {
			      wrap_buffer_malloced = true;
			      wrap_buffer = (char *)malloc(maxser_size);
			    } else {
			      wrap_buffer = (char *)alloca(maxser_size);
			    }
			  }
			  // keep a snapshot of the iterator in cse we don't wrap after all
			  Serialization::DynamicBufferSerializer dbs(64);
			  dbs << *(in_port->iter);
			  memcpy(wrap_buffer, src, bytes_left);
			  // get pointer to data on other side of wrap
			  in_port->iter->confirm_step();
			  size_t amt = in_port->iter->step(max_bytes - bytes_avail,
							   src_info,
							   0,
							   true /*tentative*/);
			  // it's actually ok for this to appear to come up short - due to
			  //  flow control we know we won't ever actually wrap around
			  //assert(amt == (max_bytes - bytes_avail));
			  const void *src = in_port->mem->get_direct_ptr(src_info.base_offset,
									 amt);
			  assert(src != 0);
			  memcpy(wrap_buffer + bytes_left, src, maxser_size - bytes_left);
			  src = ((const char *)src) + (maxser_size - bytes_left);

			  while((elems_done < num_elems) && (bytes_left > 0)) {
			    // deserialize one element from our buffer
			    amt = dst_serdez_op->deserialize(((char *)dst) + (elems_done * field_size),
							     wrap_buffer);
			    if(amt < bytes_left) {
			      // slide data, get a few more bytes
			      memmove(wrap_buffer,
				      wrap_buffer + amt,
				      maxser_size - amt);
			      memcpy(wrap_buffer + maxser_size, src, amt);
			      bytes_left -= amt;
			      src = ((const char *)src) + amt;
			    } else {
			      // update iterator to say how much wrapped data was actually used
			      in_port->iter->cancel_step();
			      if(amt > bytes_left) {
				size_t amt2 = in_port->iter->step(amt - bytes_left,
								  src_info,
								  0,
								  false /*!tentative*/);
				assert(amt2 == (amt - bytes_left));
			      }
			      bytes_left = 0;
			    }
			    elems_done++;
			    bytes_used += amt;
			  }
			  // if we still finished with bytes left, we have
			  //  to restore the iterator because we
			  //  can't double-cancel
			  if(bytes_left > 0) {
			    assert(elems_done == num_elems);
			    delete in_port->iter;
			    Serialization::FixedBufferDeserializer fbd(dbs.get_buffer(), dbs.bytes_used());
			    in_port->iter = TransferIterator::deserialize_new(fbd);
			    in_port->iter->cancel_step();
			    size_t amt2 = in_port->iter->step(bytes_used,
							      src_info,
							      0,
							      false /*!tentative*/);
			    assert(amt2 == bytes_used);
			  }
			}

			// now that we're after the wraparound, any remaining
			//  elements are fairly straightforward
			if(elems_done < num_elems) {
			  size_t max_remain = ((num_elems - elems_done) * maxser_size);
			  size_t amt = in_port->iter->step(max_remain,
							   src_info,
							   0,
							   true /*tentative*/);
			  assert(amt == max_remain); // no double-wrap
			  const void *src = in_port->mem->get_direct_ptr(src_info.base_offset,
									 amt);
			  assert(src != 0);
			  size_t amt2 = dst_serdez_op->deserialize(((char *)dst) + (elems_done * field_size),
								   field_size,
								   num_elems - elems_done,
								   src);
			  bytes_used += amt2;
			  if(amt2 == max_remain) {
			    in_port->iter->confirm_step();
			  } else {
			    in_port->iter->cancel_step();
			    size_t amt3 = in_port->iter->step(amt2,
							      src_info,
							      0,
							      false /*!tentative*/);
			    assert(amt3 == amt2);
			  }
			}
		      }
		    }
		    assert(bytes_used <= max_bytes);
		    if(bytes_used < max_bytes)
		      rewind_src += (max_bytes - bytes_used);
		    in_port->local_bytes_total += bytes_used;
		  } else {
		    // normal copy
		    memcpy(dst, src, req->nbytes);
		  }
		}
		if(req->dim == Request::DIM_1D) break;
		// serdez cases update src/dst directly
		// NOTE: this looks backwards, but it's not - a src serdez means it's the
		//  destination that moves unpredictably
		if(!dst_serdez_op) src += req->src_str;
		if(!src_serdez_op) dst += req->dst_str;
	      }
	      if((req->dim == Request::DIM_1D) ||
		 (req->dim == Request::DIM_2D)) break;
	      // serdez cases update src/dst directly - copy back to src/dst_p
	      src_p = (dst_serdez_op ? src : src_p + req->src_pstr);
	      dst_p = (src_serdez_op ? dst : dst_p + req->dst_pstr);
	    }
	    // clean up our wrap buffer, if we malloc'd it
	    if(wrap_buffer_malloced)
	      free(wrap_buffer);
	  }
	  if(src_serdez_op && !dst_serdez_op) {
	    // we manage write_bytes_total, write_seq_{pos,count}
	    req->write_seq_count = out_port->local_bytes_total - req->write_seq_pos;
	    if(rewind_dst > 0) {
	      //log_request.print() << "rewind dst: " << rewind_dst;
	      out_port->local_bytes_cons.fetch_sub(rewind_dst);
	    }
	  } else
	    assert(rewind_dst == 0);
	  if(!src_serdez_op && dst_serdez_op) {
	    // we manage read_bytes_total, read_seq_{pos,count}
	    req->read_seq_count = in_port->local_bytes_total - req->read_seq_pos;
	    if(rewind_src > 0) {
	      //log_request.print() << "rewind src: " << rewind_src;
	      in_port->local_bytes_cons.fetch_sub(rewind_src);
	    }
	  } else
	      assert(rewind_src == 0);
          req->xd->notify_request_read_done(req);
          req->xd->notify_request_write_done(req);
        }
        return nr;
        /*
        pending_lock.lock();
        //if (nr > 0)
          //printf("MemcpyChannel::submit[nr = %ld]\n", nr);
        for (long i = 0; i < nr; i++) {
          pending_queue.push_back(mem_cpy_reqs[i]);
        }
        if (sleep_threads) {
          pthread_cond_broadcast(&pending_cond);
          sleep_threads = false;
        }
        pending_lock.unlock();
        return nr;
        */
        /*
        for (int i = 0; i < nr; i++) {
          push_request(mem_cpy_reqs[i]);
          memcpy(mem_cpy_reqs[i]->dst_buf, mem_cpy_reqs[i]->src_buf, mem_cpy_reqs[i]->nbytes);
          mem_cpy_reqs[i]->xd->notify_request_read_done(mem_cpy_reqs[i]);
          mem_cpy_reqs[i]->xd->notify_request_write_done(mem_cpy_reqs[i]);
        }
        return nr;
        */
      }


      GASNetChannel::GASNetChannel(BackgroundWorkManager *bgwork,
				   XferDesKind _kind)
	: SingleXDQChannel<GASNetChannel, GASNetXferDes>(bgwork,
							 _kind,
							 stringbuilder() << "gasnet channel (kind= " << _kind << ")")
      {
	unsigned bw = 0; // TODO
	unsigned latency = 0;
	// any combination of SYSTEM/REGDMA/Z_COPY/SOCKET_MEM
	for(size_t i = 0; i < num_cpu_mem_kinds; i++)
	  if(_kind == XFER_GASNET_READ)
	    add_path(Memory::GLOBAL_MEM, true,
		     cpu_mem_kinds[i], false,
		     bw, latency, false, false, XFER_GASNET_READ);
	  else
	    add_path(cpu_mem_kinds[i], false,
		     Memory::GLOBAL_MEM, true,
		     bw, latency, false, false, XFER_GASNET_WRITE);
      }

      GASNetChannel::~GASNetChannel()
      {
      }

      long GASNetChannel::submit(Request** requests, long nr)
      {
        for (long i = 0; i < nr; i++) {
          GASNetRequest* req = (GASNetRequest*) requests[i];
	  // no serdez support
	  assert(req->xd->input_ports[req->src_port_idx].serdez_op == 0);
	  assert(req->xd->output_ports[req->dst_port_idx].serdez_op == 0);
          switch (kind) {
            case XFER_GASNET_READ:
            {
	      req->xd->input_ports[req->src_port_idx].mem->get_bytes(req->gas_off,
								     req->mem_base,
								     req->nbytes);
              break;
            }
            case XFER_GASNET_WRITE:
            {
	      req->xd->output_ports[req->dst_port_idx].mem->put_bytes(req->gas_off,
								      req->mem_base,
								      req->nbytes);
              break;
            }
            default:
              assert(0);
          }
          req->xd->notify_request_read_done(req);
          req->xd->notify_request_write_done(req);
        }
        return nr;
      }

      RemoteWriteChannel::RemoteWriteChannel(BackgroundWorkManager *bgwork)
	: SingleXDQChannel<RemoteWriteChannel, RemoteWriteXferDes>(bgwork,
								   XFER_REMOTE_WRITE,
								   "remote write channel")
      {
	unsigned bw = 0; // TODO
	unsigned latency = 0;
	// any combination of SYSTEM/REGDMA/Z_COPY/SOCKET_MEM
	// for(size_t i = 0; i < num_cpu_mem_kinds; i++)
	//   add_path(cpu_mem_kinds[i], false,
	// 	   Memory::REGDMA_MEM, true,
	// 	   bw, latency, false, false, XFER_REMOTE_WRITE);
	add_path(false /*!local_loopback*/,
		 bw, latency,
		 false /*!redops*/, false /*!serdez*/,
		 XFER_REMOTE_WRITE);
      }

      RemoteWriteChannel::~RemoteWriteChannel() {}

      long RemoteWriteChannel::submit(Request** requests, long nr)
      {
        for (long i = 0; i < nr; i ++) {
          RemoteWriteRequest* req = (RemoteWriteRequest*) requests[i];
	  XferDes::XferPort *in_port = &req->xd->input_ports[req->src_port_idx];
	  XferDes::XferPort *out_port = &req->xd->output_ports[req->dst_port_idx];
	  // no serdez support
	  assert((in_port->serdez_op == 0) && (out_port->serdez_op == 0));
	  NodeID dst_node = ID(out_port->mem->me).memory_owner_node();
	  size_t write_bytes_total = (size_t)-1;
	  if(out_port->needs_pbt_update.load() &&
	     req->xd->iteration_completed.load_acquire()) {
	    // this can result in sending the pbt twice, but this code path
	    //  is "mostly dead" and should be nuked soon
	    out_port->needs_pbt_update.store(false);
	    write_bytes_total = out_port->local_bytes_total;
	  }
	  RemoteAddress dst_buf;
	  bool ok = out_port->mem->get_remote_addr(req->dst_off, dst_buf);
	  assert(ok);
	  // send a request if there's data or if there's a next XD to update
	  if((req->nbytes > 0) ||
	     (out_port->peer_guid != XferDes::XFERDES_NO_GUID)) {
	    if (req->dim == Request::DIM_1D) {
	      XferDesRemoteWriteMessage::send_request(
                dst_node, dst_buf, req->src_base, req->nbytes, req,
		out_port->peer_guid, out_port->peer_port_idx,
		req->write_seq_pos, req->write_seq_count, 
		write_bytes_total);
	    } else {
	      assert(req->dim == Request::DIM_2D);
	      // dest MUST be continuous
	      assert(req->nlines <= 1 || ((size_t)req->dst_str) == req->nbytes);
	      XferDesRemoteWriteMessage::send_request(
                dst_node, dst_buf, req->src_base, req->nbytes,
                req->src_str, req->nlines, req,
		out_port->peer_guid, out_port->peer_port_idx,
		req->write_seq_pos, req->write_seq_count, 
		write_bytes_total);
	    }
	  }
	  // for an empty transfer, we do the local completion ourselves
	  //   instead of waiting for an ack from the other node
	  if(req->nbytes == 0) {
	    req->xd->notify_request_read_done(req);
	    req->xd->notify_request_write_done(req);
	  }
        /*RemoteWriteRequest* req = (RemoteWriteRequest*) requests[i];
          req->complete_event = GenEventImpl::create_genevent()->current_event();
          Realm::RemoteWriteMessage::RequestArgs args;
          args.mem = req->dst_mem;
          args.offset = req->dst_offset;
          args.event = req->complete_event;
          args.sender = Network::my_node_id;
          args.sequence_id = 0;

          Realm::RemoteWriteMessage::Message::request(ID(args.mem).node(), args,
                                                      req->src_buf, req->nbytes,
                                                      PAYLOAD_KEEPREG,
                                                      req->dst_buf);*/
        }
        return nr;
      }

#ifdef REALM_USE_CUDA
      GPUChannel::GPUChannel(Cuda::GPU* _src_gpu, XferDesKind _kind,
			     BackgroundWorkManager *bgwork)
	: SingleXDQChannel<GPUChannel,GPUXferDes>(bgwork,
						  _kind,
						  stringbuilder() << "cuda channel (gpu=" << _src_gpu->info->index << " kind=" << (int)_kind << ")")
      {
        src_gpu = _src_gpu;

	Memory fbm = src_gpu->fbmem->me;

	switch(_kind) {
	case XFER_GPU_TO_FB:
	  {
	    unsigned bw = 0; // TODO
	    unsigned latency = 0;
	    for(std::set<Memory>::const_iterator it = src_gpu->pinned_sysmems.begin();
		it != src_gpu->pinned_sysmems.end();
		++it)
	      add_path(*it, fbm, bw, latency, false, false,
		       XFER_GPU_TO_FB);

	    break;
	  }

	case XFER_GPU_FROM_FB:
	  {
	    unsigned bw = 0; // TODO
	    unsigned latency = 0;
	    for(std::set<Memory>::const_iterator it = src_gpu->pinned_sysmems.begin();
		it != src_gpu->pinned_sysmems.end();
		++it)
	      add_path(fbm, *it, bw, latency, false, false,
		       XFER_GPU_FROM_FB);

	    break;
	  }

	case XFER_GPU_IN_FB:
	  {
	    // self-path
	    unsigned bw = 0; // TODO
	    unsigned latency = 0;
	    add_path(fbm, fbm, bw, latency, false, false,
		     XFER_GPU_IN_FB);

	    break;
	  }

	case XFER_GPU_PEER_FB:
	  {
	    // just do paths to peers - they'll do the other side
	    unsigned bw = 0; // TODO
	    unsigned latency = 0;
	    for(std::set<Memory>::const_iterator it = src_gpu->peer_fbs.begin();
		it != src_gpu->peer_fbs.end();
		++it)
	      add_path(fbm, *it, bw, latency, false, false,
		       XFER_GPU_PEER_FB);

	    break;
	  }

	default:
	  assert(0);
	}
      }

      GPUChannel::~GPUChannel()
      {
      }

      long GPUChannel::submit(Request** requests, long nr)
      {
        for (long i = 0; i < nr; i++) {
          GPURequest* req = (GPURequest*) requests[i];
	  // no serdez support
	  assert(req->xd->input_ports[req->src_port_idx].serdez_op == 0);
	  assert(req->xd->output_ports[req->dst_port_idx].serdez_op == 0);

	  // empty transfers don't need to bounce off the GPU
	  if(req->nbytes == 0) {
	    req->xd->notify_request_read_done(req);
	    req->xd->notify_request_write_done(req);
	    continue;
	  }

	  switch(req->dim) {
	    case Request::DIM_1D: {
	      switch (kind) {
                case XFER_GPU_TO_FB:
		  src_gpu->copy_to_fb(req->dst_off, req->src_base,
				      req->nbytes, &req->event);
		  break;
                case XFER_GPU_FROM_FB:
		  src_gpu->copy_from_fb(req->dst_base, req->src_off,
					req->nbytes, &req->event);
		  break;
                case XFER_GPU_IN_FB:
		  src_gpu->copy_within_fb(req->dst_off, req->src_off,
					  req->nbytes, &req->event);
		  break;
                case XFER_GPU_PEER_FB:
		  src_gpu->copy_to_peer(req->dst_gpu, req->dst_off,
					req->src_off, req->nbytes,
					&req->event);
		  break;
                default:
		  assert(0);
	      }
	      break;
	    }

	    case Request::DIM_2D: {
              switch (kind) {
	        case XFER_GPU_TO_FB:
		  src_gpu->copy_to_fb_2d(req->dst_off, req->src_base,
					 req->dst_str, req->src_str,
					 req->nbytes, req->nlines, &req->event);
		  break;
	        case XFER_GPU_FROM_FB:
		  src_gpu->copy_from_fb_2d(req->dst_base, req->src_off,
					   req->dst_str, req->src_str,
					   req->nbytes, req->nlines,
					   &req->event);
		  break;
                case XFER_GPU_IN_FB:
		  src_gpu->copy_within_fb_2d(req->dst_off, req->src_off,
					     req->dst_str, req->src_str,
					     req->nbytes, req->nlines,
					     &req->event);
		  break;
                case XFER_GPU_PEER_FB:
		  src_gpu->copy_to_peer_2d(req->dst_gpu, req->dst_off,
					   req->src_off, req->dst_str,
					   req->src_str, req->nbytes,
					   req->nlines, &req->event);
		  break;
                default:
		  assert(0);
	      }
	      break;
	    }

	    case Request::DIM_3D: {
              switch (kind) {
	        case XFER_GPU_TO_FB:
		  src_gpu->copy_to_fb_3d(req->dst_off, req->src_base,
					 req->dst_str, req->src_str,
					 req->dst_pstr, req->src_pstr,
					 req->nbytes, req->nlines, req->nplanes,
					 &req->event);
		  break;
	        case XFER_GPU_FROM_FB:
		  src_gpu->copy_from_fb_3d(req->dst_base, req->src_off,
					   req->dst_str, req->src_str,
					   req->dst_pstr, req->src_pstr,
					   req->nbytes, req->nlines, req->nplanes,
					   &req->event);
		  break;
                case XFER_GPU_IN_FB:
		  src_gpu->copy_within_fb_3d(req->dst_off, req->src_off,
					     req->dst_str, req->src_str,
					     req->dst_pstr, req->src_pstr,
					     req->nbytes, req->nlines, req->nplanes,
					     &req->event);
		  break;
                case XFER_GPU_PEER_FB:
		  src_gpu->copy_to_peer_3d(req->dst_gpu,
					   req->dst_off, req->src_off,
					   req->dst_str, req->src_str,
					   req->dst_pstr, req->src_pstr,
					   req->nbytes, req->nlines, req->nplanes,
					   &req->event);
		  break;
                default:
		  assert(0);
	      }
	      break;
	    }

	    default:
	      assert(0);
	  }

          //pending_copies.push_back(req);
        }
        return nr;
      }

      void GPUCompletionEvent::request_completed(void)
      {
	req->xd->notify_request_read_done(req);
	req->xd->notify_request_write_done(req);
      }
#endif

#ifdef REALM_USE_HDF5
      HDF5Channel::HDF5Channel(BackgroundWorkManager *bgwork)
	: SingleXDQChannel<HDF5Channel, HDF5XferDes>(bgwork,
						     XFER_NONE /*FIXME*/,
						     "hdf5 channel")
      {
	unsigned bw = 0; // TODO
	unsigned latency = 0;
	// any combination of SYSTEM/REGDMA/Z_COPY_MEM
	for(size_t i = 0; i < num_cpu_mem_kinds; i++) {
	  add_path(Memory::HDF_MEM, false,
		   cpu_mem_kinds[i], false,
		   bw, latency, false, false, XFER_HDF5_READ);

	  add_path(cpu_mem_kinds[i], false,
		   Memory::HDF_MEM, false,
		   bw, latency, false, false, XFER_HDF5_WRITE);
	}
      }

      HDF5Channel::~HDF5Channel() {}

      long HDF5Channel::submit(Request** requests, long nr)
      {
        HDF5Request** hdf_reqs = (HDF5Request**) requests;
        for (long i = 0; i < nr; i++) {
          HDF5Request* req = hdf_reqs[i];
	  // no serdez support
	  assert(req->xd->input_ports[req->src_port_idx].serdez_op == 0);
	  assert(req->xd->output_ports[req->dst_port_idx].serdez_op == 0);
          //pthread_rwlock_rdlock(req->rwlock);
          if (req->xd->kind == XFER_HDF5_READ)
            CHECK_HDF5( H5Dread(req->dataset_id, req->datatype_id,
				req->mem_space_id, req->file_space_id,
				H5P_DEFAULT, req->mem_base) );
	  else
            CHECK_HDF5( H5Dwrite(req->dataset_id, req->datatype_id,
				 req->mem_space_id, req->file_space_id,
				 H5P_DEFAULT, req->mem_base) );
          //pthread_rwlock_unlock(req->rwlock);
          req->xd->notify_request_read_done(req);
          req->xd->notify_request_write_done(req);
        }
        return nr;
      }
#endif

      /*static*/
      void XferDesRemoteWriteMessage::handle_message(NodeID sender,
						     const XferDesRemoteWriteMessage &args,
						     const void *data,
						     size_t datalen)
      {
        // assert data copy is in right position
        //assert(data == args.dst_buf);

	log_xd.info() << "remote write recieved: next="
		      << std::hex << args.next_xd_guid << std::dec
		      << " start=" << args.span_start
		      << " size=" << args.span_size
		      << " pbt=" << args.pre_bytes_total;

	// if requested, notify (probably-local) next XD
	if(args.next_xd_guid != XferDes::XFERDES_NO_GUID) {
	  if(args.pre_bytes_total != size_t(-1))
	    xferDes_queue->update_pre_bytes_total(args.next_xd_guid,
						  args.next_port_idx,
						  args.pre_bytes_total);
	  xferDes_queue->update_pre_bytes_write(args.next_xd_guid,
						args.next_port_idx,
						args.span_start,
						args.span_size);
	}

	// don't ack empty requests
	if(datalen > 0)
	  XferDesRemoteWriteAckMessage::send_request(sender, args.req);
      }

      /*static*/
      void XferDesRemoteWriteAckMessage::handle_message(NodeID sender,
							const XferDesRemoteWriteAckMessage &args,
							const void *data,
							size_t datalen)
      {
        RemoteWriteRequest* req = args.req;
        req->xd->notify_request_read_done(req);
        req->xd->notify_request_write_done(req);
      }

      /*static*/ void XferDesDestroyMessage::handle_message(NodeID sender,
							    const XferDesDestroyMessage &args,
							    const void *msgdata,
							    size_t msglen)
      {
        xferDes_queue->destroy_xferDes(args.guid);
      }

      /*static*/ void UpdateBytesTotalMessage::handle_message(NodeID sender,
							      const UpdateBytesTotalMessage &args,
							      const void *msgdata,
							      size_t msglen)
      {
        xferDes_queue->update_pre_bytes_total(args.guid,
					      args.port_idx,
					      args.pre_bytes_total);
      }

      /*static*/ void UpdateBytesWriteMessage::handle_message(NodeID sender,
							      const UpdateBytesWriteMessage &args,
							      const void *msgdata,
							      size_t msglen)
      {
        xferDes_queue->update_pre_bytes_write(args.guid,
					      args.port_idx,
					      args.span_start,
					      args.span_size);
      }

      /*static*/ void UpdateBytesReadMessage::handle_message(NodeID sender,
							    const UpdateBytesReadMessage &args,
							    const void *msgdata,
							    size_t msglen)
      {
        xferDes_queue->update_next_bytes_read(args.guid,
					      args.port_idx,
					      args.span_start,
					      args.span_size);
      }

      XferDesQueue* get_xdq_singleton()
      {
        return xferDes_queue;
      }

      ChannelManager* get_channel_manager()
      {
        return channel_manager;
      }

      ChannelManager::~ChannelManager(void) {
      }

      MemcpyChannel* ChannelManager::create_memcpy_channel(BackgroundWorkManager *bgwork)
      {
        assert(memcpy_channel == NULL);
        memcpy_channel = new MemcpyChannel(bgwork);
        return memcpy_channel;
      }
      GASNetChannel* ChannelManager::create_gasnet_read_channel(BackgroundWorkManager *bgwork) {
        assert(gasnet_read_channel == NULL);
        gasnet_read_channel = new GASNetChannel(bgwork, XFER_GASNET_READ);
        return gasnet_read_channel;
      }
      GASNetChannel* ChannelManager::create_gasnet_write_channel(BackgroundWorkManager *bgwork) {
        assert(gasnet_write_channel == NULL);
        gasnet_write_channel = new GASNetChannel(bgwork, XFER_GASNET_WRITE);
        return gasnet_write_channel;
      }
      RemoteWriteChannel* ChannelManager::create_remote_write_channel(BackgroundWorkManager *bgwork) {
        assert(remote_write_channel == NULL);
        remote_write_channel = new RemoteWriteChannel(bgwork);
        return remote_write_channel;
      }
#ifdef REALM_USE_CUDA
      GPUChannel* ChannelManager::create_gpu_to_fb_channel(Cuda::GPU* src_gpu,
							   BackgroundWorkManager *bgwork) {
        gpu_to_fb_channels[src_gpu] = new GPUChannel(src_gpu,
						     XFER_GPU_TO_FB,
						     bgwork);
        return gpu_to_fb_channels[src_gpu];
      }
      GPUChannel* ChannelManager::create_gpu_from_fb_channel(Cuda::GPU* src_gpu,
							     BackgroundWorkManager *bgwork) {
        gpu_from_fb_channels[src_gpu] = new GPUChannel(src_gpu,
						       XFER_GPU_FROM_FB,
						       bgwork);
        return gpu_from_fb_channels[src_gpu];
      }
      GPUChannel* ChannelManager::create_gpu_in_fb_channel(Cuda::GPU* src_gpu,
							   BackgroundWorkManager *bgwork) {
        gpu_in_fb_channels[src_gpu] = new GPUChannel(src_gpu,
						     XFER_GPU_IN_FB,
						     bgwork);
        return gpu_in_fb_channels[src_gpu];
      }
      GPUChannel* ChannelManager::create_gpu_peer_fb_channel(Cuda::GPU* src_gpu,
							     BackgroundWorkManager *bgwork) {
        gpu_peer_fb_channels[src_gpu] = new GPUChannel(src_gpu,
						       XFER_GPU_PEER_FB,
						       bgwork);
        return gpu_peer_fb_channels[src_gpu];
      }
#endif
#ifdef REALM_USE_HDF5
      HDF5Channel* ChannelManager::create_hdf5_channel(BackgroundWorkManager *bgwork) {
        assert(hdf5_channel == NULL);
        hdf5_channel = new HDF5Channel(bgwork);
        return hdf5_channel;
      }
#endif
      AddressSplitChannel *ChannelManager::create_addr_split_channel(BackgroundWorkManager *bgwork) {
	assert(addr_split_channel == 0);
	addr_split_channel = new AddressSplitChannel(bgwork);
	return addr_split_channel;
      }

#ifdef REALM_USE_CUDA
      void register_gpu_in_dma_systems(Cuda::GPU* gpu)
      {
        dma_all_gpus.push_back(gpu);
      }
#endif
      void start_channel_manager(BackgroundWorkManager *bgwork)
      {
        xferDes_queue = new XferDesQueue;
        channel_manager = new ChannelManager;
        xferDes_queue->start_worker(channel_manager, bgwork);
      }
      FileChannel* ChannelManager::create_file_channel(BackgroundWorkManager *bgwork) {
        assert(file_channel == NULL);
        file_channel = new FileChannel(bgwork);
        return file_channel;
      }
      DiskChannel* ChannelManager::create_disk_channel(BackgroundWorkManager *bgwork) {
        assert(disk_channel == NULL);
        disk_channel = new DiskChannel(bgwork);
        return disk_channel;
      }

      void XferDesQueue::update_pre_bytes_write(XferDesID xd_guid, int port_idx,
						size_t span_start, size_t span_size)
      {
        NodeID execution_node = xd_guid >> (NODE_BITS + INDEX_BITS);
        if (execution_node == Network::my_node_id) {
	  RWLock::AutoWriterLock al(guid_lock);
          std::map<XferDesID, XferDesWithUpdates>::iterator it = guid_to_xd.find(xd_guid);
          if (it != guid_to_xd.end()) {
            if (it->second.xd != NULL) {
	      it->second.xd->update_pre_bytes_write(port_idx, span_start, span_size);
            } else {
	      it->second.seq_pre_write[port_idx].add_span(span_start, span_size);
            }
          } else {
            XferDesWithUpdates& xdup = guid_to_xd[xd_guid];
	    xdup.seq_pre_write[port_idx].add_span(span_start, span_size);
          }
        }
        else {
	  // this should never happen?  (i.e. it should be built into whatever
	  //  message delivered the data)
	  assert(0);
#if 0
          // send a active message to remote node
          UpdateBytesWriteMessage::send_request(execution_node, xd_guid,
						port_idx,
						span_start, span_size,
						pre_bytes_total);
#endif
        }
      }

      void XferDesQueue::update_pre_bytes_total(XferDesID xd_guid, int port_idx,
						size_t pre_bytes_total)
      {
        NodeID execution_node = xd_guid >> (NODE_BITS + INDEX_BITS);
        if (execution_node == Network::my_node_id) {
	  RWLock::AutoWriterLock al(guid_lock);
          std::map<XferDesID, XferDesWithUpdates>::iterator it = guid_to_xd.find(xd_guid);
          if (it != guid_to_xd.end()) {
            if (it->second.xd != NULL) {
	      it->second.xd->update_pre_bytes_total(port_idx, pre_bytes_total);
            } else {
	      // should never get more than one update
	      assert(it->second.pre_bytes_total.count(port_idx) == 0);
	      it->second.pre_bytes_total[port_idx] = pre_bytes_total;
            }
          } else {
            XferDesWithUpdates& xdup = guid_to_xd[xd_guid];
	    xdup.pre_bytes_total[port_idx] = pre_bytes_total;
          }
        }
        else {
          // send an active message to remote node
	  ActiveMessage<UpdateBytesTotalMessage> amsg(execution_node);
	  amsg->guid = xd_guid;
	  amsg->port_idx = port_idx;
	  amsg->pre_bytes_total = pre_bytes_total;
	  amsg.commit();
        }
      }

      void XferDesQueue::update_next_bytes_read(XferDesID xd_guid, int port_idx,
						size_t span_start, size_t span_size)
      {
        NodeID execution_node = xd_guid >> (NODE_BITS + INDEX_BITS);
        if (execution_node == Network::my_node_id) {
	  RWLock::AutoReaderLock al(guid_lock);
          std::map<XferDesID, XferDesWithUpdates>::iterator it = guid_to_xd.find(xd_guid);
          if (it != guid_to_xd.end()) {
	    assert(it->second.xd != NULL);
	    it->second.xd->update_next_bytes_read(port_idx, span_start, span_size);
	  } else {
            // This means this update goes slower than future updates, which marks
            // completion of xfer des (ID = xd_guid). In this case, it is safe to drop the update
	  }
        }
        else {
          // send a active message to remote node
          UpdateBytesReadMessage::send_request(execution_node, xd_guid,
					       port_idx,
					       span_start, span_size);
        }
      }

      bool XferDesQueue::enqueue_xferDes_local(XferDes* xd,
					       bool add_to_queue /*= true*/)
      {
	Event wait_on = xd->request_metadata();
	if(!wait_on.has_triggered()) {
	  log_new_dma.info() << "xd metadata wait: xd=" << xd->guid << " ready=" << wait_on;
	  xd->deferred_enqueue.defer(xferDes_queue, xd, wait_on);
	  return false;
	}

	{
	  RWLock::AutoWriterLock al(guid_lock);
	  std::map<XferDesID, XferDesWithUpdates>::iterator git = guid_to_xd.find(xd->guid);
	  if (git != guid_to_xd.end()) {
	    // xerDes_queue has received updates of this xferdes
	    // need to integrate these updates into xferdes
	    assert(git->second.xd == NULL);
	    git->second.xd = xd;
	    for(std::map<int, size_t>::const_iterator it = git->second.pre_bytes_total.begin();
		it != git->second.pre_bytes_total.end();
		++it)
	      xd->input_ports[it->first].remote_bytes_total.store(it->second);
	    for(std::map<int, SequenceAssembler>::iterator it = git->second.seq_pre_write.begin();
		it != git->second.seq_pre_write.end();
		++it)
	      xd->input_ports[it->first].seq_remote.swap(it->second);
	  } else {
	    XferDesWithUpdates& xdup = guid_to_xd[xd->guid];
	    xdup.xd = xd;
	  }
	}

	if(!add_to_queue) return true;
	assert(0);

	return true;
      }

      void XferDesQueue::start_worker(ChannelManager* channel_manager,
				      BackgroundWorkManager *bgwork)
      {
	RuntimeImpl *r = get_runtime();

	// TODO: numa-specific channels
        MemcpyChannel* memcpy_channel = channel_manager->create_memcpy_channel(bgwork);
	GASNetChannel* gasnet_read_channel = channel_manager->create_gasnet_read_channel(bgwork);
	GASNetChannel* gasnet_write_channel = channel_manager->create_gasnet_write_channel(bgwork);
	AddressSplitChannel *addr_split_channel = channel_manager->create_addr_split_channel(bgwork);
	r->add_dma_channel(memcpy_channel);
	r->add_dma_channel(gasnet_read_channel);
	r->add_dma_channel(gasnet_write_channel);
	r->add_dma_channel(addr_split_channel);

	RemoteWriteChannel *remote_channel = channel_manager->create_remote_write_channel(bgwork);
	DiskChannel *disk_channel = channel_manager->create_disk_channel(bgwork);
	FileChannel *file_channel = channel_manager->create_file_channel(bgwork);
        r->add_dma_channel(remote_channel);
	r->add_dma_channel(disk_channel);
	r->add_dma_channel(file_channel);
#ifdef REALM_USE_HDF5
	HDF5Channel *hdf5_channel = channel_manager->create_hdf5_channel(bgwork);
	r->add_dma_channel(hdf5_channel);
#endif

#ifdef REALM_USE_CUDA
        std::vector<Cuda::GPU*>::iterator it;
        for (it = dma_all_gpus.begin(); it != dma_all_gpus.end(); it ++) {
	  GPUChannel *gpu_to_fb_channel = channel_manager->create_gpu_to_fb_channel(*it, bgwork);
	  GPUChannel *gpu_from_fb_channel = channel_manager->create_gpu_from_fb_channel(*it, bgwork);
	  GPUChannel *gpu_in_fb_channel = channel_manager->create_gpu_in_fb_channel(*it, bgwork);
	  GPUChannel *gpu_peer_fb_channel = channel_manager->create_gpu_peer_fb_channel(*it, bgwork);
          r->add_dma_channel(gpu_to_fb_channel);
          r->add_dma_channel(gpu_from_fb_channel);
          r->add_dma_channel(gpu_in_fb_channel);
          r->add_dma_channel(gpu_peer_fb_channel);
        }
#endif

      }

      void stop_channel_manager()
      {
        xferDes_queue->stop_worker();
        delete xferDes_queue;
        delete channel_manager;
      }

      void XferDesQueue::stop_worker() {
      }

      void XferDes::DeferredXDEnqueue::defer(XferDesQueue *_xferDes_queue,
					     XferDes *_xd, Event wait_on)
      {
	xferDes_queue = _xferDes_queue;
	xd = _xd;
	Realm::EventImpl::add_waiter(wait_on, this);
      }

      void XferDes::DeferredXDEnqueue::event_triggered(bool poisoned,
						       TimeLimit work_until)
      {
	// TODO: handle poisoning
	assert(!poisoned);
	log_new_dma.info() << "xd metadata ready: xd=" << xd->guid;
	xd->channel->enqueue_ready_xd(xd);
	//xferDes_queue->enqueue_xferDes_local(xd);
      }

      void XferDes::DeferredXDEnqueue::print(std::ostream& os) const
      {
	os << "deferred xd enqueue: xd=" << xd->guid;
      }

      Event XferDes::DeferredXDEnqueue::get_finish_event(void) const
      {
	// TODO: would be nice to provide dma op's finish event here
	return Event::NO_EVENT;
      }

    void destroy_xfer_des(XferDesID _guid)
    {
      log_new_dma.info("Destroy XferDes: id(" IDFMT ")", _guid);
      NodeID execution_node = _guid >> (XferDesQueue::NODE_BITS + XferDesQueue::INDEX_BITS);
      if (execution_node == Network::my_node_id) {
        xferDes_queue->destroy_xferDes(_guid);
      }
      else {
        XferDesDestroyMessage::send_request(execution_node, _guid);
      }
    }

#define CREATE_MESSAGE_HANDLER(type) \
ActiveMessageHandlerReg<XferDesCreateMessage<type> > xfer_des_create_ ## type ## _message_handler
CREATE_MESSAGE_HANDLER(MemcpyXferDes);
CREATE_MESSAGE_HANDLER(GASNetXferDes);
CREATE_MESSAGE_HANDLER(RemoteWriteXferDes);
CREATE_MESSAGE_HANDLER(DiskXferDes);
CREATE_MESSAGE_HANDLER(FileXferDes);
#ifdef REALM_USE_CUDA
CREATE_MESSAGE_HANDLER(GPUXferDes);
#endif
#ifdef REALM_USE_HDF5
CREATE_MESSAGE_HANDLER(HDF5XferDes);
#endif

ActiveMessageHandlerReg<NotifyXferDesCompleteMessage> notify_xfer_des_complete_handler;
ActiveMessageHandlerReg<XferDesRemoteWriteMessage> xfer_des_remote_write_handler;
ActiveMessageHandlerReg<XferDesRemoteWriteAckMessage> xfer_des_remote_write_ack_handler;
ActiveMessageHandlerReg<XferDesDestroyMessage> xfer_des_destroy_message_handler;
ActiveMessageHandlerReg<UpdateBytesTotalMessage> update_bytes_total_message_handler;
ActiveMessageHandlerReg<UpdateBytesWriteMessage> update_bytes_write_message_handler;
ActiveMessageHandlerReg<UpdateBytesReadMessage> update_bytes_read_message_handler;
ActiveMessageHandlerReg<RemoteWriteXferDes::Write1DMessage> remote_write_1d_message_handler;

}; // namespace Realm


