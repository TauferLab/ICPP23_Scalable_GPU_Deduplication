#ifndef KOKKOS_MERKLE_TREE_HPP
#define KOKKOS_MERKLE_TREE_HPP
#include <Kokkos_Core.hpp>
#include <Kokkos_UnorderedMap.hpp>
#include <climits>
#include "hash_functions.hpp"
#include "map_helpers.hpp"
#include "kokkos_queue.hpp"
#include <iostream>
#include "utils.hpp"

#define STATS

//template<uint32_t N>
class MerkleTree {
public:
  Kokkos::View<HashDigest*> tree_d;
  Kokkos::View<HashDigest*>::HostMirror tree_h;
  Kokkos::View<uint8_t*> distinct_children_d;
  Kokkos::View<uint8_t*>::HostMirror distinct_children_h;

  MerkleTree(const uint32_t num_leaves) {
    tree_d = Kokkos::View<HashDigest*>("Merkle tree", (2*num_leaves-1));
    tree_h = Kokkos::create_mirror_view(tree_d);
    distinct_children_d = Kokkos::View<uint8_t*>("Num distinct children", (2*num_leaves-1));
    distinct_children_h = Kokkos::create_mirror_view(distinct_children_d);
    Kokkos::deep_copy(distinct_children_d, 0);
  }
  
  KOKKOS_INLINE_FUNCTION HashDigest& operator()(int32_t i) const {
    return tree_d(i);
  }
 
  void digest_to_hex_(const uint8_t digest[16], char* output) {
    int i,j;
    char* c = output;
    for(i=0; i<16/4; i++) {
      for(j=0; j<4; j++) {
        sprintf(c, "%02X", digest[i*4 + j]);
        c += 2;
      }
      sprintf(c, " ");
      c += 1;
    }
    *(c-1) = '\0';
  }

  void print_leaves() {
    Kokkos::deep_copy(tree_h, tree_d);
    uint32_t num_leaves = (tree_h.extent(0)+1)/2;
    printf("============================================================\n");
    char buffer[64];
    unsigned int counter = 2;
    for(unsigned int i=num_leaves-1; i<tree_h.extent(0); i++) {
      digest_to_hex_((uint8_t*)(tree_h(i).digest), buffer);
      printf("Node: %u: %s \n", i, buffer);
      if(i == counter) {
        printf("\n");
        counter += 2*counter;
      }
    }
    printf("============================================================\n");
  }

  void print() {
    Kokkos::deep_copy(tree_h, tree_d);
printf("Num digests: %lu\n", tree_h.extent(0));
    uint32_t num_leaves = (tree_h.extent(0)+1)/2;
    printf("============================================================\n");
    char buffer[64];
    unsigned int counter = 2;
    for(unsigned int i=16777215; i<16777315; i++) {
      digest_to_hex_((uint8_t*)(tree_h(i).digest), buffer);
      printf("Node: %u: %s \n", i, buffer);
      if(i == counter) {
        printf("\n");
        counter += 2*counter;
      }
    }
    printf("============================================================\n");
  }
};

template <class Hasher>
void create_merkle_tree(Hasher& hasher, MerkleTree& tree, Kokkos::View<uint8_t*>& data, const uint32_t chunk_size, const int32_t n_levels=INT_MAX) {
  uint32_t num_chunks = data.size()/chunk_size;
  if(num_chunks*chunk_size < data.size())
    num_chunks += 1;
  const uint32_t num_nodes = 2*num_chunks-1;
  const uint32_t num_levels = static_cast<uint32_t>(ceil(log2(num_nodes+1)));
  int32_t stop_level = 0;
  if(n_levels < num_levels)
    stop_level = num_levels-n_levels;
  const uint32_t leaf_start = num_chunks-1;
  for(int32_t level=num_levels-1; level>=stop_level; level--) {
    uint32_t nhashes = 1 << level;
    uint32_t start_offset = nhashes-1;
    if(start_offset + nhashes > num_nodes)
      nhashes = num_nodes - start_offset;
    auto range_policy = Kokkos::RangePolicy<>(start_offset, start_offset+nhashes);
    Kokkos::parallel_for("Build tree", range_policy, KOKKOS_LAMBDA(const int i) {
      uint32_t num_bytes = chunk_size;
      if((i-leaf_start) == num_chunks-1)
        num_bytes = data.size()-((i-leaf_start)*chunk_size);
      if(i >= leaf_start) {
        hasher.hash(data.data()+((i-leaf_start)*chunk_size), 
                    num_bytes, 
                    (uint8_t*)(tree(i).digest));
      } else {
        hasher.hash((uint8_t*)&tree(2*i+1), 2*hasher.digest_size(), (uint8_t*)&tree(i));
      }
    });
  }
  Kokkos::fence();
}

template <class Hasher>
MerkleTree create_merkle_tree(Hasher& hasher, Kokkos::View<uint8_t*>& data, const uint32_t chunk_size) {
  uint32_t num_chunks = data.size()/chunk_size;
  if(num_chunks*chunk_size < data.size())
    num_chunks += 1;
  MerkleTree tree = MerkleTree(num_chunks);
  create_merkle_tree(hasher, tree, data, chunk_size, INT_MAX);
  return tree;
}

template <class Hasher>
void create_merkle_tree(Hasher& hasher, MerkleTree& tree, Kokkos::View<uint8_t*>& data, uint32_t chunk_size, uint32_t tree_id, DistinctMap& distinct_map, SharedTreeMap& shared_map) {
//chunk_size=128;
//for(; chunk_size<8192; chunk_size*=2) {
  uint32_t num_chunks = static_cast<uint32_t>(data.size()/chunk_size);
  if(num_chunks*chunk_size < data.size())
    num_chunks += 1;
  const uint32_t num_nodes = 2*num_chunks-1;
  const uint32_t num_levels = static_cast<uint32_t>(ceil(log2(num_nodes+1)));
  const uint32_t leaf_start = num_chunks-1;
  for(int32_t level=num_levels-1; level>=0; level--) {
    DEBUG_PRINT("Processing level %d\n", level);
    uint32_t nhashes = 1 << static_cast<uint32_t>(level);
    uint32_t start_offset = nhashes-1;
    if(start_offset + nhashes > num_nodes)
      nhashes = num_nodes - start_offset;
    DEBUG_PRINT("Computing %u hashes\n", nhashes);
    auto range_policy = Kokkos::RangePolicy<>(start_offset, start_offset+nhashes);
    Kokkos::parallel_for("Build tree", range_policy, KOKKOS_LAMBDA(const uint32_t i) {
      uint32_t num_bytes = chunk_size;
      if((i-leaf_start) == num_chunks-1)
        num_bytes = data.size()-((i-leaf_start)*chunk_size);
      if(i >= leaf_start) {
        hasher.hash(data.data()+((i-leaf_start)*chunk_size), 
                    num_bytes, 
                    (uint8_t*)(tree(i).digest));
      } else {
        hasher.hash((uint8_t*)&tree(2*i+1), 2*hasher.digest_size(), (uint8_t*)&tree(i));
      }
      auto result = distinct_map.insert(tree(i), NodeID(i, tree_id));
      if(result.existing()) {
        auto& entry = distinct_map.value_at(result.index());
//          shared_map.insert(i,entry.node);
          shared_map.insert(i,NodeID(entry.node, entry.tree));
      } else if(result.failed()) {
          printf("Failed to insert node %u into distinct map\n",i);
      }
    });
  }
  Kokkos::fence();
//}
}

//template <class Hasher>
//void create_merkle_tree_subtrees(Hahser& hasher, MerkleTree& tree, Kokkos::View<uint8_t*>& data, const uint32_t chunk_size) {
//  uint32_t num_chunks = data.size()/chunk_size;
//  if(num_chunks*chunk_size < data.size())
//    num_chunks += 1;
//  const uint32_t num_nodes = 2*num_chunks-1;
//  const uint32_t num_levels = static_cast<uint32_t>(ceil(log2(num_nodes+1)));
//  constexpr uint32_t num_threads = 128;
//  const uint32_t leaf_start = num_chunks-1;
//  uint32_t num_leagues = num_chunks/num_threads;
//  if(num_threads*num_leagues < num_chunks)
//    num_leagues += 1;
//  Kokkos::TeamPolicy<> team_policy(num_leagues, Kokkos::AUTO());
//  using team_member_type = Kokkos::TeamPolicy<>::member_type;
//  Kokkos::parallel_for("Build tree by subtrees", team_policy, KOKKOS_LAMBDA(team_member_type team_member) {
//    uint32_t league_offset = team_member.league_rank()*num_threads;
//    uint32_t active_threads = 128;
//    uint32_t n_level = num_levels-1;
//    while(active_threads > 0) {
//      Kokkos::parallel_for("Compute level of subtree", Kokkos::RangePolicy<>(0, active_threads), KOKKOS_LAMBDA(const uint32_t j) {
//        uint32_t i = league_offset + j;
//        uint32_t num_bytes = chunk_size;
//        if((i-leaf_start) == num_chunks-1)
//          num_bytes = data.size()-((i-leaf_start)*chunk_size);
//        if(i >= leaf_start) {
//          hasher.hash(data.data()+((i-leaf_start)*chunk_size), 
//                      num_bytes, 
//                      (uint8_t*)(tree(i).digest));
//        } else {
//          hasher.hash((uint8_t*)&tree(2*i+1), 2*hasher.digest_size(), (uint8_t*)&tree(i));
//        }
//      });
//    }
//  });
//}

//void compare_trees_fused(const MerkleTree& tree, Queue& queue, const uint32_t tree_id, DistinctMap& distinct_map) {
//  uint32_t num_comp = 0;
//  uint32_t q_size = queue.size();
//  while(q_size > 0) {
//    num_comp += q_size;
//    Kokkos::parallel_for("Compare trees", Kokkos::RangePolicy<>(0, q_size), KOKKOS_LAMBDA(const uint32_t entry) {
//      uint32_t node = queue.pop();
//      HashDigest digest = tree(node);
//      NodeInfo info(node, node, tree_id);
//      auto result = distinct_map.insert(digest, info); // Try to insert
//      if(result.success()) { // Node is distinct
//        uint32_t child_l = 2*node+1;
//        if(child_l < queue.capacity())
//          queue.push(child_l);
//        uint32_t child_r = 2*node+2;
//        if(child_r < queue.capacity())
//          queue.push(child_r);
//#ifdef STATS
//      } else {
//        printf("Failed to insert (%u,%u,%u). Already exists.\n", info.node, info.src, info.tree);
//#endif
//      }
//    });
//    q_size = queue.size();
//  }
//
//  printf("Number of comparisons (Merkle Tree): %u\n", num_comp);
//  Kokkos::fence();
//}

//template<typename Scheduler>
//struct CompareTreeTask {
//  using sched_type  = Scheduler;
//  using future_type = Kokkos::BasicFuture<uint32_t, Scheduler>;
//  using value_type  = uint32_t;
//
//  uint32_t node;
//  uint32_t tree_id;
//  MerkleTree tree;
//  DistinctMap distinct_map;
//  future_type child_l_fut;
//  future_type child_r_fut;
//  bool l_active;
//  bool r_active;
//
//  KOKKOS_INLINE_FUNCTION
//  CompareTreeTask(const uint32_t n, const MerkleTree& merkle_tree, const uint32_t treeID, DistinctMap& distinct) : 
//                  node(n), tree_id(treeID), tree(merkle_tree), distinct_map(distinct), l_active(true), r_active(true) {}
//
//  KOKKOS_INLINE_FUNCTION
//  void operator()(typename sched_type::member_type& member, uint32_t& result) {
//    auto& sched = member.scheduler();
//
//    bool child_l_ready = ( (l_active && !child_l_fut.is_null()) || (l_active == false) );
//    bool child_r_ready = ( (r_active && !child_r_fut.is_null()) || (r_active == false) );
//    if((child_l_ready && child_r_ready)) { // On task respawn
//      result = 1 + child_l_fut.get() + child_r_fut.get();
//    } else { // Perform task and spawn for children if needed
//      uint32_t active_children = 0;
//      HashDigest digest = tree(node);
//      NodeInfo info(node, node, tree_id);
//      auto insert_result = distinct_map.insert(digest, info); // Try to insert
//      if(insert_result.success()) { // Node is distinct
//        uint32_t child_l = 2*node+1;
//        if(child_l < tree.tree_d.extent(0)) {
//          child_l_fut = Kokkos::task_spawn(Kokkos::TaskSingle(sched, Kokkos::TaskPriority::High), CompareTreeTask(child_l, tree, tree_id, distinct_map));
//          active_children += 1;
//        } else {
//          l_active = false;
//        }
//        uint32_t child_r = 2*node+2;
//        if(child_r < tree.tree_d.extent(0)) {
//          child_r_fut = Kokkos::task_spawn(Kokkos::TaskSingle(sched, Kokkos::TaskPriority::High), CompareTreeTask(child_r, tree, tree_id, distinct_map));
//          active_children += 1;
//        } else {
//          l_active = false;
//        }
//      }
//      if(active_children == 2) {
//        Kokkos::BasicFuture<void, Scheduler> dep[] = {child_l_fut, child_r_fut};
//        Kokkos::BasicFuture<void, Scheduler> all_children = sched.when_all(dep, 2);
//        Kokkos::respawn(this, all_children, Kokkos::TaskPriority::High);
//      } else if(active_children == 1) {
//        if(l_active) { 
//          Kokkos::respawn(this, child_l_fut, Kokkos::TaskPriority::High);
//        } else {
//          Kokkos::respawn(this, child_r_fut, Kokkos::TaskPriority::High);
//        }
//      } else {
//        result = 1;
//      }
//    }
//  }
//};
//
//void compare_trees_tasks(const MerkleTree& tree, Queue& queue, const uint32_t tree_id, DistinctMap& distinct_map) {
//  using scheduler_type = Kokkos::TaskScheduler<Kokkos::DefaultExecutionSpace>;
//  using memory_space = typename scheduler_type::memory_space;
//  using memory_pool = typename scheduler_type::memory_pool;
//  auto mpool = memory_pool(memory_space{}, estimate_required_memory(tree.tree_d.extent(0)));
//  auto root_sched = scheduler_type(mpool);
//  Kokkos::BasicFuture<uint32_t, scheduler_type> f = Kokkos::host_spawn(Kokkos::TaskSingle(root_sched), 
//                                                                  CompareTreeTask<scheduler_type>(0, tree, tree_id, distinct_map));
//  Kokkos::wait(root_sched);
//  printf("Number of comparisons (Merkle Tree Task): %u\n", f.get());
//}

KOKKOS_INLINE_FUNCTION uint32_t num_leaf_descendents(uint32_t node, uint32_t num_nodes) {
  uint32_t leftmost = (2*node)+1;
  uint32_t rightmost = (2*node)+2;
  while(leftmost < num_nodes) {
    leftmost = (2*leftmost)+1;
    rightmost = (2*rightmost)+2;
  }
  leftmost = (leftmost-1)/2;
  rightmost = (rightmost-2)/2;
  if(rightmost > num_nodes)
    rightmost = num_nodes-1;
  return static_cast<uint32_t>(rightmost-leftmost+1);
}

KOKKOS_INLINE_FUNCTION uint32_t leftmost_leaf(uint32_t node, uint32_t num_nodes) {
  uint32_t leftmost = (2*node)+1;
  uint32_t rightmost = (2*node)+2;
  while(leftmost < num_nodes) {
    leftmost = (2*leftmost)+1;
    rightmost = (2*rightmost)+2;
  }
  leftmost = (leftmost-1)/2;
  rightmost = (rightmost-2)/2;
  if(rightmost > num_nodes)
    rightmost = num_nodes-1;
  return static_cast<uint32_t>(leftmost);
}

KOKKOS_INLINE_FUNCTION uint32_t rightmost_leaf(uint32_t node, uint32_t num_nodes) {
  uint32_t leftmost = (2*node)+1;
  uint32_t rightmost = (2*node)+2;
  while(leftmost < num_nodes) {
    leftmost = (2*leftmost)+1;
    rightmost = (2*rightmost)+2;
  }
  leftmost = (leftmost-1)/2;
  rightmost = (rightmost-2)/2;
  if(rightmost > num_nodes)
    rightmost = num_nodes-1;
  return static_cast<uint32_t>(rightmost);
}

//template<uint32_t N>
//KOKKOS_INLINE_FUNCTION
//void insert_entry(const CompactTable<N>& updates, const uint32_t node, const uint32_t num_nodes, const uint32_t tree_id, const uint32_t prior_node) {
//if(node > num_nodes)
//printf("Something very wrong happened.\n");
////  uint32_t num_chunks = (num_nodes+1)/2;
////  uint32_t size = num_leaf_descendents(node, num_nodes);
////  uint32_t leaf = leftmost_leaf(node, num_nodes);
//  CompactNodeInfo info(node, prior_node);
//  auto result = updates.insert(info);
//  auto& update = updates.value_at(result.index());
//  update.push(tree_id);
//}

KOKKOS_INLINE_FUNCTION
void insert_entry(const CompactTable& updates, const uint32_t node, const uint32_t num_nodes, const uint32_t tree_id, const NodeID node_id) {
  if(node > num_nodes)
    printf("Something very wrong happened.\n");
  auto result = updates.insert(node, NodeID(node_id.node, node_id.tree));
}

//template<class Hasher, uint32_t N>
template<class Hasher>
void deduplicate_data(Kokkos::View<uint8_t*>& data, 
                      const uint32_t chunk_size, 
                      const Hasher hasher, 
                      MerkleTree& tree, 
                      const uint32_t tree_id, 
                      const SharedTreeMap& prior_shared_map, 
                      const DistinctMap& prior_distinct_map, 
                      SharedTreeMap& shared_map, 
                      DistinctMap& distinct_map, 
//                      CompactTable<N>& shared_updates,
//                      CompactTable<N>& distinct_updates) {
                      CompactTable& shared_updates,
                      CompactTable& distinct_updates) {
  uint32_t num_chunks = data.size()/chunk_size;
  if(num_chunks*chunk_size < data.size())
    num_chunks += 1;
  const uint32_t num_nodes = 2*num_chunks-1;
  const uint32_t num_levels = static_cast<uint32_t>(ceil(log2(num_nodes+1)));
  const uint32_t leaf_start = num_chunks-1;

  DEBUG_PRINT("Num prior shared: %u\n", prior_shared_map.size());
  DEBUG_PRINT("Num prior distinct: %u\n", prior_distinct_map.size());

  uint32_t prev_leftover = UINT32_MAX;
  uint32_t current_level = num_levels-1;
  uint32_t start_offset = (1 << (num_levels-1))-1;
  uint32_t end_offset = (1 << num_levels)-1;
  if(end_offset > num_nodes)
    end_offset = num_nodes;
DEBUG_PRINT("Number of chunks: %u\n", num_chunks);
DEBUG_PRINT("Number of nodes: %u\n", num_nodes);
DEBUG_PRINT("Number of levels: %u\n", num_levels);
DEBUG_PRINT("leaf start: %u\n", leaf_start);
DEBUG_PRINT("Start, end offsets: (%u,%u)\n", start_offset, end_offset);
  Kokkos::View<uint32_t[1]> nodes_leftover("Leftover nodes to process");
  Kokkos::View<uint32_t[1]>::HostMirror nodes_leftover_h = Kokkos::create_mirror_view(nodes_leftover);
  Kokkos::deep_copy(nodes_leftover, 0);
  nodes_leftover_h(0) = 0;
#ifdef STATS
  Kokkos::View<uint32_t[1]> num_same("Number of chunks that remain the same");
  Kokkos::View<uint32_t[1]> num_new("Number of chunks that are new");
  Kokkos::View<uint32_t[1]> num_shift("Number of chunks that exist but in different spaces");
  Kokkos::View<uint32_t[1]> num_comp_d("Number of compressed distinct nodes");
  Kokkos::View<uint32_t[1]> num_comp_s("Number of compressed shared nodes");
  Kokkos::View<uint32_t[1]> num_dupl("Number of new duplicate nodes");
  Kokkos::View<uint32_t[1]> num_other("Number of other nodes");
  Kokkos::View<uint32_t[1]>::HostMirror num_same_h = Kokkos::create_mirror_view(num_same);
  Kokkos::View<uint32_t[1]>::HostMirror num_new_h = Kokkos::create_mirror_view(num_new);
  Kokkos::View<uint32_t[1]>::HostMirror num_shift_h = Kokkos::create_mirror_view(num_shift);
  Kokkos::View<uint32_t[1]>::HostMirror num_comp_d_h = Kokkos::create_mirror_view(num_comp_d);
  Kokkos::View<uint32_t[1]>::HostMirror num_comp_s_h = Kokkos::create_mirror_view(num_comp_s);
  Kokkos::View<uint32_t[1]>::HostMirror num_dupl_h = Kokkos::create_mirror_view(num_dupl);
  Kokkos::View<uint32_t[1]>::HostMirror num_other_h = Kokkos::create_mirror_view(num_other);
using DistinctMap = Kokkos::UnorderedMap<HashDigest, 
                                         NodeID, 
                                         Kokkos::CudaUVMSpace, 
//                                         Kokkos::DefaultExecutionSpace, 
                                         digest_hash, 
                                         digest_equal_to>;
  Kokkos::UnorderedMap<HashDigest, void, Kokkos::CudaUVMSpace, digest_hash, digest_equal_to> table(num_chunks);
  Kokkos::View<uint32_t[10]> num_prior_chunks_d("Number of chunks from prior checkpoints");
  Kokkos::View<uint32_t[10]>::HostMirror num_prior_chunks_h = Kokkos::create_mirror_view(num_prior_chunks_d);
  Kokkos::deep_copy(num_same, 0);
  Kokkos::deep_copy(num_new, 0);
  Kokkos::deep_copy(num_shift, 0);
  Kokkos::deep_copy(num_comp_d, 0);
  Kokkos::deep_copy(num_comp_s, 0);
  Kokkos::deep_copy(num_dupl, 0);
  Kokkos::deep_copy(num_other, 0);
  Kokkos::deep_copy(num_prior_chunks_d, 0);
#endif

//  const uint32_t min_chunk_size = 128;
//  const uint32_t num_subchunks = chunk_size/min_chunk_size;

  while(nodes_leftover_h(0) != prev_leftover) {
    prev_leftover = nodes_leftover_h(0);
    if(start_offset > num_chunks-1)
      prev_leftover = UINT32_MAX;
//uint32_t num_leagues = (end_offset-start_offset)/32;
//if(num_leagues*32 < (end_offset-start_offset))
//  num_leagues += 1;
    Kokkos::parallel_for("Insert/compare hashes", Kokkos::RangePolicy<>(start_offset,end_offset), KOKKOS_LAMBDA(const uint32_t i) {
//    Kokkos::TeamPolicy<> team_policy = Kokkos::TeamPolicy<>(num_leagues, 32).set_scratch_size(0, Kokkos::PerThread(16*(2*num_subchunks-1)));
//    Kokkos::parallel_for("Insert/compare hashes", team_policy, KOKKOS_LAMBDA(const Kokkos::TeamPolicy<>::member_type& team_member) {
//      uint32_t i=start_offset+team_member.league_rank()*32+team_member.team_rank();
//if(i < end_offset) {
      uint32_t node = i;
      if(node >= leaf_start) {
        uint32_t num_bytes = chunk_size;
        if(node == num_nodes-1)
          num_bytes = data.size()-(node-leaf_start)*chunk_size;
        hasher.hash(data.data()+((node-leaf_start)*chunk_size), num_bytes, tree(node).digest);

//if(num_subchunks > 1) {
//        HashDigest* hashbuff = (HashDigest*) team_member.thread_scratch(0).get_shmem(sizeof(HashDigest)*((2*num_subchunks)-1));
//        for(int j=0; j<num_subchunks; j++) {
//          uint32_t sz = min_chunk_size;
//          if(num_bytes < min_chunk_size*(j+1))
//            sz = num_bytes - min_chunk_size*j;
//          hasher.hash(data.data()+((node-leaf_start)*chunk_size+j*min_chunk_size), sz, hashbuff[j+num_subchunks-1].digest);
//        }
//        for(int32_t j=num_subchunks-2; j>0; j--) {
//          hasher.hash(hashbuff[2*j+1].digest, 2*sizeof(HashDigest), hashbuff[j].digest);
//        }
//        hasher.hash(hashbuff[1].digest, 2*sizeof(HashDigest), tree(node).digest);
//} else {
//        hasher.hash(data.data()+((node-leaf_start)*chunk_size), num_bytes, tree(node).digest);
//}

        if(tree_id == 0) {
        } else {
          NodeID info = NodeID(node,tree_id);
          uint32_t index = prior_distinct_map.find(tree(node));
          if(!prior_distinct_map.valid_at(index)) { // Chunk not in prior map
            auto result = distinct_map.insert(tree(node), info);
            if(result.success()) { // Chunk is brand new
              tree.distinct_children_d(node) = 2;
#ifdef STATS
              Kokkos::atomic_add(&(num_new(0)), 1);
#endif
              nodes_leftover(0) += static_cast<uint32_t>(1);
            } else if(result.existing()) { // Chunk already exists locally
              NodeID& existing_info = distinct_map.value_at(result.index());
              tree.distinct_children_d(node) = 8;
//              auto res = shared_map.insert(node, existing_info.node);
              shared_map.insert(node, NodeID(existing_info.node, existing_info.tree));
              nodes_leftover(0) += static_cast<uint32_t>(1);
#ifdef STATS
              Kokkos::atomic_add(&num_dupl(0), static_cast<uint32_t>(1));
#endif
            } else if(result.failed()) {
              printf("Failed to insert new chunk into distinct or shared map (tree %u). Shouldn't happen.", tree_id);
            }
          } else { // Chunk already exists
            NodeID old_distinct = prior_distinct_map.value_at(index);
            uint32_t prior_shared_idx = prior_shared_map.find(node);
            if(prior_shared_map.valid_at(prior_shared_idx)) { // Node was repeat last checkpoint 
              NodeID prior_shared = prior_shared_map.value_at(prior_shared_idx);
              if(prior_shared.node == old_distinct.node && prior_shared.tree == old_distinct.tree) {
                tree.distinct_children_d(node) = 0;
#ifdef STATS
                Kokkos::atomic_add(&(num_same(0)), static_cast<uint32_t>(1));
#endif
              } else {
                shared_map.insert(node, NodeID(old_distinct.node, old_distinct.tree));
#ifdef STATS
auto res = table.insert(tree(node));
if(res.success())
  Kokkos::atomic_add(&num_prior_chunks_d(old_distinct.tree), 1);
#endif
                tree.distinct_children_d(node) = 8;
                nodes_leftover(0) += static_cast<uint32_t>(1);
#ifdef STATS
                Kokkos::atomic_add(&(num_shift(0)), static_cast<uint32_t>(1));
#endif
              }
            } else { // Node was distinct last checkpoint
              if(node == old_distinct.node) { // No change since last checkpoint
                tree.distinct_children_d(node) = 0;
#ifdef STATS
                Kokkos::atomic_add(&(num_same(0)), static_cast<uint32_t>(1));
#endif
              } else { // Node changed since last checkpoint
                shared_map.insert(node, NodeID(old_distinct.node, old_distinct.tree));
#ifdef STATS
auto res = table.insert(tree(node));
if(res.success())
  Kokkos::atomic_add(&num_prior_chunks_d(old_distinct.tree), 1);
#endif
                tree.distinct_children_d(node) = 8;
                nodes_leftover(0) += static_cast<uint32_t>(1);
#ifdef STATS
                Kokkos::atomic_add(&(num_shift(0)), static_cast<uint32_t>(1));
#endif
              }
            }
//            if(node != old_distinct.node) { // Chunk exists but at a different offset
//              uint32_t prior_shared_idx = prior_shared_map.find(node);
//              if(prior_shared_map.valid_at(prior_shared_idx)) { // Node is in prior shared map
//                uint32_t prior_node = (prior_shared_map.value_at(prior_shared_idx)).node;
//                if(prior_node != old_distinct.node) { // Chunk has changed since prior checkpoint
////                  auto res = shared_map.insert(node, old_distinct.node);
//                  shared_map.insert(node, NodeID(old_distinct.node, old_distinct.tree));
//                  tree.distinct_children_d(node) = 8;
//                  nodes_leftover(0) += static_cast<uint32_t>(1);
//#ifdef STATS
//                  Kokkos::atomic_add(&(num_shift(0)), static_cast<uint32_t>(1));
//#endif
//                } else { // Chunk hasn't changed since last checkpoint
//#ifdef STATS
//                  Kokkos::atomic_add(&(num_same(0)), static_cast<uint32_t>(1));
//#endif
//                  tree.distinct_children_d(node) = 0;
//                }
//              } else { // Node not in prior shared map
////                auto res = shared_map.insert(node, old_distinct.node);
//                shared_map.insert(node, NodeID(old_distinct.node, old_distinct.tree));
//                tree.distinct_children_d(node) = 8;
//                nodes_leftover(0) += static_cast<uint32_t>(1);
//#ifdef STATS
//                Kokkos::atomic_add(&(num_shift(0)), static_cast<uint32_t>(1));
//#endif
//              }
//            } else { // Chunk exists and hasn't changed node
//#ifdef STATS
//              Kokkos::atomic_add(&(num_same(0)), static_cast<uint32_t>(1));
//#endif
//              tree.distinct_children_d(node) = 0;
//            }
          }
        }
      } else {
        uint32_t child_l = 2*node + 1;
        uint32_t child_r = 2*node + 2;
        tree.distinct_children_d(node) = tree.distinct_children_d(child_l)/2 + tree.distinct_children_d(child_r)/2;
        if(tree.distinct_children_d(node) == 2) {
          hasher.hash((uint8_t*)&tree(2*(node)+1), 2*hasher.digest_size(), (uint8_t*)&tree(node));
distinct_map.insert(tree(node), NodeID(node, tree_id));
          nodes_leftover(0) += static_cast<uint32_t>(1);
        } else if(tree.distinct_children_d(node) == 8) {
          hasher.hash((uint8_t*)&tree(2*(node)+1), 2*hasher.digest_size(), (uint8_t*)&tree(node));
          if(prior_distinct_map.exists(tree(node))) {
            nodes_leftover(0) += static_cast<uint32_t>(1);
          } else {
            uint32_t child_l = 2*(node)+1;
            uint32_t child_r = 2*(node)+2;
            if(prior_distinct_map.exists(tree(child_l))) {
              NodeID info = prior_distinct_map.value_at(prior_distinct_map.find(tree(child_l)));
//              insert_entry(shared_updates, child_l, num_nodes, tree_id, info.node);
              insert_entry(shared_updates, child_l, num_nodes, tree_id, info);
#ifdef STATS
              Kokkos::atomic_add(&(num_comp_s(0)), static_cast<uint32_t>(1));
#endif
            } else if(shared_map.exists(child_l)) {
//              insert_entry(distinct_updates, child_l, num_nodes, tree_id, shared_map.value_at(shared_map.find(child_l)).node);
              insert_entry(distinct_updates, child_l, num_nodes, tree_id, shared_map.value_at(shared_map.find(child_l)));
#ifdef STATS
              Kokkos::atomic_add(&(num_comp_d(0)), static_cast<uint32_t>(1));
#endif
            }
            if(prior_distinct_map.exists(tree(child_r))) {
              NodeID info = prior_distinct_map.value_at(prior_distinct_map.find(tree(child_r)));
//              insert_entry(shared_updates, child_r, num_nodes, tree_id, info.node);
              insert_entry(shared_updates, child_r, num_nodes, tree_id, info);
#ifdef STATS
              Kokkos::atomic_add(&(num_comp_s(0)), static_cast<uint32_t>(1));
#endif
            } else if(shared_map.exists(child_r)) {
//              insert_entry(distinct_updates, child_r, num_nodes, tree_id, shared_map.value_at(shared_map.find(child_r)).node);
              insert_entry(distinct_updates, child_r, num_nodes, tree_id, shared_map.value_at(shared_map.find(child_r)));
#ifdef STATS
              Kokkos::atomic_add(&(num_comp_d(0)), static_cast<uint32_t>(1));
#endif
            }
            tree.distinct_children_d(node) = 0;
          }
        } else if(tree.distinct_children_d(node) == 5) {
          uint32_t child_l = 2*(node)+1;
          uint32_t child_r = 2*(node)+2;
          if(child_l < num_nodes) {
            if((tree.distinct_children_d(child_l) == 2)) {
//              insert_entry(distinct_updates, child_l, num_nodes, tree_id, child_l);
              insert_entry(distinct_updates, child_l, num_nodes, tree_id, NodeID(child_l, tree_id));
#ifdef STATS
              Kokkos::atomic_add(&(num_comp_d(0)), static_cast<uint32_t>(1));
#endif
            } else if((tree.distinct_children_d(child_l) == 8)) {
              if(prior_distinct_map.exists(tree(child_l))) {
                NodeID info = prior_distinct_map.value_at(prior_distinct_map.find(tree(child_l)));
//                insert_entry(shared_updates, child_l, num_nodes, tree_id, info.node);
                insert_entry(shared_updates, child_l, num_nodes, tree_id, info);
#ifdef STATS
              Kokkos::atomic_add(&(num_comp_s(0)), static_cast<uint32_t>(1));
#endif
              } else if(shared_map.exists(child_l)) {
//                uint32_t repeat = shared_map.value_at(shared_map.find(child_l)).node;
//                insert_entry(distinct_updates, child_l, num_nodes, tree_id, repeat);
                auto repeat = shared_map.value_at(shared_map.find(child_l));
                insert_entry(distinct_updates, child_l, num_nodes, tree_id, repeat);
#ifdef STATS
              Kokkos::atomic_add(&(num_comp_d(0)), static_cast<uint32_t>(1));
#endif
              }
            }
          }
          if(child_r < num_nodes) {
            if((tree.distinct_children_d(child_r) == 2)) {
//              insert_entry(distinct_updates, child_r, num_nodes, tree_id, child_r);
              insert_entry(distinct_updates, child_r, num_nodes, tree_id, NodeID(child_r, tree_id));
#ifdef STATS
              Kokkos::atomic_add(&(num_comp_d(0)), static_cast<uint32_t>(1));
#endif
            } else if((tree.distinct_children_d(child_r) == 8)) {
              if(prior_distinct_map.exists(tree(child_r))) {
                NodeID info = prior_distinct_map.value_at(prior_distinct_map.find(tree(child_r)));
//                insert_entry(shared_updates, child_r, num_nodes, tree_id, info.node);
                insert_entry(shared_updates, child_r, num_nodes, tree_id, info);
#ifdef STATS
              Kokkos::atomic_add(&(num_comp_s(0)), static_cast<uint32_t>(1));
#endif
              } else if(shared_map.exists(child_r)) {
//                uint32_t repeat = shared_map.value_at(shared_map.find(child_r)).node;
//                insert_entry(distinct_updates, child_r, num_nodes, tree_id, repeat);
                NodeID repeat = shared_map.value_at(shared_map.find(child_r));
                insert_entry(distinct_updates, child_r, num_nodes, tree_id, repeat);
#ifdef STATS
              Kokkos::atomic_add(&(num_comp_d(0)), static_cast<uint32_t>(1));
#endif
              }
            }
          }
          tree.distinct_children_d(node) = 0;
        } else if(tree.distinct_children_d(node) == 4) {
          uint32_t child_l = 2*(node)+1;
          uint32_t child_r = 2*(node)+2;
          if((child_l < num_nodes) && (tree.distinct_children_d(child_l) == 8)) {
            if(prior_distinct_map.exists(tree(child_l))) {
              NodeID info = prior_distinct_map.value_at(prior_distinct_map.find(tree(child_l)));
//              insert_entry(shared_updates, child_l, num_nodes, tree_id, info.node);
              insert_entry(shared_updates, child_l, num_nodes, tree_id, info);
#ifdef STATS
              Kokkos::atomic_add(&(num_comp_s(0)), static_cast<uint32_t>(1));
#endif
            } else if(shared_map.exists(child_l)) {
//                insert_entry(distinct_updates, child_l, num_nodes, tree_id, shared_map.value_at(shared_map.find(child_l)).node);
                insert_entry(distinct_updates, child_l, num_nodes, tree_id, shared_map.value_at(shared_map.find(child_l)));
#ifdef STATS
              Kokkos::atomic_add(&(num_comp_d(0)), static_cast<uint32_t>(1));
#endif
            }
          } else if((child_r < num_nodes) && (tree.distinct_children_d(child_r) == 8)) {
            if(prior_distinct_map.exists(tree(child_r))) {
              NodeID info = prior_distinct_map.value_at(prior_distinct_map.find(tree(child_r)));
//              insert_entry(shared_updates, child_r, num_nodes, tree_id, info.node);
              insert_entry(shared_updates, child_r, num_nodes, tree_id, info);
#ifdef STATS
              Kokkos::atomic_add(&(num_comp_s(0)), static_cast<uint32_t>(1));
#endif
            } else if(shared_map.exists(child_r)) {
//                insert_entry(distinct_updates, child_r, num_nodes, tree_id, shared_map.value_at(shared_map.find(child_r)).node);
                insert_entry(distinct_updates, child_r, num_nodes, tree_id, shared_map.value_at(shared_map.find(child_r)));
#ifdef STATS
              Kokkos::atomic_add(&(num_comp_d(0)), static_cast<uint32_t>(1));
#endif
            }
          }
          tree.distinct_children_d(node) = 0;
        } else if(tree.distinct_children_d(node) == 1) {
          uint32_t child_l = 2*(node)+1;
          uint32_t child_r = 2*(node)+2;
          if((child_l < num_nodes) && (tree.distinct_children_d(child_l) == 2)) {
//            insert_entry(distinct_updates, child_l, num_nodes, tree_id, child_l);
            insert_entry(distinct_updates, child_l, num_nodes, tree_id, NodeID(child_l, tree_id));
#ifdef STATS
              Kokkos::atomic_add(&(num_comp_d(0)), static_cast<uint32_t>(1));
#endif
          } else if((child_r < num_nodes) && (tree.distinct_children_d(child_r) == 2)) {
//            insert_entry(distinct_updates, child_r, num_nodes, tree_id, child_r);
            insert_entry(distinct_updates, child_r, num_nodes, tree_id, NodeID(child_r, tree_id));
#ifdef STATS
              Kokkos::atomic_add(&(num_comp_d(0)), static_cast<uint32_t>(1));
#endif
          }
          tree.distinct_children_d(node) = 0;
        }
      }
//}
    });
#ifdef STATS
if(start_offset >= leaf_start-(num_chunks/2)) {
Kokkos::fence();
printf("------------------------------\n");
uint32_t n_distinct = 0;
Kokkos::parallel_reduce("Count number of distinct", Kokkos::RangePolicy<>(start_offset, end_offset), KOKKOS_LAMBDA(const uint32_t i, uint32_t& update) {
//  if(tree.distinct_children_d(start_offset+i) == 2) {
  if(tree.distinct_children_d(i) == 2) {
    update += 1;
  }
}, n_distinct);
uint32_t n_same = 0;
Kokkos::fence();
Kokkos::parallel_reduce("Count number of same", Kokkos::RangePolicy<>(start_offset, end_offset), KOKKOS_LAMBDA(const uint32_t i, uint32_t& update) {
  if(tree.distinct_children_d(i) == 0) {
    update += 1;
  }
}, n_same);
uint32_t n_shared = 0;
Kokkos::parallel_reduce("Count number of shared", Kokkos::RangePolicy<>(start_offset, end_offset), KOKKOS_LAMBDA(const uint32_t i, uint32_t& update) {
  if(tree.distinct_children_d(i) == 8) {
    update += 1;
  }
}, n_shared);
uint32_t n_distinct_shared = 0;
Kokkos::parallel_reduce("Count number of distinct shared", Kokkos::RangePolicy<>(start_offset, end_offset), KOKKOS_LAMBDA(const uint32_t i, uint32_t& update) {
  if(tree.distinct_children_d(i) == 5) {
    update += 1;
  }
}, n_distinct_shared);
uint32_t n_distinct_same = 0;
Kokkos::parallel_reduce("Count number of distinct_same", Kokkos::RangePolicy<>(start_offset, end_offset), KOKKOS_LAMBDA(const uint32_t i, uint32_t& update) {
  if(tree.distinct_children_d(i) == 1) {
    update += 1;
  }
}, n_distinct_same);
uint32_t n_shared_same = 0;
Kokkos::parallel_reduce("Count number of shared_same", Kokkos::RangePolicy<>(start_offset, end_offset), KOKKOS_LAMBDA(const uint32_t i, uint32_t& update) {
  if(tree.distinct_children_d(i) == 4) {
    update += 1;
  }
}, n_shared_same);
Kokkos::fence();
printf("Count number of distinct chunks: %u\n", n_distinct);
printf("Count number of same chunks: %u\n", n_same);
printf("Count number of shared chunks: %u\n", n_shared);
printf("Count number of distinct shared chunks: %u\n", n_distinct_shared);
printf("Count number of distinct_same chunks: %u\n", n_distinct_same);
printf("Count number of shared_same chunks: %u\n", n_shared_same);
printf("------------------------------\n");
}
#endif
Kokkos::fence();
    Kokkos::deep_copy(nodes_leftover_h, nodes_leftover);
    current_level -= 1;
    start_offset = (1 << current_level) - 1;
    end_offset = (1 << (current_level+1)) - 1;
  }
  Kokkos::fence();
#ifdef STATS
  Kokkos::deep_copy(num_same_h, num_same);
  Kokkos::deep_copy(num_new_h, num_new);
  Kokkos::deep_copy(num_shift_h, num_shift);
  Kokkos::deep_copy(num_comp_d_h, num_comp_d);
  Kokkos::deep_copy(num_comp_s_h, num_comp_s);
  Kokkos::deep_copy(num_dupl_h, num_dupl);
  Kokkos::deep_copy(num_other_h, num_other);
Kokkos::fence();
  printf("Number of chunks: %u\n", num_chunks);
  printf("Number of new chunks: %u\n", num_new_h(0));
  printf("Number of same chunks: %u\n", num_same_h(0));
  printf("Number of shift chunks: %u\n", num_shift_h(0));
  printf("Number of distinct comp nodes: %u\n", num_comp_d_h(0));
  printf("Number of shared comp nodes: %u\n", num_comp_s_h(0));
  printf("Number of dupl nodes: %u\n", num_dupl_h(0));
  printf("Number of other nodes: %u\n", num_other_h(0));
Kokkos::deep_copy(num_prior_chunks_h, num_prior_chunks_d);
for(int i=0; i<10; i++) {
  printf("Number of chunks repeating from checkpoint %d: %u\n", i, num_prior_chunks_h(i));
}
#endif
}

//template<uint32_t N>
//bool restart( const Kokkos::View<uint8_t*> data,
//              const Kokkos::View<uint8_t*> current, 
//              const Kokkos::View<uint8_t*> restart, 
//              const uint32_t chunk_size, 
//              const uint32_t tree_id, 
//              const CompactTable<N>& distinct, 
//              const CompactTable<N>& shared) {
//  uint64_t data_len = data.size();
//  uint32_t num_chunks = data.size() / chunk_size;
//  if(num_chunks*chunk_size < data.size())
//    num_chunks += 1;
//  uint32_t num_nodes = 2*num_chunks-1;
//  uint32_t leaf_start = num_chunks-1;
//  Kokkos::Bitset<Kokkos::DefaultExecutionSpace> done(num_chunks);
//  done.reset();
//
//  Kokkos::parallel_for("Restart distinct", Kokkos::RangePolicy<>(0, distinct.capacity()), KOKKOS_LAMBDA(const uint32_t i) {
//    if(distinct.valid_at(i)) {
//      Array<N> hist = distinct.value_at(i);
//      for(uint32_t j=0; j<hist.size(); j++) {
//        if(hist(j) == tree_id) {
//          CompactNodeInfo info = distinct.key_at(i);
//          uint32_t start = info.curr_node;
//          uint32_t prev = info.prev_node;
//          for(uint32_t k=start-leaf_start; k<start-leaf_start+prev; k++) {
//            done.set(k);
//          }
//          uint32_t end = ((start-leaf_start)+info.prev_node)*chunk_size;
//          if(end > data.size())
//            end = data.size();
//          for(uint32_t k=(start-leaf_start)*chunk_size; k<end; k++) {
//            restart(k) = current(k);
//          }
//          break;
//        }
//      }
//    }
//  });
//  Kokkos::fence();
//
//  uint32_t num_distinct = done.count();
//  printf("Restarted %u chunks\n", num_distinct);
//
//  Kokkos::parallel_for("Restart shared", Kokkos::RangePolicy<>(0, shared.capacity()), KOKKOS_LAMBDA(const uint32_t i) {
//    if(shared.valid_at(i)) {
//      Array<N> hist = shared.value_at(i);
//      CompactNodeInfo info = shared.key_at(i);
//      for(uint32_t j=0; j<hist.size(); j++) {
//        if(hist(j) == tree_id) {
//          uint32_t start = info.curr_node;
//          for(uint32_t k=start-leaf_start; k<start-leaf_start+info.prev_node; k++) {
////if(done.test(k))
////printf("Overlap %u Shouldn't happen.\n", k);
//            done.set(k);
//          }
//          uint32_t end = ((start-leaf_start)+info.prev_node)*chunk_size;
//          if(end > data.size())
//            end = data.size();
//          for(uint32_t k=(start-leaf_start)*chunk_size; k<end; k++) {
//            restart(k) = current(k);
//          }
//          break;
//        }
//      }
//    }
//  });
//  Kokkos::fence();
//
//  uint32_t num_shared = done.count() - num_distinct;
//  printf("Restarted %u shared chunks\n", num_shared);
//
//  Kokkos::parallel_for("Restart identical", Kokkos::RangePolicy<>(0, num_chunks), KOKKOS_LAMBDA(const uint32_t i) {
//    if(!done.test(i)) {
//      uint32_t size = chunk_size;
//      if(i == num_chunks-1) {
//        size = data.size()-i*chunk_size;
//      } 
//      for(uint32_t j=i*chunk_size; j<i*chunk_size + size; j++) {
////        restart(j) = data(j);
//        restart(j) = current(j);
//      }
//      done.set(i);
//    }
//  });
//  Kokkos::fence();
//  uint32_t num_same = done.count() - num_distinct - num_shared;
//  printf("Restarted %u identical chunks\n", num_same);
//  
//  uint32_t num_diff = 0;
//  Kokkos::parallel_reduce("Check is same", Kokkos::RangePolicy<>(0, data.size()), KOKKOS_LAMBDA(const uint32_t i, uint32_t& update) {
//    if(current(i) != restart(i))
//      update += 1;
//  }, num_diff);
//
//  if(num_diff == 0) {
//    return true;
//  } else {
//printf("Number of differences: %u Bytes out of %u\n", num_diff, data.size());
//    return false;
//  }
//}

void count_distinct_nodes(const MerkleTree& tree, Queue& queue, const uint32_t tree_id, const DistinctMap& distinct) {
  Kokkos::View<uint32_t[1]> n_distinct("Num distinct\n");
  Kokkos::View<uint32_t[1]>::HostMirror n_distinct_h = Kokkos::create_mirror_view(n_distinct);
  Kokkos::deep_copy(n_distinct, 0);
  uint32_t q_size = queue.size();
  while(q_size > 0) {
    Kokkos::parallel_for(q_size, KOKKOS_LAMBDA(const uint32_t entry) {
      uint32_t node = queue.pop();
      HashDigest digest = tree(node);
      if(distinct.exists(digest)) {
        uint32_t existing_id = distinct.find(digest);
        NodeID info = distinct.value_at(existing_id);
        Kokkos::atomic_add(&n_distinct(0), static_cast<uint32_t>(1));
        if(info.node == node && info.tree == tree_id) {
          uint32_t child_l = 2*node+1;
          if(child_l < queue.capacity())
            queue.push(child_l);
          uint32_t child_r = 2*node+2;
          if(child_r < queue.capacity())
            queue.push(child_r);
	}
      } else {
        printf("Node %u digest not in map. This shouldn't happen.\n", node);
      }
    });
    q_size = queue.size();
  }
  Kokkos::deep_copy(n_distinct_h, n_distinct);
  Kokkos::fence();
  printf("Number of distinct nodes: %u out of %lu\n", n_distinct_h(0), tree.tree_d.extent(0));
}

void print_nodes(const MerkleTree& tree, const uint32_t tree_id, const DistinctMap& distinct) {
  Kokkos::View<uint32_t[1]> n_distinct("Num distinct\n");
  Kokkos::View<uint32_t[1]>::HostMirror n_distinct_h = Kokkos::create_mirror_view(n_distinct);
  Kokkos::deep_copy(n_distinct, 0);
  Queue queue(tree.tree_d.extent(0));
  queue.host_push(0);
  uint32_t q_size = queue.size();
  while(q_size > 0) {
    Kokkos::parallel_for(1, KOKKOS_LAMBDA(const uint32_t entry) {
      for(uint32_t i=0; i<q_size; i++) {
        uint32_t node = queue.pop();
        HashDigest digest = tree(node);
        if(distinct.exists(digest)) {
          uint32_t existing_id = distinct.find(digest);
          NodeID info = distinct.value_at(existing_id);
          printf("Distinct Node %u: (%u,%u)\n", node, info.node, info.tree);
          Kokkos::atomic_add(&n_distinct(0), static_cast<uint32_t>(1));
          if(info.node == node && info.tree == tree_id) {
            uint32_t child_l = 2*node+1;
            if(child_l < queue.capacity())
              queue.push(child_l);
            uint32_t child_r = 2*node+2;
            if(child_r < queue.capacity())
              queue.push(child_r);
          }
        } else {
          printf("Node %u digest not in map. This shouldn't happen.\n", node);
        }
      }
    });
    q_size = queue.size();
  }
  Kokkos::deep_copy(n_distinct_h, n_distinct);
  Kokkos::fence();
  printf("Number of distinct nodes: %u out of %lu\n", n_distinct_h(0), tree.tree_d.extent(0));
}

std::pair<uint64_t,uint64_t> write_incr_chkpt_hashtree( const std::string& filename, 
                                const Kokkos::View<uint8_t*>& data, 
                                Kokkos::View<uint8_t*>& buffer_d,
                                uint32_t chunk_size, 
                                const DistinctMap& distinct, 
                                const SharedTreeMap& shared,
                                uint32_t prior_chkpt_id,
                                uint32_t chkpt_id) {
//  std::ofstream file;
//  file.exceptions(std::ofstream::failbit | std::ofstream::badbit);
//  file.open(filename, std::ofstream::out | std::ofstream::binary);

//uint32_t old_chunk_size = chunk_size;
//chunk_size=128;
  uint32_t num_chunks = data.size()/chunk_size;
  if(num_chunks*chunk_size < data.size()) {
    num_chunks += 1;
  }
  uint32_t num_nodes = 2*num_chunks-1;

  // Write whether we are storing the hashes, length full checkpoint, chunk size, number of repeat chunks, number of distinct chunks
//  file << static_cast<uint32_t>(prior_chkpt_id) << static_cast<uint32_t>(chkpt_id) << (static_cast<uint64_t>(data.size()) << static_cast<uint32_t>(chunk_size) << static_cast<uint32_t>(shared.size()) << static_cast<uint32_t>(distinct.size());
  DEBUG_PRINT("Wrote header\n");
  buffer_d = Kokkos::View<uint8_t*>("Buffer", shared.size()*3*sizeof(uint32_t) + distinct.size()*(2*sizeof(uint32_t)+sizeof(HashDigest)+chunk_size));
//  Kokkos::View<uint8_t*> buffer_d("Buffer", shared.size()*2*sizeof(uint32_t) + distinct.size()*(sizeof(uint32_t)+sizeof(HashDigest)+chunk_size));
//  Kokkos::View<uint8_t*>::HostMirror buffer_h = Kokkos::create_mirror_view(buffer_d);
  Kokkos::View<uint64_t[1]> num_bytes_d("Number of bytes written");
  Kokkos::View<uint64_t[1]>::HostMirror num_bytes_h = Kokkos::create_mirror_view(num_bytes_d);
  Kokkos::View<uint64_t[1]> num_bytes_data_d("Number of bytes written for checkpoint data");
  Kokkos::View<uint64_t[1]>::HostMirror num_bytes_data_h = Kokkos::create_mirror_view(num_bytes_data_d);
  Kokkos::View<uint64_t[1]> num_bytes_metadata_d("Number of bytes written for checkpoint metadata");
  Kokkos::View<uint64_t[1]>::HostMirror num_bytes_metadata_h = Kokkos::create_mirror_view(num_bytes_metadata_d);
  Kokkos::deep_copy(num_bytes_d, 0);
  Kokkos::deep_copy(num_bytes_data_d, 0);
  Kokkos::deep_copy(num_bytes_metadata_d, 0);
  DEBUG_PRINT("Setup counters and buffers\n");
  DEBUG_PRINT("Distinct capacity: %u, size: %u\n", distinct.capacity(), distinct.size());
  DEBUG_PRINT("Repeat capacity: %u, size: %u\n", shared.capacity(), shared.size());

  Kokkos::parallel_for("Count shared updates", Kokkos::RangePolicy<>(0, shared.capacity()), KOKKOS_LAMBDA(const uint32_t i) {
    if(shared.valid_at(i)) {
      Kokkos::atomic_add(&num_bytes_metadata_d(0), 3*sizeof(uint32_t));
      uint64_t pos = Kokkos::atomic_fetch_add(&num_bytes_d(0), 3*sizeof(uint32_t));
      uint32_t k = shared.key_at(i);
      NodeID v = shared.value_at(i);
      memcpy(buffer_d.data()+pos, &k, sizeof(uint32_t));
      memcpy(buffer_d.data()+pos+sizeof(uint32_t), &v, 2*sizeof(uint32_t));
//      uint32_t* buffer32 = (uint32_t*)(buffer_d.data()+pos);
//      buffer32[0] = shared.key_at(i);
//      buffer32[1] = shared.value_at(i);
    }
  });
  Kokkos::parallel_for("Count distinct updates", Kokkos::RangePolicy<>(0, distinct.capacity()), KOKKOS_LAMBDA(const uint32_t i) {
    if(distinct.valid_at(i)) {
      auto info = distinct.value_at(i);
      if(info.node >= num_chunks-1) {
        auto digest = distinct.key_at(i);
        Kokkos::atomic_add(&num_bytes_metadata_d(0), 2*sizeof(uint32_t)+sizeof(HashDigest));
        Kokkos::atomic_add(&num_bytes_data_d(0), static_cast<uint64_t>(chunk_size));
        size_t pos = Kokkos::atomic_fetch_add(&num_bytes_d(0), 2*sizeof(uint32_t) + sizeof(HashDigest) + chunk_size);
        memcpy(buffer_d.data()+pos, &info, 2*sizeof(uint32_t));
        memcpy(buffer_d.data() + (pos+2*sizeof(uint32_t)), digest.digest, sizeof(HashDigest));
        uint32_t writesize = chunk_size;
        if(info.node == num_nodes-1) {
          writesize = data.size()-(info.node-num_chunks+1)*chunk_size;
        }
        memcpy(buffer_d.data()+pos+2*sizeof(uint32_t)+sizeof(HashDigest), data.data()+chunk_size*(info.node-num_chunks+1), writesize);
      } else {
        auto digest = distinct.key_at(i);
        Kokkos::atomic_add(&num_bytes_metadata_d(0), 2*sizeof(uint32_t)+sizeof(HashDigest));
        size_t pos = Kokkos::atomic_fetch_add(&num_bytes_d(0), 2*sizeof(uint32_t) + sizeof(HashDigest));
        memcpy(buffer_d.data()+pos, &info, 2*sizeof(uint32_t));
        for(size_t j=0; j<sizeof(HashDigest); j++) {
          buffer_d(pos+2*sizeof(uint32_t)+j) = digest.digest[j];
        }
      }
    }
  });
  Kokkos::fence();
  Kokkos::deep_copy(num_bytes_h, num_bytes_d);
  Kokkos::deep_copy(num_bytes_data_h, num_bytes_data_d);
  Kokkos::deep_copy(num_bytes_metadata_h, num_bytes_metadata_d);
//  Kokkos::deep_copy(buffer_h, buffer_d);
  Kokkos::fence();
  DEBUG_PRINT("Copied data to host\n");
//  file.write((const char*)(buffer_h.data()), num_bytes_h(0));
  STDOUT_PRINT("Number of bytes written for compact incremental checkpoint: %lu\n", 7*sizeof(uint32_t) + num_bytes_h(0));
  STDOUT_PRINT("Number of bytes written for data: %lu\n", num_bytes_data_h(0));
  STDOUT_PRINT("Number of bytes written for metadata: %lu\n", 7*sizeof(uint32_t) + num_bytes_metadata_h(0));
//  file.close();
  return std::make_pair(num_bytes_data_h(0), 7*sizeof(uint32_t) + num_bytes_metadata_h(0));
}

//template<uint32_t N>
std::pair<uint64_t,uint64_t> write_incr_chkpt_hashtree( const std::string& filename, 
                                const Kokkos::View<uint8_t*>& data, 
                                Kokkos::View<uint8_t*>& buffer_d, 
                                uint32_t chunk_size, 
//                                const CompactTable<10>& distinct, 
//                                const CompactTable<10>& shared,
                                const CompactTable& distinct, 
                                const CompactTable& shared,
                                uint32_t prior_chkpt_id,
                                uint32_t chkpt_id) {
//  std::ofstream file;
//  file.exceptions(std::ofstream::failbit | std::ofstream::badbit);
//  file.open(filename, std::ofstream::out | std::ofstream::binary);
  DEBUG_PRINT("File: %s\n", filename.c_str());
  
  uint32_t num_chunks = data.size()/chunk_size;
  if(num_chunks*chunk_size < data.size()) {
    num_chunks += 1;
  }
  uint32_t num_nodes = 2*num_chunks-1;

  // Write whether we are storing the hashes, length full checkpoint, chunk size, number of repeat chunks, number of distinct chunks

  Kokkos::View<uint64_t[1]> num_bytes_d("Number of bytes written");
  Kokkos::View<uint64_t[1]>::HostMirror num_bytes_h = Kokkos::create_mirror_view(num_bytes_d);
  Kokkos::View<uint64_t[1]> num_bytes_data_d("Number of bytes written for checkpoint data");
  Kokkos::View<uint64_t[1]>::HostMirror num_bytes_data_h = Kokkos::create_mirror_view(num_bytes_data_d);
  Kokkos::View<uint64_t[1]> num_bytes_metadata_d("Number of bytes written for checkpoint metadata");
  Kokkos::View<uint64_t[1]>::HostMirror num_bytes_metadata_h = Kokkos::create_mirror_view(num_bytes_metadata_d);
  Kokkos::deep_copy(num_bytes_d, 0);
  Kokkos::deep_copy(num_bytes_data_d, 0);
  Kokkos::deep_copy(num_bytes_metadata_d, 0);
  DEBUG_PRINT("Setup Views\n");

  Kokkos::parallel_for("Count shared bytes", Kokkos::RangePolicy<>(0, shared.capacity()), KOKKOS_LAMBDA(const uint32_t i) {
    if(shared.valid_at(i)) {
      uint32_t node = shared.key_at(i);
      NodeID prev = shared.value_at(i);
      Kokkos::atomic_add(&num_bytes_d(0), sizeof(uint32_t)+sizeof(NodeID));
//      auto entry = shared.key_at(i);
//      auto hist = shared.value_at(i);
//      for(uint32_t j=0; j<hist.size(); j++) {
//        if(hist(j) == chkpt_id) {
//          Kokkos::atomic_add(&num_bytes_d(0), sizeof(uint32_t)*2);
//          break;
//        }
//      }
    }
  });
  DEBUG_PRINT("Wrote shared metadata\n");
  Kokkos::View<uint32_t[1]> max_reg("Max region size");
  Kokkos::View<uint32_t[1]>::HostMirror max_reg_h = Kokkos::create_mirror_view(max_reg);
  max_reg_h(0) = 0;
  Kokkos::deep_copy(max_reg, max_reg_h);
  Kokkos::parallel_for("Count distinct bytes", Kokkos::RangePolicy<>(0, distinct.capacity()), KOKKOS_LAMBDA(const uint32_t i) {
    if(distinct.valid_at(i)) {
      uint32_t node = distinct.key_at(i);
      NodeID prev = distinct.value_at(i);
      if(node == prev.node && chkpt_id == prev.tree) {
        uint32_t size = num_leaf_descendents(node, num_nodes);
        uint32_t start = leftmost_leaf(node, num_nodes) - (num_chunks-1);
        Kokkos::atomic_add(&num_bytes_d(0), sizeof(uint32_t)+sizeof(NodeID) + size*chunk_size);
Kokkos::atomic_max(&max_reg(0), size);
      } else {
        Kokkos::atomic_add(&num_bytes_d(0), sizeof(uint32_t)+sizeof(NodeID));
      }
    }
//    if(distinct.valid_at(i)) {
//      auto entry = distinct.key_at(i);
//      auto hist = distinct.value_at(i);
//      for(uint32_t j=0; j<hist.size(); j++) {
//        if(hist(j) == chkpt_id) {
//          if(entry.curr_node == entry.prev_node) {
//            uint32_t size = num_leaf_descendents(entry.curr_node, num_nodes);
//            uint32_t start = leftmost_leaf(entry.curr_node, num_nodes) - (num_chunks-1);
//            Kokkos::atomic_add(&num_bytes_d(0), sizeof(uint32_t)*2 + size*chunk_size);
//Kokkos::atomic_max(&max_reg(0), size);
//          } else {
//            Kokkos::atomic_add(&num_bytes_d(0), sizeof(uint32_t)*2);
//          }
//          break;
//        }
//      }
//    }
  });

  Kokkos::deep_copy(num_bytes_h, num_bytes_d);
  Kokkos::deep_copy(max_reg_h, max_reg);

  DEBUG_PRINT("Length of buffer: %lu\n", num_bytes_h(0));
  buffer_d = Kokkos::View<uint8_t*>("Buffer", num_bytes_h(0));

  Kokkos::deep_copy(num_bytes_d, 0);
Kokkos::deep_copy(num_bytes_h, num_bytes_d);
size_t start_offset = num_bytes_h(0);

  DEBUG_PRINT("Start writing shared metadata\n");
  Kokkos::parallel_for("Write shared bytes", Kokkos::RangePolicy<>(0, shared.capacity()), KOKKOS_LAMBDA(const uint32_t i) {
    if(shared.valid_at(i)) {
      uint32_t node = shared.key_at(i);
      NodeID prev = shared.value_at(i);
      Kokkos::atomic_add(&num_bytes_metadata_d(0), sizeof(uint32_t)+sizeof(NodeID));
      size_t pos = Kokkos::atomic_fetch_add(&num_bytes_d(0), sizeof(uint32_t)+sizeof(NodeID));
      memcpy(buffer_d.data()+pos, &node, sizeof(uint32_t));
      memcpy(buffer_d.data()+pos+sizeof(uint32_t), &prev, sizeof(NodeID));

//      auto entry = shared.key_at(i);
//      auto hist = shared.value_at(i);
//      for(uint32_t j=0; j<hist.size(); j++) {
//        if(hist(j) == chkpt_id) {
//          Kokkos::atomic_add(&num_bytes_metadata_d(0), sizeof(uint32_t)*2);
//          size_t pos = Kokkos::atomic_fetch_add(&num_bytes_d(0), sizeof(uint32_t)*2);
//          memcpy(buffer_d.data()+pos, &entry.curr_node, sizeof(uint32_t));
//          memcpy(buffer_d.data()+pos+sizeof(uint32_t), &entry.prev_node, sizeof(uint32_t));
//          break;
//        }
//      }
    }
  });
  DEBUG_PRINT("Wrote shared metadata\n");
STDOUT_PRINT("Largest region: %u\n", max_reg_h(0));
Kokkos::deep_copy(num_bytes_h, num_bytes_d);
start_offset = num_bytes_h(0);
std::cout << start_offset << std::endl;
if(max_reg_h(0) < 2048) {
  Kokkos::parallel_for("Write distinct bytes", Kokkos::RangePolicy<>(0, distinct.capacity()), KOKKOS_LAMBDA(const uint32_t i) {
    if(distinct.valid_at(i)) {
      uint32_t node = distinct.key_at(i);
      NodeID prev = distinct.value_at(i);
      if(node == prev.node && chkpt_id == prev.tree) {
//        uint32_t size = num_leaf_descendents(node, num_nodes);
//        uint32_t start = leftmost_leaf(node, num_nodes) - (num_chunks-1);
//        Kokkos::atomic_add(&num_bytes_metadata_d(0), sizeof(uint32_t)+sizeof(NodeID));
//        Kokkos::atomic_add(&num_bytes_data_d(0), static_cast<uint64_t>(size*chunk_size));
//        size_t pos = Kokkos::atomic_fetch_add(&num_bytes_d(0), sizeof(uint32_t)+sizeof(NodeID) + size*chunk_size);
//if(pos == start_offset)
//printf("Pos: %lu, node: %u, node ID: (%u,%u)\n", pos, node, prev.node, prev.tree);
//if(node == prev.node && chkpt_id == prev.tree)
//printf("Distinct node: %u: (%u,%u)\n", node, prev.node, prev.tree);
//        memcpy(buffer_d.data()+pos, &node, sizeof(uint32_t));
//        memcpy(buffer_d.data()+pos+sizeof(uint32_t), &prev, sizeof(NodeID));
//        uint32_t writesize = chunk_size*size;
//        if(start*chunk_size+writesize > data.size())
//          writesize = data.size()-start*chunk_size;
//        memcpy(buffer_d.data()+pos+sizeof(uint32_t)+sizeof(NodeID), data.data()+start*chunk_size, writesize);
      } else {
        Kokkos::atomic_add(&num_bytes_metadata_d(0), sizeof(uint32_t)+sizeof(NodeID));
        size_t pos = Kokkos::atomic_fetch_add(&num_bytes_d(0), sizeof(uint32_t)+sizeof(NodeID));
        memcpy(buffer_d.data()+pos, &node, sizeof(uint32_t));
        memcpy(buffer_d.data()+pos+sizeof(uint32_t), &prev, sizeof(NodeID));
      }

//      auto entry = distinct.key_at(i);
//      auto hist = distinct.value_at(i);
//      for(uint32_t j=0; j<hist.size(); j++) {
//        if(hist(j) == chkpt_id) {
//          if(entry.curr_node == entry.prev_node) {
//            uint32_t size = num_leaf_descendents(entry.curr_node, num_nodes);
//            uint32_t start = leftmost_leaf(entry.curr_node, num_nodes) - (num_chunks-1);
//            Kokkos::atomic_add(&num_bytes_metadata_d(0), sizeof(uint32_t)*2);
//            Kokkos::atomic_add(&num_bytes_data_d(0), static_cast<uint64_t>(size*chunk_size));
//            size_t pos = Kokkos::atomic_fetch_add(&num_bytes_d(0), sizeof(uint32_t)*2 + size*chunk_size);
//            memcpy(buffer_d.data()+pos, &entry.curr_node, sizeof(uint32_t));
//            memcpy(buffer_d.data()+pos+sizeof(uint32_t), &entry.prev_node, sizeof(uint32_t));
//            uint32_t writesize = chunk_size*size;
//            if(start*chunk_size+writesize > data.size())
//              writesize = data.size()-start*chunk_size;
//            memcpy(buffer_d.data()+pos+2*sizeof(uint32_t), data.data()+start*chunk_size, writesize);
//          } else {
//            Kokkos::atomic_add(&num_bytes_metadata_d(0), sizeof(uint32_t)*2);
//            size_t pos = Kokkos::atomic_fetch_add(&num_bytes_d(0), sizeof(uint32_t)*2);
//            memcpy(buffer_d.data()+pos, &entry.curr_node, sizeof(uint32_t));
//            memcpy(buffer_d.data()+pos+sizeof(uint32_t), &entry.prev_node, sizeof(uint32_t));
//          }
//          break;
//        }
//      }
    }
  });
  Kokkos::parallel_for("Write distinct bytes", Kokkos::RangePolicy<>(0, distinct.capacity()), KOKKOS_LAMBDA(const uint32_t i) {
    if(distinct.valid_at(i)) {
      uint32_t node = distinct.key_at(i);
      NodeID prev = distinct.value_at(i);
      if(node == prev.node && chkpt_id == prev.tree) {
        uint32_t size = num_leaf_descendents(node, num_nodes);
        uint32_t start = leftmost_leaf(node, num_nodes) - (num_chunks-1);
        Kokkos::atomic_add(&num_bytes_metadata_d(0), sizeof(uint32_t)+sizeof(NodeID));
        Kokkos::atomic_add(&num_bytes_data_d(0), static_cast<uint64_t>(size*chunk_size));
        size_t pos = Kokkos::atomic_fetch_add(&num_bytes_d(0), sizeof(uint32_t)+sizeof(NodeID) + size*chunk_size);
        memcpy(buffer_d.data()+pos, &node, sizeof(uint32_t));
        memcpy(buffer_d.data()+pos+sizeof(uint32_t), &prev, sizeof(NodeID));
        uint32_t writesize = chunk_size*size;
        if(start*chunk_size+writesize > data.size())
          writesize = data.size()-start*chunk_size;
        memcpy(buffer_d.data()+pos+sizeof(uint32_t)+sizeof(NodeID), data.data()+start*chunk_size, writesize);
      } else {
//        Kokkos::atomic_add(&num_bytes_metadata_d(0), sizeof(uint32_t)+sizeof(NodeID));
//        size_t pos = Kokkos::atomic_fetch_add(&num_bytes_d(0), sizeof(uint32_t)+sizeof(NodeID));
//        memcpy(buffer_d.data()+pos, &node, sizeof(uint32_t));
//        memcpy(buffer_d.data()+pos+sizeof(uint32_t), &prev, sizeof(NodeID));
      }

//      auto entry = distinct.key_at(i);
//      auto hist = distinct.value_at(i);
//      for(uint32_t j=0; j<hist.size(); j++) {
//        if(hist(j) == chkpt_id) {
//          if(entry.curr_node == entry.prev_node) {
//            uint32_t size = num_leaf_descendents(entry.curr_node, num_nodes);
//            uint32_t start = leftmost_leaf(entry.curr_node, num_nodes) - (num_chunks-1);
//            Kokkos::atomic_add(&num_bytes_metadata_d(0), sizeof(uint32_t)*2);
//            Kokkos::atomic_add(&num_bytes_data_d(0), static_cast<uint64_t>(size*chunk_size));
//            size_t pos = Kokkos::atomic_fetch_add(&num_bytes_d(0), sizeof(uint32_t)*2 + size*chunk_size);
//            memcpy(buffer_d.data()+pos, &entry.curr_node, sizeof(uint32_t));
//            memcpy(buffer_d.data()+pos+sizeof(uint32_t), &entry.prev_node, sizeof(uint32_t));
//            uint32_t writesize = chunk_size*size;
//            if(start*chunk_size+writesize > data.size())
//              writesize = data.size()-start*chunk_size;
//            memcpy(buffer_d.data()+pos+2*sizeof(uint32_t), data.data()+start*chunk_size, writesize);
//          } else {
//            Kokkos::atomic_add(&num_bytes_metadata_d(0), sizeof(uint32_t)*2);
//            size_t pos = Kokkos::atomic_fetch_add(&num_bytes_d(0), sizeof(uint32_t)*2);
//            memcpy(buffer_d.data()+pos, &entry.curr_node, sizeof(uint32_t));
//            memcpy(buffer_d.data()+pos+sizeof(uint32_t), &entry.prev_node, sizeof(uint32_t));
//          }
//          break;
//        }
//      }
    }
  });
} else {
  Kokkos::parallel_for("Write distinct bytes", Kokkos::TeamPolicy<>(distinct.capacity(), Kokkos::AUTO) , 
                       KOKKOS_LAMBDA(const Kokkos::TeamPolicy<>::member_type& team_member) {
//    uint32_t i=team_member.league_rank()*team_member.team_size()+team_member.team_rank();
    uint32_t i=team_member.league_rank();
    if(distinct.valid_at(i)) {
      uint32_t node = distinct.key_at(i);
      NodeID prev = distinct.value_at(i);
      if(node == prev.node && chkpt_id == prev.tree) {
        uint32_t size = num_leaf_descendents(node, num_nodes);
        uint32_t start = leftmost_leaf(node, num_nodes) - (num_chunks-1);
        size_t pos;
        if(team_member.team_rank() == 0) {
          Kokkos::atomic_add(&num_bytes_metadata_d(0), sizeof(uint32_t)+sizeof(NodeID));
          Kokkos::atomic_add(&num_bytes_data_d(0), static_cast<uint64_t>(size*chunk_size));
          pos = Kokkos::atomic_fetch_add(&num_bytes_d(0), sizeof(uint32_t) + sizeof(NodeID) + size*chunk_size);
          memcpy(buffer_d.data()+pos, &node, sizeof(uint32_t));
          memcpy(buffer_d.data()+pos+sizeof(uint32_t), &prev, sizeof(NodeID));
        }
        team_member.team_barrier();
        team_member.team_broadcast(pos, 0);
        uint32_t writesize = chunk_size*size;
        if(start*chunk_size+writesize > data.size())
          writesize = data.size()-start*chunk_size;
        Kokkos::parallel_for(Kokkos::TeamThreadRange(team_member, writesize), [&] (const uint64_t& j) {
          buffer_d(pos+sizeof(uint32_t)+sizeof(NodeID)+j) = data(start*chunk_size+j);
        });
      } else {
        if(team_member.team_rank() == 0) {
          Kokkos::atomic_add(&num_bytes_metadata_d(0), sizeof(uint32_t)+sizeof(NodeID));
          size_t pos = Kokkos::atomic_fetch_add(&num_bytes_d(0), sizeof(uint32_t)+sizeof(NodeID));
          memcpy(buffer_d.data()+pos, &node, sizeof(uint32_t));
          memcpy(buffer_d.data()+pos+sizeof(uint32_t), &prev, sizeof(NodeID));
        }
      }
//      auto entry = distinct.key_at(i);
//      auto hist = distinct.value_at(i);
//      for(uint32_t j=0; j<hist.size(); j++) {
//        if(hist(j) == chkpt_id) {
//          if(entry.curr_node == entry.prev_node) {
//            uint32_t size = num_leaf_descendents(entry.curr_node, num_nodes);
//            uint32_t start = leftmost_leaf(entry.curr_node, num_nodes) - (num_chunks-1);
//            size_t pos;
//            if(team_member.team_rank() == 0) {
//              Kokkos::atomic_add(&num_bytes_metadata_d(0), sizeof(uint32_t)*2);
//              Kokkos::atomic_add(&num_bytes_data_d(0), static_cast<uint64_t>(size*chunk_size));
//              pos = Kokkos::atomic_fetch_add(&num_bytes_d(0), sizeof(uint32_t)*2 + size*chunk_size);
//              memcpy(buffer_d.data()+pos, &entry.curr_node, sizeof(uint32_t));
//              memcpy(buffer_d.data()+pos+sizeof(uint32_t), &entry.prev_node, sizeof(uint32_t));
//            }
//            team_member.team_barrier();
//            team_member.team_broadcast(pos, 0);
//            uint32_t writesize = chunk_size*size;
//            if(start*chunk_size+writesize > data.size())
//              writesize = data.size()-start*chunk_size;
//            Kokkos::parallel_for(Kokkos::TeamThreadRange(team_member, writesize), [&](const uint32_t& j) {
//              buffer_d(pos+2*sizeof(uint32_t)+j) = data(start*chunk_size+j);
//            });
////            memcpy(buffer_d.data()+pos+2*sizeof(uint32_t), data.data()+start*chunk_size, writesize);
//          } else {
//            if(team_member.team_rank() == 0) {
//              Kokkos::atomic_add(&num_bytes_metadata_d(0), sizeof(uint32_t)*2);
//              size_t pos = Kokkos::atomic_fetch_add(&num_bytes_d(0), sizeof(uint32_t)*2);
//              memcpy(buffer_d.data()+pos, &entry.curr_node, sizeof(uint32_t));
//              memcpy(buffer_d.data()+pos+sizeof(uint32_t), &entry.prev_node, sizeof(uint32_t));
//            }
//          }
//          break;
//        }
//      }
    }
  });
}
  Kokkos::fence();
  DEBUG_PRINT("Finished collecting data\n");
  Kokkos::deep_copy(num_bytes_h, num_bytes_d);
  Kokkos::deep_copy(num_bytes_data_h, num_bytes_data_d);
  Kokkos::deep_copy(num_bytes_metadata_h, num_bytes_metadata_d);
//  Kokkos::deep_copy(buffer_h, buffer_d);
  Kokkos::fence();
//  file.write((const char*)(buffer_h.data()), num_bytes_h(0));
  STDOUT_PRINT("Number of bytes written for compact incremental checkpoint: %lu\n", 7*sizeof(uint32_t) + num_bytes_h(0));
  STDOUT_PRINT("Number of bytes written for data: %lu\n", num_bytes_data_h(0));
  STDOUT_PRINT("Number of bytes written for metadata: %lu\n", 7*sizeof(uint32_t) + num_bytes_metadata_h(0));
//  file.close();
  DEBUG_PRINT("Closed file\n");
  return std::make_pair(num_bytes_data_h(0), 7*sizeof(uint32_t) + num_bytes_metadata_h(0));
}

#endif // KOKKOS_MERKLE_TREE_HPP
