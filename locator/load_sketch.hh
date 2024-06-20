/*
 * Copyright (C) 2023-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#pragma once

#include "locator/topology.hh"
#include "locator/token_metadata.hh"
#include "locator/tablets.hh"
#include "utils/stall_free.hh"
#include "utils/extremum_tracking.hh"
#include "utils/div_ceil.hh"

#include <seastar/core/smp.hh>
#include <seastar/coroutine/maybe_yield.hh>

#include <optional>
#include <vector>

namespace locator {

/// A data structure which keeps track of load associated with data ownership
/// on shards of the whole cluster.
class load_sketch {
    using shard_id = seastar::shard_id;
    struct shard_load {
        shard_id id;
        size_t load; // In tablets.
    };
    // Used in a max-heap to yield lower load first.
    struct shard_load_cmp {
        bool operator()(const shard_load& a, const shard_load& b) const {
            return a.load > b.load;
        }
    };
    struct node_load {
        std::vector<shard_load> _shards;
        uint64_t _load = 0; // In tablets.

        node_load(size_t shard_count) : _shards(shard_count) {
            shard_id next_shard = 0;
            for (auto&& s : _shards) {
                s.id = next_shard++;
                s.load = 0;
            }
        }

        uint64_t& load() noexcept {
            return _load;
        }
        const uint64_t& load() const noexcept {
            return _load;
        }
    };
    std::unordered_map<host_id, node_load> _nodes;
    token_metadata_ptr _tm;
private:
    tablet_replica_set get_replicas_for_tablet_load(const tablet_info& ti, const tablet_transition_info* trinfo) const {
        // We reflect migrations in the load as if they already happened,
        // optimistically assuming that they will succeed.
        return trinfo ? trinfo->next : ti.replicas;
    }

    future<> populate_table(const tablet_map& tmap, std::optional<host_id> host) {
        const topology& topo = _tm->get_topology();
        co_await tmap.for_each_tablet([&] (tablet_id tid, const tablet_info& ti) -> future<> {
            for (auto&& replica : get_replicas_for_tablet_load(ti, tmap.get_tablet_transition_info(tid))) {
                if (host && *host != replica.host) {
                    continue;
                }
                if (!_nodes.contains(replica.host)) {
                    _nodes.emplace(replica.host, node_load{topo.find_node(replica.host)->get_shard_count()});
                }
                node_load& n = _nodes.at(replica.host);
                if (replica.shard < n._shards.size()) {
                    n.load() += 1;
                    n._shards[replica.shard].load += 1;
                }
            }
            return make_ready_future<>();
        });
    }
public:
    load_sketch(token_metadata_ptr tm)
        : _tm(std::move(tm)) {
    }

    future<> populate(std::optional<host_id> host = std::nullopt, std::optional<table_id> only_table = std::nullopt) {
        co_await utils::clear_gently(_nodes);

        if (only_table) {
            auto& tmap = _tm->tablets().get_tablet_map(*only_table);
            co_await populate_table(tmap, host);
        } else {
            for (auto&& [table, tmap]: _tm->tablets().all_tables()) {
                co_await populate_table(tmap, host);
            }
        }

        for (auto&& n : _nodes) {
            std::make_heap(n.second._shards.begin(), n.second._shards.end(), shard_load_cmp());
        }
    }

    shard_id next_shard(host_id node) {
        const topology& topo = _tm->get_topology();
        if (!_nodes.contains(node)) {
            auto shard_count = topo.find_node(node)->get_shard_count();
            if (shard_count == 0) {
                throw std::runtime_error(format("Shard count not known for node {}", node));
            }
            _nodes.emplace(node, node_load{shard_count});
        }
        auto& n = _nodes.at(node);
        std::pop_heap(n._shards.begin(), n._shards.end(), shard_load_cmp());
        shard_load& s = n._shards.back();
        auto shard = s.id;
        s.load += 1;
        n.load() += 1;
        std::push_heap(n._shards.begin(), n._shards.end(), shard_load_cmp());
        return shard;
    }

    void unload(host_id node, shard_id shard) {
        auto& n = _nodes.at(node);
        for (auto& shard_load : n._shards) {
            if (shard_load.id == shard) {
                assert(shard_load.load > 0);
                --shard_load.load;
                break;
            }
        }
        std::make_heap(n._shards.begin(), n._shards.end(), shard_load_cmp());
    }

    void pick(host_id node, shard_id shard) {
        auto& n = _nodes.at(node);
        for (auto& shard_load : n._shards) {
            if (shard_load.id == shard) {
                ++shard_load.load;
                break;
            }
        }
        std::make_heap(n._shards.begin(), n._shards.end(), shard_load_cmp());
    }

    uint64_t get_load(host_id node) const {
        if (!_nodes.contains(node)) {
            return 0;
        }
        return _nodes.at(node).load();
    }

    uint64_t total_load() const {
        uint64_t total = 0;
        for (auto&& n : _nodes) {
            total += n.second.load();
        }
        return total;
    }

    uint64_t get_avg_shard_load(host_id node) const {
        if (!_nodes.contains(node)) {
            return 0;
        }
        auto& n = _nodes.at(node);
        return div_ceil(n.load(), n._shards.size());
    }

    double get_real_avg_shard_load(host_id node) const {
        if (!_nodes.contains(node)) {
            return 0;
        }
        auto& n = _nodes.at(node);
        return double(n.load()) / n._shards.size();
    }

    shard_id get_shard_count(host_id node) const {
        if (!_nodes.contains(node)) {
            return 0;
        }
        return _nodes.at(node)._shards.size();
    }

    // Returns the difference in tablet count between highest-loaded shard and lowest-loaded shard.
    // Returns 0 when shards are perfectly balanced.
    // Returns 1 when shards are imbalanced, but it's not possible to balance them.
    uint64_t get_shard_imbalance(host_id node) const {
        auto minmax = get_shard_minmax(node);
        return minmax.max() - minmax.max();
    }

    min_max_tracker<uint64_t> get_shard_minmax(host_id node) const {
        min_max_tracker<uint64_t> minmax;
        if (_nodes.contains(node)) {
            auto& n = _nodes.at(node);
            for (auto&& s: n._shards) {
                minmax.update(s.load);
            }
        } else {
            minmax.update(0);
        }
        return minmax;
    }
};

} // namespace locator
