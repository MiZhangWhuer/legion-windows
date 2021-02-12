/* Copyright 2020 Stanford University, NVIDIA Corporation
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

// Realm inter-node networking abstractions

#include "realm/network.h"
#include "realm/cmdline.h"

static void *aligned_malloc(size_t bytes, size_t alignment)
{
#ifdef REALM_ON_WINDOWS
  return _aligned_malloc(bytes, alignment);
#else
  void *ptr = 0;
  int ret = posix_memalign(&ptr, alignment, bytes);
  return ((ret == 0) ? ptr : 0);
#endif
}

static void aligned_free(void *ptr)
{
#ifdef REALM_ON_WINDOWS
  _aligned_free(ptr);
#else
  free(ptr);
#endif
}

namespace Realm {

  namespace Network {
    NodeID my_node_id = 0;
    NodeID max_node_id = 0;
    NodeSet all_peers;
    NetworkModule *single_network = 0;
  }


  ////////////////////////////////////////////////////////////////////////
  //
  // class NetworkModule
  //

  NetworkModule::NetworkModule(const std::string& _name)
    : Module(_name)
  {}

  void NetworkModule::parse_command_line(RuntimeImpl *runtime,
					 std::vector<std::string>& cmdline)
  {}


  ////////////////////////////////////////////////////////////////////////
  //
  // class NetworkSegment
  //

  NetworkSegment::NetworkSegment()
    : base(0), bytes(0), alignment(0)
    , memtype(NetworkSegmentInfo::Unknown)
    , memextra(0)
    , single_network(0), single_network_data(0)
  {}

#if 0
  // normally a request will just be for a particular size
  inline NetworkSegment::NetworkSegment(size_t _bytes, size_t _alignment)
    : base(0), bytes(_bytes), alignment(_alignment)
    , single_network(0), single_network_data(0)
  {}

  // but it can also be for a pre-allocated chunk of memory with a fixed address
  inline NetworkSegment::NetworkSegment(void *_base, size_t _bytes)
    : base(_base), bytes(_bytes), alignment(0)
    , single_network(0), single_network_data(0)
  {}
#endif

  void NetworkSegment::request(NetworkSegmentInfo::MemoryType _memtype,
			       size_t _bytes, size_t _alignment,
			       NetworkSegmentInfo::MemoryTypeExtraData _memextra /*= 0*/)
  {
    memtype = _memtype;
    bytes = _bytes;
    alignment = _alignment;
    memextra = _memextra;
  }

  void NetworkSegment::assign(NetworkSegmentInfo::MemoryType _memtype,
			      void *_base, size_t _bytes,
			      NetworkSegmentInfo::MemoryTypeExtraData _memextra /*= 0*/)
  {
    memtype = _memtype;
    base = _base;
    bytes = _bytes;
    memextra = _memextra;
  }


  ////////////////////////////////////////////////////////////////////////
  //
  // class LoopbackNetworkModule
  //

  // used when there are no other networks
  
  class LoopbackNetworkModule : public NetworkModule {
  protected:
    LoopbackNetworkModule();

  public:
    static NetworkModule *create_network_module(RuntimeImpl *runtime,
						int *argc, const char ***argv);

    // actual parsing of the command line should wait until here if at all
    //  possible
    virtual void parse_command_line(RuntimeImpl *runtime,
				    std::vector<std::string>& cmdline);

    // "attaches" to the network, if that is meaningful - attempts to
    //  bind/register/(pick your network-specific verb) the requested memory
    //  segments with the network
    virtual void attach(RuntimeImpl *runtime,
			std::vector<NetworkSegment *>& segments);

    // detaches from the network
    virtual void detach(RuntimeImpl *runtime,
			std::vector<NetworkSegment *>& segments);

    // collective communication within this network
    virtual void barrier(void);
    virtual void broadcast(NodeID root,
			   const void *val_in, void *val_out, size_t bytes);
    virtual void gather(NodeID root,
			const void *val_in, void *vals_out, size_t bytes);
    virtual bool check_for_quiescence(void);

    // used to create a remote proxy for a memory
    virtual MemoryImpl *create_remote_memory(Memory m, size_t size, Memory::Kind kind,
					     const ByteArray& rdma_info);
    virtual IBMemory *create_remote_ib_memory(Memory m, size_t size, Memory::Kind kind,
					      const ByteArray& rdma_info);

    virtual ActiveMessageImpl *create_active_message_impl(NodeID target,
							  unsigned short msgid,
							  size_t header_size,
							  size_t max_payload_size,
							  const void *src_payload_addr,
							  size_t src_payload_lines,
							  size_t src_payload_line_stride,
							  void *storage_base,
							  size_t storage_size);

    virtual ActiveMessageImpl *create_active_message_impl(NodeID target,
							  unsigned short msgid,
							  size_t header_size,
							  size_t max_payload_size,
							  const void *src_payload_addr,
							  size_t src_payload_lines,
							  size_t src_payload_line_stride,
							  const RemoteAddress& dest_payload_addr,
							  void *storage_base,
							  size_t storage_size);

    virtual ActiveMessageImpl *create_active_message_impl(const NodeSet& targets,
							  unsigned short msgid,
							  size_t header_size,
							  size_t max_payload_size,
							  const void *src_payload_addr,
							  size_t src_payload_lines,
							  size_t src_payload_line_stride,
							  void *storage_base,
							  size_t storage_size);

    virtual size_t recommended_max_payload(NodeID target,
					   bool with_congestion,
					   size_t header_size);
    virtual size_t recommended_max_payload(const NodeSet& targets,
					   bool with_congestion,
					   size_t header_size);
    virtual size_t recommended_max_payload(NodeID target,
					   const RemoteAddress& dest_payload_addr,
					   bool with_congestion,
					   size_t header_size);
    virtual size_t recommended_max_payload(NodeID target,
					   const void *data, size_t bytes_per_line,
					   size_t lines, size_t line_stride,
					   bool with_congestion,
					   size_t header_size);
    virtual size_t recommended_max_payload(const NodeSet& targets,
					   const void *data, size_t bytes_per_line,
					   size_t lines, size_t line_stride,
					   bool with_congestion,
					   size_t header_size);
    virtual size_t recommended_max_payload(NodeID target,
					   const void *data, size_t bytes_per_line,
					   size_t lines, size_t line_stride,
					   const RemoteAddress& dest_payload_addr,
					   bool with_congestion,
					   size_t header_size);
  };

  LoopbackNetworkModule::LoopbackNetworkModule()
    : NetworkModule("loopback")
  {}

  /*static*/ NetworkModule *LoopbackNetworkModule::create_network_module(RuntimeImpl *runtime,
									 int *argc, const char ***argv)
  {
    return new LoopbackNetworkModule;
  }

  // actual parsing of the command line should wait until here if at all
  //  possible
  void LoopbackNetworkModule::parse_command_line(RuntimeImpl *runtime,
						 std::vector<std::string>& cmdline)
  {
    NetworkModule::parse_command_line(runtime, cmdline);

    size_t global_size = 0;
    CommandLineParser cp;
    cp.add_option_int_units("-ll:gsize", global_size, 'm');
    bool ok = cp.parse_command_line(cmdline);
    assert(ok);
    assert((global_size == 0) && "no global mem support in dummy network yet");
  }

  // "attaches" to the network, if that is meaningful - attempts to
  //  bind/register/(pick your network-specific verb) the requested memory
  //  segments with the network
  void LoopbackNetworkModule::attach(RuntimeImpl *runtime,
				     std::vector<NetworkSegment *>& segments)
  {
    // service any still-unbound request by doing a malloc
    for(std::vector<NetworkSegment *>::iterator it = segments.begin();
	it != segments.end();
	++it) {
      if(((*it)->bytes > 0) && ((*it)->base == 0)) {
        void *memptr = aligned_malloc((*it)->bytes,
                                      std::max((*it)->alignment, sizeof(void *)));
	assert(memptr != 0);
	(*it)->base = memptr;
	(*it)->add_rdma_info(this, &memptr, sizeof(void *));
      }
    }
  }

  // detaches from the network
  void LoopbackNetworkModule::detach(RuntimeImpl *runtime,
				     std::vector<NetworkSegment *>& segments)
  {
    // free any segment memory we allocated
    for(std::vector<NetworkSegment *>::iterator it = segments.begin();
	it != segments.end();
	++it) {
      const ByteArray *rdma_info = (*it)->get_rdma_info(this);
      if(rdma_info) {
	aligned_free((*it)->base);
	(*it)->base = 0;
      }
    }
  }

  // collective communication within this network
  void LoopbackNetworkModule::barrier(void)
  {
    // nothing to do
  }
  
  void LoopbackNetworkModule::broadcast(NodeID root,
					const void *val_in, void *val_out,
					size_t bytes)
  {
    memcpy(val_out, val_in, bytes);
  }
  
  void LoopbackNetworkModule::gather(NodeID root,
				     const void *val_in, void *vals_out,
				     size_t bytes)
  {
    memcpy(vals_out, val_in, bytes);
  }

  bool LoopbackNetworkModule::check_for_quiescence(void)
  {
    return true;
  }

  // used to create a remote proxy for a memory
  MemoryImpl *LoopbackNetworkModule::create_remote_memory(Memory m, size_t size, Memory::Kind kind,
							  const ByteArray& rdma_info)
  {
    // should never be called
    abort();
  }
  
  IBMemory *LoopbackNetworkModule::create_remote_ib_memory(Memory m, size_t size, Memory::Kind kind,
							   const ByteArray& rdma_info)
  {
    // should never be called
    abort();
  }
  
  ActiveMessageImpl *LoopbackNetworkModule::create_active_message_impl(NodeID target,
								       unsigned short msgid,
								       size_t header_size,
								       size_t max_payload_size,
								       const void *src_payload_addr,
								       size_t src_payload_lines,
								       size_t src_payload_line_stride,
								       void *storage_base,
								       size_t storage_size)
  {
    // should never be called
    abort();
  }

  ActiveMessageImpl *LoopbackNetworkModule::create_active_message_impl(NodeID target,
								       unsigned short msgid,
								       size_t header_size,
								       size_t max_payload_size,
								       const void *src_payload_addr,
								       size_t src_payload_lines,
								       size_t src_payload_line_stride,
								       const RemoteAddress& dest_payload_addr,
								       void *storage_base,
								       size_t storage_size)
  {
    // should never be called
    abort();
  }

  ActiveMessageImpl *LoopbackNetworkModule::create_active_message_impl(const NodeSet& targets,
								       unsigned short msgid,
								       size_t header_size,
								       size_t max_payload_size,
								       const void *src_payload_addr,
								       size_t src_payload_lines,
								       size_t src_payload_line_stride,
								       void *storage_base,
								       size_t storage_size)
  {
    // should never be called
    abort();
  }
  
  size_t LoopbackNetworkModule::recommended_max_payload(NodeID target,
							bool with_congestion,
							size_t header_size)
  {
    // should never be called
    abort();
    return 0;
  }

  size_t LoopbackNetworkModule::recommended_max_payload(const NodeSet& targets,
							bool with_congestion,
							size_t header_size)
  {
    // should never be called
    abort();
    return 0;
  }

  size_t LoopbackNetworkModule::recommended_max_payload(NodeID target,
							const RemoteAddress& dest_payload_addr,
							bool with_congestion,
							size_t header_size)
  {
    // should never be called
    abort();
    return 0;
  }
  
  size_t LoopbackNetworkModule::recommended_max_payload(NodeID target,
							const void *data, size_t bytes_per_line,
							size_t lines, size_t line_stride,
							bool with_congestion,
							size_t header_size)
  {
    // should never be called
    abort();
    return 0;
  }

  size_t LoopbackNetworkModule::recommended_max_payload(const NodeSet& targets,
							const void *data, size_t bytes_per_line,
							size_t lines, size_t line_stride,
							bool with_congestion,
							size_t header_size)
  {
    // should never be called
    abort();
    return 0;
  }

  size_t LoopbackNetworkModule::recommended_max_payload(NodeID target,
							const void *data, size_t bytes_per_line,
							size_t lines, size_t line_stride,
							const RemoteAddress& dest_payload_addr,
							bool with_congestion,
							size_t header_size)
  {
    // should never be called
    abort();
    return 0;
  }
  

  ////////////////////////////////////////////////////////////////////////
  //
  // class NetworkSegment
  //

  void NetworkSegment::add_rdma_info(NetworkModule *network,
				     const void *data, size_t len)
  {
    ByteArray& ba = networks[network];
    ba.set(data, len);
#ifdef REALM_USE_MULTIPLE_NETWORKS
#else
    assert(single_network == 0);
    single_network = network;
    single_network_data = &ba;
#endif
  }

  const ByteArray *NetworkSegment::get_rdma_info(NetworkModule *network) const
  {
    if(single_network == network) {
      return single_network_data;
    } else {
#ifdef REALM_USE_MULTIPLE_NETWORKS
      std::map<NetworkModule *, ByteArray>::iterator it = networks.find(network);
      if(it != networks.end())
	return &(it->second);
#endif
      return 0;
    }
  }
  
  
  ////////////////////////////////////////////////////////////////////////
  //
  // class ModuleRegistrar
  //

  namespace {
    ModuleRegistrar::NetworkRegistrationBase *network_modules_head = 0;
    ModuleRegistrar::NetworkRegistrationBase **network_modules_tail = &network_modules_head;
  };

  // called by the runtime during init - these may change the command line!
  void ModuleRegistrar::create_network_modules(std::vector<NetworkModule *>& modules,
					       int *argc, const char ***argv)
  {
    // iterate over the network module list, trying to create each module
    bool need_loopback = true;
    for(const NetworkRegistrationBase *nreg = network_modules_head;
	nreg;
	nreg = nreg->next) {
      NetworkModule *m = nreg->create_network_module(runtime, argc, argv);
      if(m) {
	modules.push_back(m);
#ifndef REALM_USE_MULTIPLE_NETWORKS
	assert(Network::single_network == 0);
#endif
	Network::single_network = m;
	need_loopback = false;
      }
    }

    if(need_loopback) {
      NetworkModule *m = LoopbackNetworkModule::create_network_module(runtime, argc, argv);
      assert(m != 0);
      modules.push_back(m);
      assert(Network::single_network == 0);
      Network::single_network = m;
    }
  }

  /*static*/ void ModuleRegistrar::add_network_registration(NetworkRegistrationBase *reg)
  {
    // done during init, so single-threaded
    *network_modules_tail = reg;
    network_modules_tail = &(reg->next);
  }
  

};
