/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
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
 *
 * Modified by ScyllaDB
 * Copyright (C) 2015 ScyllaDB
 */

/*
 * This file is part of Scylla.
 *
 * Scylla is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Scylla is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Scylla.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "gms/inet_address.hh"
#include "gms/endpoint_state.hh"
#include "gms/gossip_digest.hh"
#include "gms/gossip_digest_syn.hh"
#include "gms/gossip_digest_ack.hh"
#include "gms/gossip_digest_ack2.hh"
#include "gms/versioned_value.hh"
#include "gms/gossiper.hh"
#include "gms/application_state.hh"
#include "gms/failure_detector.hh"
#include "gms/i_failure_detection_event_listener.hh"
#include "gms/i_endpoint_state_change_subscriber.hh"
#include "gms/i_failure_detector.hh"
#include "service/storage_service.hh"
#include "message/messaging_service.hh"
#include "dht/i_partitioner.hh"
#include "log.hh"
#include <seastar/core/sleep.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/metrics.hh>
#include <seastar/util/defer.hh>
#include <chrono>
#include "dht/i_partitioner.hh"
#include <boost/range/algorithm/set_algorithm.hpp>
#include <boost/range/adaptors.hpp>

namespace gms {

using clk = gossiper::clk;

static logging::logger logger("gossip");

constexpr std::chrono::milliseconds gossiper::INTERVAL;
constexpr std::chrono::hours gossiper::A_VERY_LONG_TIME;
constexpr int64_t gossiper::MAX_GENERATION_DIFFERENCE;

distributed<gossiper> _the_gossiper;

netw::msg_addr gossiper::get_msg_addr(inet_address to) {
    return msg_addr{to, _default_cpuid};
}

sstring gossiper::get_cluster_name() {
    return _cluster_name;
}

void gossiper::set_cluster_name(sstring name) {
    _cluster_name = name;
}

sstring gossiper::get_partitioner_name() {
    return dht::global_partitioner().name();
}

std::set<inet_address> gossiper::get_seeds() {
    return _seeds_from_config;
}

void gossiper::set_seeds(std::set<inet_address> _seeds) {
    _seeds_from_config = _seeds;
}

std::chrono::milliseconds gossiper::quarantine_delay() {
    auto& ss = service::get_local_storage_service();
    return ss.get_ring_delay() * 2;
}

auto& storage_service_value_factory() {
    return service::get_local_storage_service().value_factory;
}

class feature_enabler : public i_endpoint_state_change_subscriber {
    gossiper& _g;
public:
    feature_enabler(gossiper& g) : _g(g) {}
    void on_join(inet_address ep, endpoint_state state) override {
        _g.maybe_enable_features();
    }
    void on_change(inet_address ep, application_state state, const versioned_value&) override {
        if (state == application_state::SUPPORTED_FEATURES) {
            _g.maybe_enable_features();
        }
    }
    void before_change(inet_address, endpoint_state, application_state, const versioned_value&) override { }
    void on_alive(inet_address, endpoint_state) override {}
    void on_dead(inet_address, endpoint_state) override {}
    void on_remove(inet_address) override {}
    void on_restart(inet_address, endpoint_state) override {}
};

gossiper::gossiper() {
    // Gossiper's stuff below runs only on CPU0
    if (engine().cpu_id() != 0) {
        return;
    }

    _scheduled_gossip_task.set_callback([this] { run(); });
    // half of QUARATINE_DELAY, to ensure _just_removed_endpoints has enough leeway to prevent re-gossip
    fat_client_timeout = quarantine_delay() / 2;
    /* register with the Failure Detector for receiving Failure detector events */
    get_local_failure_detector().register_failure_detection_event_listener(this);
    register_(make_shared<feature_enabler>(*this));
    // Register this instance with JMX
    namespace sm = seastar::metrics;
    auto ep = get_broadcast_address();
    _metrics.add_group("gossip", {
        sm::make_derive("heart_beat",
            [ep, this] {
                auto es = get_endpoint_state_for_endpoint_ptr(ep);
                if (es) {
                    return es->get_heart_beat_state().get_heart_beat_version();
                } else {
                    return 0;
                }
            }, sm::description("Heartbeat of the current Node.")),
    });
}

void gossiper::set_last_processed_message_at() {
    set_last_processed_message_at(now());
}

void gossiper::set_last_processed_message_at(clk::time_point tp) {
    _last_processed_message_at = tp;
}

/*
 * First construct a map whose key is the endpoint in the GossipDigest and the value is the
 * GossipDigest itself. Then build a list of version differences i.e difference between the
 * version in the GossipDigest and the version in the local state for a given InetAddress.
 * Sort this list. Now loop through the sorted list and retrieve the GossipDigest corresponding
 * to the endpoint from the map that was initially constructed.
*/
void gossiper::do_sort(std::vector<gossip_digest>& g_digest_list) {
    /* Construct a map of endpoint to GossipDigest. */
    std::map<inet_address, gossip_digest> ep_to_digest_map;
    for (auto g_digest : g_digest_list) {
        ep_to_digest_map.emplace(g_digest.get_endpoint(), g_digest);
    }

    /*
     * These digests have their maxVersion set to the difference of the version
     * of the local EndpointState and the version found in the GossipDigest.
    */
    std::vector<gossip_digest> diff_digests;
    for (auto g_digest : g_digest_list) {
        auto ep = g_digest.get_endpoint();
        auto* ep_state = this->get_endpoint_state_for_endpoint_ptr(ep);
        int version = ep_state ? this->get_max_endpoint_state_version(*ep_state) : 0;
        int diff_version = ::abs(version - g_digest.get_max_version());
        diff_digests.emplace_back(gossip_digest(ep, g_digest.get_generation(), diff_version));
    }

    g_digest_list.clear();
    std::sort(diff_digests.begin(), diff_digests.end());
    int size = diff_digests.size();
    /*
     * Report the digests in descending order. This takes care of the endpoints
     * that are far behind w.r.t this local endpoint
    */
    for (int i = size - 1; i >= 0; --i) {
        g_digest_list.emplace_back(ep_to_digest_map[diff_digests[i].get_endpoint()]);
    }
}

// Depends on
// - no external dependency
future<> gossiper::handle_syn_msg(msg_addr from, gossip_digest_syn syn_msg) {
    logger.trace("handle_syn_msg():from={},cluster_name:peer={},local={},partitioner_name:peer={},local={}",
        from, syn_msg.cluster_id(), get_cluster_name(), syn_msg.partioner(), get_partitioner_name());
    this->set_last_processed_message_at();
    if (!this->is_enabled()) {
        return make_ready_future<>();
    }

    /* If the message is from a different cluster throw it away. */
    if (syn_msg.cluster_id() != get_cluster_name()) {
        logger.warn("ClusterName mismatch from {} {}!={}", from.addr, syn_msg.cluster_id(), get_cluster_name());
        return make_ready_future<>();
    }

    if (syn_msg.partioner() != "" && syn_msg.partioner() != get_partitioner_name()) {
        logger.warn("Partitioner mismatch from {} {}!={}", from.addr, syn_msg.partioner(), get_partitioner_name());
        return make_ready_future<>();
    }

    auto g_digest_list = syn_msg.get_gossip_digests();
    do_sort(g_digest_list);
    std::vector<gossip_digest> delta_gossip_digest_list;
    std::map<inet_address, endpoint_state> delta_ep_state_map;
    this->examine_gossiper(g_digest_list, delta_gossip_digest_list, delta_ep_state_map);
    gms::gossip_digest_ack ack_msg(std::move(delta_gossip_digest_list), std::move(delta_ep_state_map));
    logger.trace("handle_syn_msg(): response={}", ack_msg);
    return this->ms().send_gossip_digest_ack(from, std::move(ack_msg));
}

class gossiper::msg_proc_guard {
public:
    msg_proc_guard(gossiper& g)
                    : _ptr(&(++g._msg_processing), [](uint64_t * p) { if (p) { --(*p); } })
    {}
    msg_proc_guard(msg_proc_guard&& m) = default;
private:
    std::unique_ptr<uint64_t, void(*)(uint64_t*)> _ptr;
};

// Depends on
// - failure_detector
// - on_change callbacks, e.g., storage_service -> access db system_table
// - on_restart callbacks
// - on_join callbacks
// - on_alive
future<> gossiper::handle_ack_msg(msg_addr id, gossip_digest_ack ack_msg) {
    logger.trace("handle_ack_msg():from={},msg={}", id, ack_msg);

    this->set_last_processed_message_at();
    if (!this->is_enabled() && !this->is_in_shadow_round()) {
        return make_ready_future<>();
    }

    msg_proc_guard mp(*this);

    auto g_digest_list = ack_msg.get_gossip_digest_list();
    auto& ep_state_map = ack_msg.get_endpoint_state_map();

    auto f = make_ready_future<>();
    if (ep_state_map.size() > 0) {
        /* Notify the Failure Detector */
        this->notify_failure_detector(ep_state_map);
        f = this->apply_state_locally(std::move(ep_state_map));
    }

    assert(_msg_processing > 0);

    return f.then([id, g_digest_list = std::move(g_digest_list), mp = std::move(mp), this] {
        if (this->is_in_shadow_round()) {
            this->finish_shadow_round();
            // don't bother doing anything else, we have what we came for
            return make_ready_future<>();
        }
        /* Get the state required to send to this gossipee - construct GossipDigestAck2Message */
        std::map<inet_address, endpoint_state> delta_ep_state_map;
        for (auto g_digest : g_digest_list) {
            inet_address addr = g_digest.get_endpoint();
            auto local_ep_state_ptr = this->get_state_for_version_bigger_than(addr, g_digest.get_max_version());
            if (local_ep_state_ptr) {
                delta_ep_state_map.emplace(addr, *local_ep_state_ptr);
            }
        }
        gms::gossip_digest_ack2 ack2_msg(std::move(delta_ep_state_map));
        logger.trace("Sending a GossipDigestACK2 to {}", id);
        return this->ms().send_gossip_digest_ack2(id, std::move(ack2_msg)).handle_exception([id] (auto ep) {
            logger.warn("Fail to send GossipDigestACK2 to {}: {}", id, ep);
        });
    });
}

// Depends on
// - failure_detector
// - on_change callbacks, e.g., storage_service -> access db system_table
// - on_restart callbacks
// - on_join callbacks
// - on_alive callbacks
future<> gossiper::handle_ack2_msg(gossip_digest_ack2 msg) {
    logger.trace("handle_ack2_msg():msg={}", msg);
    set_last_processed_message_at();
    if (!is_enabled()) {
        return make_ready_future<>();
    }

    msg_proc_guard mp(*this);

    auto& remote_ep_state_map = msg.get_endpoint_state_map();
    /* Notify the Failure Detector */
    notify_failure_detector(remote_ep_state_map);
    return apply_state_locally(std::move(remote_ep_state_map)).finally([mp = std::move(mp)] {});
}

future<> gossiper::handle_echo_msg() {
    set_last_processed_message_at();
    return make_ready_future<>();
}

future<> gossiper::handle_shutdown_msg(inet_address from) {
    set_last_processed_message_at();
    if (!is_enabled()) {
        logger.debug("Ignoring shutdown message from {} because gossip is disabled", from);
        return make_ready_future<>();
    }
    return seastar::async([this, from] {
        this->mark_as_shutdown(from);
    });
}

void gossiper::init_messaging_service_handler(bind_messaging_port do_bind) {
    if (_ms_registered) {
        return;
    }
    _ms_registered = true;
    ms().register_gossip_digest_syn([] (const rpc::client_info& cinfo, gossip_digest_syn syn_msg) {
        auto from = netw::messaging_service::get_source(cinfo);
        smp::submit_to(0, [from, syn_msg = std::move(syn_msg)] () mutable {
            auto& gossiper = gms::get_local_gossiper();
            return gossiper.handle_syn_msg(from, std::move(syn_msg));
        }).handle_exception([] (auto ep) {
            logger.warn("Fail to handle GOSSIP_DIGEST_SYN: {}", ep);
        });
        return messaging_service::no_wait();
    });
    ms().register_gossip_digest_ack([] (const rpc::client_info& cinfo, gossip_digest_ack msg) {
        auto from = netw::messaging_service::get_source(cinfo);
        smp::submit_to(0, [from, msg = std::move(msg)] () mutable {
            auto& gossiper = gms::get_local_gossiper();
            return gossiper.handle_ack_msg(from, std::move(msg));
        }).handle_exception([] (auto ep) {
            logger.warn("Fail to handle GOSSIP_DIGEST_ACK: {}", ep);
        });
        return messaging_service::no_wait();
    });
    ms().register_gossip_digest_ack2([] (gossip_digest_ack2 msg) {
        smp::submit_to(0, [msg = std::move(msg)] () mutable {
            return gms::get_local_gossiper().handle_ack2_msg(std::move(msg));
        }).handle_exception([] (auto ep) {
            logger.warn("Fail to handle GOSSIP_DIGEST_ACK2: {}", ep);
        });
        return messaging_service::no_wait();
    });
    ms().register_gossip_echo([] {
        return smp::submit_to(0, [] {
            return gms::get_local_gossiper().handle_echo_msg();
        });
    });
    ms().register_gossip_shutdown([] (inet_address from) {
        smp::submit_to(0, [from] {
            return gms::get_local_gossiper().handle_shutdown_msg(from);
        }).handle_exception([] (auto ep) {
            logger.warn("Fail to handle GOSSIP_SHUTDOWN: {}", ep);
        });
        return messaging_service::no_wait();
    });

    // Start listening messaging_service after gossip message handlers are registered
    if (do_bind) {
        ms().start_listen();
    }
}

void gossiper::uninit_messaging_service_handler() {
    auto& ms = netw::get_local_messaging_service();
    ms.unregister_gossip_echo();
    ms.unregister_gossip_shutdown();
    ms.unregister_gossip_digest_syn();
    ms.unregister_gossip_digest_ack();
    ms.unregister_gossip_digest_ack2();
    _ms_registered = false;
}

future<> gossiper::send_gossip(gossip_digest_syn message, std::set<inet_address> epset) {
    std::vector<inet_address> __live_endpoints(epset.begin(), epset.end());
    size_t size = __live_endpoints.size();
    if (size < 1) {
        return make_ready_future<>();
    }
    /* Generate a random number from 0 -> size */
    std::uniform_int_distribution<int> dist(0, size - 1);
    int index = dist(_random);
    inet_address to = __live_endpoints[index];
    auto id = get_msg_addr(to);
    logger.trace("Sending a GossipDigestSyn to {} ...", id);
    _gossiped_to_seed = _seeds.count(to);
    return ms().send_gossip_digest_syn(id, std::move(message)).handle_exception([id] (auto ep) {
        // It is normal to reach here because it is normal that a node
        // tries to send a SYN message to a peer node which is down before
        // failure_detector thinks that peer node is down.
        logger.trace("Fail to send GossipDigestSyn to {}: {}", id, ep);
    });
}


void gossiper::notify_failure_detector(inet_address endpoint, const endpoint_state& remote_endpoint_state) {
    /*
     * If the local endpoint state exists then report to the FD only
     * if the versions workout.
    */
    auto* local_endpoint_state = get_endpoint_state_for_endpoint_ptr(endpoint);
    if (local_endpoint_state) {
        i_failure_detector& fd = get_local_failure_detector();
        int local_generation = local_endpoint_state->get_heart_beat_state().get_generation();
        int remote_generation = remote_endpoint_state.get_heart_beat_state().get_generation();
        if (remote_generation > local_generation) {
            local_endpoint_state->update_timestamp();
            // this node was dead and the generation changed, this indicates a reboot, or possibly a takeover
            // we will clean the fd intervals for it and relearn them
            if (!local_endpoint_state->is_alive()) {
                logger.debug("Clearing interval times for {} due to generation change", endpoint);
                fd.remove(endpoint);
            }
            fd.report(endpoint);
            return;
        }

        if (remote_generation == local_generation) {
            int local_version = get_max_endpoint_state_version(*local_endpoint_state);
            int remote_version = remote_endpoint_state.get_heart_beat_state().get_heart_beat_version();
            if (remote_version > local_version) {
                local_endpoint_state->update_timestamp();
                // just a version change, report to the fd
                fd.report(endpoint);
            }
        }
    }
}

future<> gossiper::apply_state_locally(std::map<inet_address, endpoint_state> map) {
    auto start = std::chrono::steady_clock::now();
    auto endpoints = boost::copy_range<std::vector<inet_address>>(map | boost::adaptors::map_keys);
    std::shuffle(endpoints.begin(), endpoints.end(), _random_engine);
    auto node_is_seed = [this] (gms::inet_address ip) { return is_seed(ip); };
    boost::partition(endpoints, node_is_seed);
    logger.debug("apply_state_locally_endpoints={}", endpoints);

    return do_with(std::move(endpoints), std::move(map), [this] (auto&& endpoints, auto&& map) {
        return parallel_for_each(endpoints, [this, &map] (auto&& ep) -> future<> {
            if (ep == this->get_broadcast_address() && !this->is_in_shadow_round()) {
                return make_ready_future<>();
            }
            if (_just_removed_endpoints.count(ep)) {
                logger.trace("Ignoring gossip for {} because it is quarantined", ep);
                return make_ready_future<>();
            }
          return seastar::with_semaphore(_apply_state_locally_semaphore, 1, [this, &ep, &map] () mutable {
            return seastar::async([this, &ep, &map] () mutable {
                /*
                   If state does not exist just add it. If it does then add it if the remote generation is greater.
                   If there is a generation tie, attempt to break it by heartbeat version.
                   */
                auto permit = this->lock_endpoint(ep).get0();
                const endpoint_state& remote_state = map[ep];
                auto es = this->get_endpoint_state_for_endpoint_ptr(ep);
                if (es) {
                    endpoint_state& local_ep_state_ptr = *es;
                    int local_generation = local_ep_state_ptr.get_heart_beat_state().get_generation();
                    int remote_generation = remote_state.get_heart_beat_state().get_generation();
                    logger.trace("{} local generation {}, remote generation {}", ep, local_generation, remote_generation);
                    // A node was removed with nodetool removenode can have a generation of 2
                    if (local_generation > 2 && remote_generation > local_generation + MAX_GENERATION_DIFFERENCE) {
                        // assume some peer has corrupted memory and is broadcasting an unbelievable generation about another peer (or itself)
                        logger.warn("received an invalid gossip generation for peer {}; local generation = {}, received generation = {}",
                            ep, local_generation, remote_generation);
                    } else if (remote_generation > local_generation) {
                        logger.trace("Updating heartbeat state generation to {} from {} for {}", remote_generation, local_generation, ep);
                        // major state change will handle the update by inserting the remote state directly
                        this->handle_major_state_change(ep, remote_state);
                    } else if (remote_generation == local_generation) {  //generation has not changed, apply new states
                        /* find maximum state */
                        int local_max_version = this->get_max_endpoint_state_version(local_ep_state_ptr);
                        int remote_max_version = this->get_max_endpoint_state_version(remote_state);
                        if (remote_max_version > local_max_version) {
                            // apply states, but do not notify since there is no major change
                            this->apply_new_states(ep, local_ep_state_ptr, remote_state);
                        } else {
                            logger.trace("Ignoring remote version {} <= {} for {}", remote_max_version, local_max_version, ep);
                        }
                        if (!local_ep_state_ptr.is_alive() && !this->is_dead_state(local_ep_state_ptr)) { // unless of course, it was dead
                            this->mark_alive(ep, local_ep_state_ptr);
                        }
                    } else {
                        logger.trace("Ignoring remote generation {} < {}", remote_generation, local_generation);
                    }
                } else {
                    // this is a new node, report it to the FD in case it is the first time we are seeing it AND it's not alive
                    get_local_failure_detector().report(ep);
                    this->handle_major_state_change(ep, remote_state);
                }
            });
          });
        });
    }).then([start] {
            logger.debug("apply_state_locally() took {} ms", std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count());
    });
}

// Runs inside seastar::async context
void gossiper::remove_endpoint(inet_address endpoint) {
    // do subscribers first so anything in the subscriber that depends on gossiper state won't get confused
    // We can not run on_remove callbacks here becasue on_remove in
    // storage_service might take the gossiper::timer_callback_lock
    seastar::async([this, endpoint] {
        _subscribers.for_each([endpoint] (auto& subscriber) {
            subscriber->on_remove(endpoint);
        });
    }).handle_exception([] (auto ep) {
        logger.warn("Fail to call on_remove callback: {}", ep);
    });

    if(_seeds.count(endpoint)) {
        build_seeds_list();
        _seeds.erase(endpoint);
        logger.info("removed {} from _seeds, updated _seeds list = {}", endpoint, _seeds);
    }

    _live_endpoints.erase(std::remove(_live_endpoints.begin(), _live_endpoints.end(), endpoint), _live_endpoints.end());
    _live_endpoints_just_added.remove(endpoint);
    _unreachable_endpoints.erase(endpoint);
    quarantine_endpoint(endpoint);
    logger.debug("removing endpoint {}", endpoint);
}

// Runs inside seastar::async context
void gossiper::do_status_check() {
    logger.trace("Performing status check ...");

    auto now = this->now();

    auto fd = get_local_failure_detector().shared_from_this();
    for (auto it = endpoint_state_map.begin(); it != endpoint_state_map.end();) {
        auto endpoint = it->first;
        auto& ep_state = it->second;
        it++;

        bool is_alive = ep_state.is_alive();
        if (endpoint == get_broadcast_address()) {
            continue;
        }

        fd->interpret(endpoint);

        // check if this is a fat client. fat clients are removed automatically from
        // gossip after FatClientTimeout.  Do not remove dead states here.
        if (is_gossip_only_member(endpoint)
            && !_just_removed_endpoints.count(endpoint)
            && ((now - ep_state.get_update_timestamp()) > fat_client_timeout)) {
            logger.info("FatClient {} has been silent for {}ms, removing from gossip", endpoint, fat_client_timeout.count());
            remove_endpoint(endpoint); // will put it in _just_removed_endpoints to respect quarantine delay
            evict_from_membership(endpoint); // can get rid of the state immediately
        }

        // check for dead state removal
        auto expire_time = get_expire_time_for_endpoint(endpoint);
        if (!is_alive && (now > expire_time)
             && (!service::get_local_storage_service().get_token_metadata().is_member(endpoint))) {
            logger.debug("time is expiring for endpoint : {} ({})", endpoint, expire_time.time_since_epoch().count());
            evict_from_membership(endpoint);
        }
    }

    for (auto it = _just_removed_endpoints.begin(); it != _just_removed_endpoints.end();) {
        auto& t= it->second;
        if ((now - t) > quarantine_delay()) {
            logger.debug("{} ms elapsed, {} gossip quarantine over", quarantine_delay().count(), it->first);
            it = _just_removed_endpoints.erase(it);
        } else {
            it++;
        }
    }
}

future<gossiper::endpoint_permit> gossiper::lock_endpoint(inet_address ep) {
    return endpoint_locks.get_or_load(ep, [] (const inet_address& ep) { return semaphore(1); }).then([] (auto eptr) {
        return get_units(*eptr, 1).then([eptr] (auto units) mutable {
            return endpoint_permit{std::move(eptr), std::move(units)};
        });
    });
}

// Depends on:
// - failure_detector
// - on_remove callbacks, e.g, storage_service -> access token_metadata
void gossiper::run() {
    timer_callback_lock().then([this, g = this->shared_from_this()] {
        return seastar::async([this, g] {
            logger.trace("=== Gossip round START");

            //wait on messaging service to start listening
            // MessagingService.instance().waitUntilListening();

            /* Update the local heartbeat counter. */
            auto br_addr = get_broadcast_address();
            heart_beat_state& hbs = endpoint_state_map[br_addr].get_heart_beat_state();
            hbs.update_heart_beat();

            logger.trace("My heartbeat is now {}", endpoint_state_map[br_addr].get_heart_beat_state().get_heart_beat_version());
            std::vector<gossip_digest> g_digests;
            this->make_random_gossip_digest(g_digests);

            if (g_digests.size() > 0) {
                gossip_digest_syn message(get_cluster_name(), get_partitioner_name(), g_digests);

                _gossiped_to_seed = false;

                auto get_random_node = [this] (const std::vector<inet_address>& nodes) {
                    std::uniform_int_distribution<int> dist(0, nodes.size() - 1);
                    int index = dist(this->_random);
                    return nodes[index];
                };

                /* Gossip to some random live members */
                // TODO: For now, we choose 10th of all the nodes in the cluster.
                auto nr_live_nodes = std::max(size_t(1), endpoint_state_map.size() / 10);
                nr_live_nodes = std::min(nr_live_nodes, _live_endpoints.size());
                std::unordered_set<gms::inet_address> live_nodes;
                logger.debug("nr_live_nodes={}, endpoint_state_map.size()={}, live_endpoints.size={}",
                    nr_live_nodes, endpoint_state_map.size(), _live_endpoints.size());
                while (live_nodes.size() < nr_live_nodes && nr_live_nodes <= _live_endpoints.size()) {
                    if (!_live_endpoints_just_added.empty()) {
                        auto ep = _live_endpoints_just_added.front();
                        _live_endpoints_just_added.pop_front();
                        logger.info("Favor newly added node {}", ep);
                        live_nodes.insert(ep);
                    } else {
                        // Get a random live node
                        live_nodes.insert(get_random_node(_live_endpoints));
                    }
                }
                logger.debug("Talk to {} live nodes: {}", nr_live_nodes, live_nodes);
                for (auto& ep: live_nodes) {
                    do_gossip_to_live_member(message, ep).handle_exception([] (auto ep) {
                        logger.trace("Failed to do_gossip_to_live_member: {}", ep);
                    });
                }

                /* Gossip to some unreachable member with some probability to check if he is back up */
                do_gossip_to_unreachable_member(message).handle_exception([] (auto ep) {
                    logger.trace("Faill to do_gossip_to_unreachable_member: {}", ep);
                });

                /* Gossip to a seed if we did not do so above, or we have seen less nodes
                   than there are seeds.  This prevents partitions where each group of nodes
                   is only gossiping to a subset of the seeds.

                   The most straightforward check would be to check that all the seeds have been
                   verified either as live or unreachable.  To avoid that computation each round,
                   we reason that:

                   either all the live nodes are seeds, in which case non-seeds that come online
                   will introduce themselves to a member of the ring by definition,

                   or there is at least one non-seed node in the list, in which case eventually
                   someone will gossip to it, and then do a gossip to a random seed from the
                   gossipedToSeed check.

                   See CASSANDRA-150 for more exposition. */
                logger.trace("gossiped_to_seed={}, _live_endpoints.size={}, _seeds.size={}",
                             _gossiped_to_seed, _live_endpoints.size(), _seeds.size());
                if (!_gossiped_to_seed || _live_endpoints.size() < _seeds.size()) {
                    do_gossip_to_seed(message).handle_exception([] (auto ep) {
                        logger.trace("Faill to do_gossip_to_seed: {}", ep);
                    });
                }

                do_status_check();
            }

            //
            // Gossiper task runs only on CPU0:
            //
            //    - If endpoint_state_map or _live_endpoints have changed - duplicate
            //      them across all other shards.
            //    - Reschedule the gossiper only after execution on all nodes is done.
            //
            bool live_endpoint_changed = (_live_endpoints != _shadow_live_endpoints);
            bool unreachable_endpoint_changed = (_unreachable_endpoints != _shadow_unreachable_endpoints);

            if (live_endpoint_changed || unreachable_endpoint_changed) {
                if (live_endpoint_changed) {
                    _shadow_live_endpoints = _live_endpoints;
                }

                if (unreachable_endpoint_changed) {
                    _shadow_unreachable_endpoints = _unreachable_endpoints;
                }

                _the_gossiper.invoke_on_all([this, live_endpoint_changed, unreachable_endpoint_changed, es = endpoint_state_map] (gossiper& local_gossiper) {
                    // Don't copy gossiper(CPU0) maps into themselves!
                    if (engine().cpu_id() != 0) {
                        if (live_endpoint_changed) {
                            local_gossiper._live_endpoints = _shadow_live_endpoints;
                        }

                        if (unreachable_endpoint_changed) {
                            local_gossiper._unreachable_endpoints = _shadow_unreachable_endpoints;
                        }

                        for (auto&& e : es) {
                            local_gossiper.endpoint_state_map[e.first].set_alive(e.second.is_alive());
                        }
                    }
                }).get();
            }
        });
    }).then_wrapped([this] (auto&& f) {
        try {
            f.get();
            _nr_run++;
            logger.trace("=== Gossip round OK");
        } catch (...) {
            logger.trace("=== Gossip round FAIL");
        }

        if (logger.is_enabled(logging::log_level::trace)) {
            for (auto& x : endpoint_state_map) {
                logger.trace("ep={}, eps={}", x.first, x.second);
            }
        }
        if (_enabled) {
            _scheduled_gossip_task.arm(INTERVAL);
        }
        this->timer_callback_unlock();
    });
}

bool gossiper::seen_any_seed() {
    return std::any_of(endpoint_state_map.begin(), endpoint_state_map.end(), [this] (auto& entry) {
        if (_seeds.count(entry.first)) {
            return true;
        }
        auto* internal_ip = entry.second.get_application_state_ptr(application_state::INTERNAL_IP);
        return internal_ip && _seeds.count(inet_address(internal_ip->value));
    });
}

bool gossiper::is_seed(const gms::inet_address& endpoint) const {
    return _seeds.count(endpoint);
}

void gossiper::register_(shared_ptr<i_endpoint_state_change_subscriber> subscriber) {
    _subscribers.push_back(subscriber);
}

void gossiper::unregister_(shared_ptr<i_endpoint_state_change_subscriber> subscriber) {
    _subscribers.remove(subscriber);
}

std::set<inet_address> gossiper::get_live_members() {
    std::set<inet_address> live_members(_live_endpoints.begin(), _live_endpoints.end());
    auto myip = get_broadcast_address();
    logger.debug("live_members before={}", live_members);
    live_members.insert(myip);
    if (is_shutdown(myip)) {
        live_members.erase(myip);
    }
    logger.debug("live_members after={}", live_members);
    return live_members;
}

std::set<inet_address> gossiper::get_live_token_owners() {
    std::set<inet_address> token_owners;
    for (auto& member : get_live_members()) {
        auto es = get_endpoint_state_for_endpoint_ptr(member);
        if (es && !is_dead_state(*es) && service::get_local_storage_service().get_token_metadata().is_member(member)) {
            token_owners.insert(member);
        }
    }
    return token_owners;
}

std::set<inet_address> gossiper::get_unreachable_token_owners() {
    std::set<inet_address> token_owners;
    for (auto&& x : _unreachable_endpoints) {
        auto& endpoint = x.first;
        if (service::get_local_storage_service().get_token_metadata().is_member(endpoint)) {
            token_owners.insert(endpoint);
        }
    }
    return token_owners;
}

// Return downtime in microseconds
int64_t gossiper::get_endpoint_downtime(inet_address ep) {
    auto it = _unreachable_endpoints.find(ep);
    if (it != _unreachable_endpoints.end()) {
        auto& downtime = it->second;
        return std::chrono::duration_cast<std::chrono::microseconds>(now() - downtime).count();
    } else {
        return 0L;
    }
}

// Depends on
// - on_dead callbacks
// It is called from failure_detector
//
// Runs inside seastar::async context
void gossiper::convict(inet_address endpoint, double phi) {
    auto* state = get_endpoint_state_for_endpoint_ptr(endpoint);
    if (!state || !state->is_alive()) {
        return;
    }

    if (logger.is_enabled(log_level::debug)) {
        logger.debug("Convicting ep={} with status={}", endpoint, get_gossip_status(*state));
        logger.trace("    phi={}, is_dead_state={}", phi, is_dead_state(*state));
    }
    if (is_shutdown(endpoint)) {
        mark_as_shutdown(endpoint);
    } else {
        mark_dead(endpoint, *state);
    }
}

std::set<inet_address> gossiper::get_unreachable_members() {
    std::set<inet_address> ret;
    for (auto&& x : _unreachable_endpoints) {
        ret.insert(x.first);
    }
    return ret;
}

int gossiper::get_max_endpoint_state_version(endpoint_state state) {
    int max_version = state.get_heart_beat_state().get_heart_beat_version();
    for (auto& entry : state.get_application_state_map()) {
        auto& value = entry.second;
        max_version = std::max(max_version, value.version);
    }
    return max_version;
}

// Runs inside seastar::async context
void gossiper::evict_from_membership(inet_address endpoint) {
    auto permit = lock_endpoint(endpoint).get0();
    _unreachable_endpoints.erase(endpoint);
    container().invoke_on_all([endpoint] (auto& g) {
        g.endpoint_state_map.erase(endpoint);
    }).get();
    _expire_time_endpoint_map.erase(endpoint);
    get_local_failure_detector().remove(endpoint);
    quarantine_endpoint(endpoint);
    logger.debug("evicting {} from gossip", endpoint);
}

void gossiper::quarantine_endpoint(inet_address endpoint) {
    quarantine_endpoint(endpoint, now());
}

void gossiper::quarantine_endpoint(inet_address endpoint, clk::time_point quarantine_expiration) {
    _just_removed_endpoints[endpoint] = quarantine_expiration;
}

void gossiper::replacement_quarantine(inet_address endpoint) {
    // remember, quarantine_endpoint will effectively already add QUARANTINE_DELAY, so this is 2x
    // logger.debug("");
    quarantine_endpoint(endpoint, now() + quarantine_delay());
}

// Runs inside seastar::async context
void gossiper::replaced_endpoint(inet_address endpoint) {
    remove_endpoint(endpoint);
    evict_from_membership(endpoint);
    replacement_quarantine(endpoint);
}

void gossiper::make_random_gossip_digest(std::vector<gossip_digest>& g_digests) {
    int generation = 0;
    int max_version = 0;

    // local epstate will be part of endpoint_state_map
    std::vector<inet_address> endpoints;
    for (auto&& x : endpoint_state_map) {
        endpoints.push_back(x.first);
    }
    std::shuffle(endpoints.begin(), endpoints.end(), _random_engine);
    for (auto& endpoint : endpoints) {
        auto es = get_endpoint_state_for_endpoint_ptr(endpoint);
        if (es) {
            auto& eps = *es;
            generation = eps.get_heart_beat_state().get_generation();
            max_version = get_max_endpoint_state_version(eps);
        }
        g_digests.push_back(gossip_digest(endpoint, generation, max_version));
    }
#if 0
    if (logger.isTraceEnabled()) {
        StringBuilder sb = new StringBuilder();
        for (GossipDigest g_digest : g_digests)
        {
            sb.append(g_digest);
            sb.append(" ");
        }
        logger.trace("Gossip Digests are : {}", sb);
    }
#endif
}

future<> gossiper::replicate(inet_address ep, const endpoint_state& es) {
    return container().invoke_on_all([ep, es, orig = engine().cpu_id(), self = shared_from_this()] (gossiper& g) {
        if (engine().cpu_id() != orig) {
            g.endpoint_state_map[ep].apply_application_state(es);
        }
    });
}

future<> gossiper::replicate(inet_address ep, const std::map<application_state, versioned_value>& src, const std::vector<application_state>& changed) {
    return container().invoke_on_all([ep, &src, &changed, orig = engine().cpu_id(), self = shared_from_this()] (gossiper& g) {
        if (engine().cpu_id() != orig) {
            for (auto&& key : changed) {
                g.endpoint_state_map[ep].apply_application_state(key, src.at(key));
            }
        }
    });
}

future<> gossiper::replicate(inet_address ep, application_state key, const versioned_value& value) {
    return container().invoke_on_all([ep, key, &value, orig = engine().cpu_id(), self = shared_from_this()] (gossiper& g) {
        if (engine().cpu_id() != orig) {
            g.endpoint_state_map[ep].apply_application_state(key, value);
        }
    });
}

future<> gossiper::advertise_removing(inet_address endpoint, utils::UUID host_id, utils::UUID local_host_id) {
    return seastar::async([this, g = this->shared_from_this(), endpoint, host_id, local_host_id] {
        auto& state = get_endpoint_state(endpoint);
        // remember this node's generation
        int generation = state.get_heart_beat_state().get_generation();
        logger.info("Removing host: {}", host_id);
        auto ring_delay = service::get_local_storage_service().get_ring_delay();
        logger.info("Sleeping for {}ms to ensure {} does not change", ring_delay.count(), endpoint);
        sleep(ring_delay).get();
        // make sure it did not change
        auto& eps = get_endpoint_state(endpoint);
        if (eps.get_heart_beat_state().get_generation() != generation) {
            throw std::runtime_error(sprint("Endpoint %s generation changed while trying to remove it", endpoint));
        }

        // update the other node's generation to mimic it as if it had changed it itself
        logger.info("Advertising removal for {}", endpoint);
        eps.update_timestamp(); // make sure we don't evict it too soon
        eps.get_heart_beat_state().force_newer_generation_unsafe();
        eps.add_application_state(application_state::STATUS, storage_service_value_factory().removing_nonlocal(host_id));
        eps.add_application_state(application_state::REMOVAL_COORDINATOR, storage_service_value_factory().removal_coordinator(local_host_id));
        endpoint_state_map[endpoint] = eps;
        replicate(endpoint, eps).get();
    });
}

future<> gossiper::advertise_token_removed(inet_address endpoint, utils::UUID host_id) {
    return seastar::async([this, g = this->shared_from_this(), endpoint, host_id] {
        auto& eps = get_endpoint_state(endpoint);
        eps.update_timestamp(); // make sure we don't evict it too soon
        eps.get_heart_beat_state().force_newer_generation_unsafe();
        auto expire_time = compute_expire_time();
        eps.add_application_state(application_state::STATUS, storage_service_value_factory().removed_nonlocal(host_id, expire_time.time_since_epoch().count()));
        logger.info("Completing removal of {}", endpoint);
        add_expire_time_for_endpoint(endpoint, expire_time);
        endpoint_state_map[endpoint] = eps;
        replicate(endpoint, eps).get();
        // ensure at least one gossip round occurs before returning
        sleep(INTERVAL * 2).get();
    });
}

future<> gossiper::unsafe_assassinate_endpoint(sstring address) {
    logger.warn("Gossiper.unsafeAssassinateEndpoint is deprecated and will be removed in the next release; use assassinate_endpoint instead");
    return assassinate_endpoint(address);
}

future<> gossiper::assassinate_endpoint(sstring address) {
    return get_gossiper().invoke_on(0, [address] (auto&& gossiper) {
        return seastar::async([&gossiper, g = gossiper.shared_from_this(), address] {
            inet_address endpoint(address);
            auto permit = gossiper.lock_endpoint(endpoint).get0();
            auto es = gossiper.get_endpoint_state_for_endpoint_ptr(endpoint);
            auto now = gossiper.now();
            int gen = std::chrono::duration_cast<std::chrono::seconds>((now + std::chrono::seconds(60)).time_since_epoch()).count();
            int ver = 9999;
            endpoint_state ep_state = es ? *es : endpoint_state(heart_beat_state(gen, ver));
            std::vector<dht::token> tokens;
            logger.warn("Assassinating {} via gossip", endpoint);
            if (es) {
                auto& ss = service::get_local_storage_service();
                auto tokens = ss.get_token_metadata().get_tokens(endpoint);
                if (tokens.empty()) {
                    logger.warn("Unable to calculate tokens for {}.  Will use a random one", address);
                    throw std::runtime_error(sprint("Unable to calculate tokens for %s", endpoint));
                }

                int generation = ep_state.get_heart_beat_state().get_generation();
                int heartbeat = ep_state.get_heart_beat_state().get_heart_beat_version();
                logger.info("Sleeping for {} ms to ensure {} does not change", ss.get_ring_delay().count(), endpoint);
                // make sure it did not change
                sleep(ss.get_ring_delay()).get();

                es = gossiper.get_endpoint_state_for_endpoint_ptr(endpoint);
                if (!es) {
                    logger.warn("Endpoint {} disappeared while trying to assassinate, continuing anyway", endpoint);
                } else {
                    auto& new_state = *es;
                    if (new_state.get_heart_beat_state().get_generation() != generation) {
                        throw std::runtime_error(sprint("Endpoint still alive: %s generation changed while trying to assassinate it", endpoint));
                    } else if (new_state.get_heart_beat_state().get_heart_beat_version() != heartbeat) {
                        throw std::runtime_error(sprint("Endpoint still alive: %s heartbeat changed while trying to assassinate it", endpoint));
                    }
                }
                ep_state.update_timestamp(); // make sure we don't evict it too soon
                ep_state.get_heart_beat_state().force_newer_generation_unsafe();
            }

            // do not pass go, do not collect 200 dollars, just gtfo
            std::unordered_set<dht::token> tokens_set(tokens.begin(), tokens.end());
            auto expire_time = gossiper.compute_expire_time();
            ep_state.add_application_state(application_state::STATUS, storage_service_value_factory().left(tokens_set, expire_time.time_since_epoch().count()));
            gossiper.handle_major_state_change(endpoint, ep_state);
            sleep(INTERVAL * 4).get();
            logger.warn("Finished assassinating {}", endpoint);
        });
    });
}

bool gossiper::is_known_endpoint(inet_address endpoint) {
    return endpoint_state_map.count(endpoint);
}

future<int> gossiper::get_current_generation_number(inet_address endpoint) {
    return get_gossiper().invoke_on(0, [endpoint] (auto&& gossiper) {
        return gossiper.get_endpoint_state(endpoint).get_heart_beat_state().get_generation();
    });
}

future<int> gossiper::get_current_heart_beat_version(inet_address endpoint) {
    return get_gossiper().invoke_on(0, [endpoint] (auto&& gossiper) {
        return gossiper.get_endpoint_state(endpoint).get_heart_beat_state().get_heart_beat_version();
    });
}

future<> gossiper::do_gossip_to_live_member(gossip_digest_syn message, gms::inet_address ep) {
    return send_gossip(message, {ep});
}

future<> gossiper::do_gossip_to_unreachable_member(gossip_digest_syn message) {
    double live_endpoint_count = _live_endpoints.size();
    double unreachable_endpoint_count = _unreachable_endpoints.size();
    if (unreachable_endpoint_count > 0) {
        /* based on some probability */
        double prob = unreachable_endpoint_count / (live_endpoint_count + 1);
        std::uniform_real_distribution<double> dist(0, 1);
        double rand_dbl = dist(_random);
        if (rand_dbl < prob) {
            std::set<inet_address> addrs;
            for (auto&& x : _unreachable_endpoints) {
                // Ignore the node which is decommissioned
                if (get_gossip_status(x.first) != sstring(versioned_value::STATUS_LEFT)) {
                    addrs.insert(x.first);
                }
            }
            logger.trace("do_gossip_to_unreachable_member: live_endpoint nr={} unreachable_endpoints nr={}",
                live_endpoint_count, unreachable_endpoint_count);
            return send_gossip(message, addrs);
        }
    }
    return make_ready_future<>();
}

future<> gossiper::do_gossip_to_seed(gossip_digest_syn prod) {
    size_t size = _seeds.size();
    if (size > 0) {
        if (size == 1 && _seeds.count(get_broadcast_address())) {
            return make_ready_future<>();
        }

        if (_live_endpoints.size() == 0) {
            logger.trace("do_gossip_to_seed: live_endpoints nr={}, seeds nr={}", 0, _seeds.size());
            return send_gossip(prod, _seeds);
        } else {
            /* Gossip with the seed with some probability. */
            double probability = _seeds.size() / (double) (_live_endpoints.size() + _unreachable_endpoints.size());
            std::uniform_real_distribution<double> dist(0, 1);
            double rand_dbl = dist(_random);
            if (rand_dbl <= probability) {
                logger.trace("do_gossip_to_seed: live_endpoints nr={}, seeds nr={}", _live_endpoints.size(), _seeds.size());
                return send_gossip(prod, _seeds);
            }
        }
    }
    return make_ready_future<>();
}

bool gossiper::is_gossip_only_member(inet_address endpoint) {
    auto es = get_endpoint_state_for_endpoint_ptr(endpoint);
    if (!es) {
        return false;
    }
    auto& ss = service::get_local_storage_service();
    return !is_dead_state(*es) && !ss.get_token_metadata().is_member(endpoint);
}

clk::time_point gossiper::get_expire_time_for_endpoint(inet_address endpoint) {
    /* default expire_time is A_VERY_LONG_TIME */
    auto it = _expire_time_endpoint_map.find(endpoint);
    if (it == _expire_time_endpoint_map.end()) {
        return compute_expire_time();
    } else {
        auto stored_time = it->second;
        return stored_time;
    }
}

const endpoint_state* gossiper::get_endpoint_state_for_endpoint_ptr(inet_address ep) const {
    auto it = endpoint_state_map.find(ep);
    if (it == endpoint_state_map.end()) {
        return nullptr;
    } else {
        return &it->second;
    }
}

endpoint_state* gossiper::get_endpoint_state_for_endpoint_ptr(inet_address ep) {
    auto it = endpoint_state_map.find(ep);
    if (it == endpoint_state_map.end()) {
        return nullptr;
    } else {
        return &it->second;
    }
}

endpoint_state& gossiper::get_endpoint_state(inet_address ep) {
    auto ptr = get_endpoint_state_for_endpoint_ptr(ep);
    if (!ptr) {
        throw std::out_of_range(sprint("ep=%s", ep));
    }
    return *ptr;
}

stdx::optional<endpoint_state> gossiper::get_endpoint_state_for_endpoint(inet_address ep) const {
    auto it = endpoint_state_map.find(ep);
    if (it == endpoint_state_map.end()) {
        return {};
    } else {
        return it->second;
    }
}

void gossiper::reset_endpoint_state_map() {
    endpoint_state_map.clear();
    _unreachable_endpoints.clear();
    _live_endpoints.clear();
    _live_endpoints_just_added.clear();
}

std::unordered_map<inet_address, endpoint_state>& gms::gossiper::get_endpoint_states() {
    return endpoint_state_map;
}

bool gossiper::uses_host_id(inet_address endpoint) {
    return netw::get_local_messaging_service().knows_version(endpoint) ||
            get_application_state_ptr(endpoint, application_state::NET_VERSION);
}

utils::UUID gossiper::get_host_id(inet_address endpoint) {
    if (!uses_host_id(endpoint)) {
        throw std::runtime_error(sprint("Host %s does not use new-style tokens!", endpoint));
    }
    auto app_state = get_application_state_ptr(endpoint, application_state::HOST_ID);
    if (!app_state) {
        throw std::runtime_error(sprint("Host %s does not have HOST_ID application_state", endpoint));
    }
    return utils::UUID(app_state->value);
}

std::experimental::optional<endpoint_state> gossiper::get_state_for_version_bigger_than(inet_address for_endpoint, int version) {
    std::experimental::optional<endpoint_state> reqd_endpoint_state;
    auto es = get_endpoint_state_for_endpoint_ptr(for_endpoint);
    if (es) {
        auto& eps = *es;
        /*
             * Here we try to include the Heart Beat state only if it is
             * greater than the version passed in. It might happen that
             * the heart beat version maybe lesser than the version passed
             * in and some application state has a version that is greater
             * than the version passed in. In this case we also send the old
             * heart beat and throw it away on the receiver if it is redundant.
            */
        int local_hb_version = eps.get_heart_beat_state().get_heart_beat_version();
        if (local_hb_version > version) {
            reqd_endpoint_state.emplace(eps.get_heart_beat_state());
            logger.trace("local heartbeat version {} greater than {} for {}", local_hb_version, version, for_endpoint);
        }
        /* Accumulate all application states whose versions are greater than "version" variable */
        for (auto& entry : eps.get_application_state_map()) {
            auto& value = entry.second;
            if (value.version > version) {
                if (!reqd_endpoint_state) {
                    reqd_endpoint_state.emplace(eps.get_heart_beat_state());
                }
                auto& key = entry.first;
                logger.trace("Adding state of {}, {}: {}" , for_endpoint, key, value.value);
                reqd_endpoint_state->add_application_state(key, value);
            }
        }
    }
    return reqd_endpoint_state;
}

int gossiper::compare_endpoint_startup(inet_address addr1, inet_address addr2) {
    auto* ep1 = get_endpoint_state_for_endpoint_ptr(addr1);
    auto* ep2 = get_endpoint_state_for_endpoint_ptr(addr2);
    if (!ep1 || !ep2) {
        auto err = sprint("Can not get endpoint_state for %s or %s", addr1, addr2);
        logger.warn("{}", err);
        throw std::runtime_error(err);
    }
    return ep1->get_heart_beat_state().get_generation() - ep2->get_heart_beat_state().get_generation();
}

void gossiper::notify_failure_detector(const std::map<inet_address, endpoint_state>& remoteEpStateMap) {
    for (auto& entry : remoteEpStateMap) {
        notify_failure_detector(entry.first, entry.second);
    }
}

// Runs inside seastar::async context
void gossiper::mark_alive(inet_address addr, endpoint_state& local_state) {
    // if (MessagingService.instance().getVersion(addr) < MessagingService.VERSION_20) {
    //     real_mark_alive(addr, local_state);
    //     return;
    // }
    auto inserted = _pending_mark_alive_endpoints.insert(addr).second;
    if (inserted) {
        // The node is not in the _pending_mark_alive_endpoints
        logger.debug("Mark Node {} alive with EchoMessage", addr);
    } else {
        // We are in the progress of marking this node alive
        logger.debug("Node {} is being marked as up, ignoring duplicated mark alive operation", addr);
        return;
    }

    local_state.mark_dead();
    msg_addr id = get_msg_addr(addr);
    logger.trace("Sending a EchoMessage to {}", id);
    ms().send_gossip_echo(id).then([this, addr] {
        logger.trace("Got EchoMessage Reply");
        set_last_processed_message_at();
        return seastar::async([this, addr] {
            // After sending echo message, the Node might not be in the
            // endpoint_state_map anymore, use the reference of local_state
            // might cause user-after-free
            auto es = get_endpoint_state_for_endpoint_ptr(addr);
            if (!es) {
                logger.info("Node {} is not in endpoint_state_map anymore", addr);
            } else {
                endpoint_state& state = *es;
                logger.debug("Mark Node {} alive after EchoMessage", addr);
                real_mark_alive(addr, state);
            }
        });
    }).finally([this, addr] {
        _pending_mark_alive_endpoints.erase(addr);
    }).handle_exception([addr] (auto ep) {
        logger.warn("Fail to send EchoMessage to {}: {}", addr, ep);
    });
}

// Runs inside seastar::async context
void gossiper::real_mark_alive(inet_address addr, endpoint_state& local_state) {
    logger.trace("marking as alive {}", addr);
    local_state.mark_alive();
    local_state.update_timestamp(); // prevents do_status_check from racing us and evicting if it was down > A_VERY_LONG_TIME
    auto it_ = std::find(_live_endpoints.begin(), _live_endpoints.end(), addr);
    if (it_ == _live_endpoints.end()) {
        _live_endpoints.push_back(addr);
    }
    auto it = std::find(_live_endpoints_just_added.begin(), _live_endpoints_just_added.end(), addr);
    if (it == _live_endpoints_just_added.end()) {
        _live_endpoints_just_added.push_back(addr);
    }
    _unreachable_endpoints.erase(addr);
    _expire_time_endpoint_map.erase(addr);
    logger.debug("removing expire time for endpoint : {}", addr);
    if (!_in_shadow_round) {
        logger.info("InetAddress {} is now UP, status = {}", addr, get_gossip_status(local_state));
    }

    _subscribers.for_each([addr, local_state] (auto& subscriber) {
        subscriber->on_alive(addr, local_state);
        logger.trace("Notified {}", subscriber.get());
    });
}

// Runs inside seastar::async context
void gossiper::mark_dead(inet_address addr, endpoint_state& local_state) {
    logger.trace("marking as down {}", addr);
    local_state.mark_dead();
    _live_endpoints.erase(std::remove(_live_endpoints.begin(), _live_endpoints.end(), addr), _live_endpoints.end());
    _live_endpoints_just_added.remove(addr);
    _unreachable_endpoints[addr] = now();
    logger.info("InetAddress {} is now DOWN, status = {}", addr, get_gossip_status(local_state));
    _subscribers.for_each([addr, local_state] (auto& subscriber) {
        subscriber->on_dead(addr, local_state);
        logger.trace("Notified {}", subscriber.get());
    });
}

// Runs inside seastar::async context
void gossiper::handle_major_state_change(inet_address ep, const endpoint_state& eps) {
    auto eps_old = get_endpoint_state_for_endpoint(ep);
    if (!is_dead_state(eps) && !_in_shadow_round) {
        if (endpoint_state_map.count(ep))  {
            logger.debug("Node {} has restarted, now UP, status = {}", ep, get_gossip_status(eps));
        } else {
            logger.debug("Node {} is now part of the cluster, status = {}", ep, get_gossip_status(eps));
        }
    }
    logger.trace("Adding endpoint state for {}, status = {}", ep, get_gossip_status(eps));
    endpoint_state_map[ep] = eps;
    replicate(ep, eps).get();

    if (_in_shadow_round) {
        // In shadow round, we only interested in the peer's endpoint_state,
        // e.g., gossip features, host_id, tokens. No need to call the
        // on_restart or on_join callbacks or to go through the mark alive
        // procedure with EchoMessage gossip message. We will do them during
        // normal gossip runs anyway.
        logger.debug("In shadow round addr={}, eps={}", ep, eps);
        return;
    }

    if (eps_old) {
        // the node restarted: it is up to the subscriber to take whatever action is necessary
        _subscribers.for_each([ep, eps_old] (auto& subscriber) {
            subscriber->on_restart(ep, *eps_old);
        });
    }

    auto& ep_state = endpoint_state_map.at(ep);
    if (!is_dead_state(ep_state)) {
        mark_alive(ep, ep_state);
    } else {
        logger.debug("Not marking {} alive due to dead state {}", ep, get_gossip_status(eps));
        mark_dead(ep, ep_state);
    }

    auto* eps_new = get_endpoint_state_for_endpoint_ptr(ep);
    if (eps_new) {
        _subscribers.for_each([ep, eps_new] (auto& subscriber) {
            subscriber->on_join(ep, *eps_new);
        });
    }
    // check this at the end so nodes will learn about the endpoint
    if (is_shutdown(ep)) {
        mark_as_shutdown(ep);
    }
}

bool gossiper::is_dead_state(const endpoint_state& eps) const {
    sstring state = get_gossip_status(eps);
    for (auto& deadstate : DEAD_STATES) {
        if (state == deadstate) {
            return true;
        }
    }
    return false;
}

bool gossiper::is_shutdown(const inet_address& endpoint) const {
    return get_gossip_status(endpoint) == sstring(versioned_value::SHUTDOWN);
}

bool gossiper::is_normal(const inet_address& endpoint) const {
    return get_gossip_status(endpoint) == sstring(versioned_value::STATUS_NORMAL);
}

bool gossiper::is_silent_shutdown_state(const endpoint_state& ep_state) const{
    sstring state = get_gossip_status(ep_state);
    for (auto& deadstate : SILENT_SHUTDOWN_STATES) {
        if (state == deadstate) {
            return true;
        }
    }
    return false;
}

// Runs inside seastar::async context
void gossiper::apply_new_states(inet_address addr, endpoint_state& local_state, const endpoint_state& remote_state) {
    // don't assert here, since if the node restarts the version will go back to zero
    //int oldVersion = local_state.get_heart_beat_state().get_heart_beat_version();

    local_state.set_heart_beat_state_and_update_timestamp(remote_state.get_heart_beat_state());
    // if (logger.isTraceEnabled()) {
    //     logger.trace("Updating heartbeat state version to {} from {} for {} ...",
    //     local_state.get_heart_beat_state().get_heart_beat_version(), oldVersion, addr);
    // }

    std::vector<application_state> changed;
    auto&& remote_map = remote_state.get_application_state_map();

    // Exceptions thrown from listeners will result in abort because that could leave the node in a bad
    // state indefinitely. Unless the value changes again, we wouldn't retry notifications.
    // Some values are set only once, so listeners would never be re-run.
    // Listeners should decide which failures are non-fatal and swallow them.
    auto run_listeners = seastar::defer([&] () noexcept {
        for (auto&& key : changed) {
            do_on_change_notifications(addr, key, remote_map.at(key));
        }
    });

    // We must replicate endpoint states before listeners run.
    // Exceptions during replication will cause abort because node's state
    // would be inconsistent across shards. Changes listeners depend on state
    // being replicated to all shards.
    auto replicate_changes = seastar::defer([&] () noexcept {
        replicate(addr, remote_map, changed).get();
    });

    // we need to make two loops here, one to apply, then another to notify,
    // this way all states in an update are present and current when the notifications are received
    for (const auto& remote_entry : remote_map) {
        const auto& remote_key = remote_entry.first;
        const auto& remote_value = remote_entry.second;
        auto remote_gen = remote_state.get_heart_beat_state().get_generation();
        auto local_gen = local_state.get_heart_beat_state().get_generation();
        if(remote_gen != local_gen) {
            auto err = sprint("Remote generation %d != local generation %d", remote_gen, local_gen);
            logger.warn("{}", err);
            throw std::runtime_error(err);
        }

        const versioned_value* local_val = local_state.get_application_state_ptr(remote_key);
        if (!local_val || remote_value.version > local_val->version) {
            changed.push_back(remote_key);
            local_state.add_application_state(remote_key, remote_value);
        }
    }
}

// Runs inside seastar::async context
void gossiper::do_before_change_notifications(inet_address addr, const endpoint_state& ep_state, const application_state& ap_state, const versioned_value& new_value) {
    _subscribers.for_each([addr, ep_state, ap_state, new_value] (auto& subscriber) {
        subscriber->before_change(addr, ep_state, ap_state, new_value);
    });
}

// Runs inside seastar::async context
void gossiper::do_on_change_notifications(inet_address addr, const application_state& state, const versioned_value& value) {
    _subscribers.for_each([addr, state, value] (auto& subscriber) {
        subscriber->on_change(addr, state, value);
    });
}

void gossiper::request_all(gossip_digest& g_digest,
    std::vector<gossip_digest>& delta_gossip_digest_list, int remote_generation) {
    /* We are here since we have no data for this endpoint locally so request everthing. */
    delta_gossip_digest_list.emplace_back(g_digest.get_endpoint(), remote_generation, 0);
    logger.trace("request_all for {}", g_digest.get_endpoint());
}

void gossiper::send_all(gossip_digest& g_digest,
    std::map<inet_address, endpoint_state>& delta_ep_state_map,
    int max_remote_version) {
    auto ep = g_digest.get_endpoint();
    logger.trace("send_all(): ep={}, version > {}", ep, max_remote_version);
    auto local_ep_state_ptr = get_state_for_version_bigger_than(ep, max_remote_version);
    if (local_ep_state_ptr) {
        delta_ep_state_map[ep] = *local_ep_state_ptr;
    }
}

void gossiper::examine_gossiper(std::vector<gossip_digest>& g_digest_list,
    std::vector<gossip_digest>& delta_gossip_digest_list,
    std::map<inet_address, endpoint_state>& delta_ep_state_map) {
    if (g_digest_list.size() == 0) {
        /* we've been sent a *completely* empty syn, which should normally
             * never happen since an endpoint will at least send a syn with
             * itself.  If this is happening then the node is attempting shadow
             * gossip, and we should reply with everything we know.
             */
        logger.debug("Shadow request received, adding all states");
        for (auto& entry : endpoint_state_map) {
            g_digest_list.emplace_back(entry.first, 0, 0);
        }
    }
    for (gossip_digest& g_digest : g_digest_list) {
        int remote_generation = g_digest.get_generation();
        int max_remote_version = g_digest.get_max_version();
        /* Get state associated with the end point in digest */
        auto&& ep = g_digest.get_endpoint();
        auto es = get_endpoint_state_for_endpoint_ptr(ep);
        /* Here we need to fire a GossipDigestAckMessage. If we have some
             * data associated with this endpoint locally then we follow the
             * "if" path of the logic. If we have absolutely nothing for this
             * endpoint we need to request all the data for this endpoint.
             */
        if (es) {
            endpoint_state& ep_state_ptr = *es;
            int local_generation = ep_state_ptr.get_heart_beat_state().get_generation();
            /* get the max version of all keys in the state associated with this endpoint */
            int max_local_version = get_max_endpoint_state_version(ep_state_ptr);
            logger.trace("examine_gossiper(): ep={}, remote={}.{}, local={}.{}", ep,
                remote_generation, max_remote_version, local_generation, max_local_version);
            if (remote_generation == local_generation && max_remote_version == max_local_version) {
                continue;
            }

            if (remote_generation > local_generation) {
                /* we request everything from the gossiper */
                request_all(g_digest, delta_gossip_digest_list, remote_generation);
            } else if (remote_generation < local_generation) {
                /* send all data with generation = localgeneration and version > 0 */
                send_all(g_digest, delta_ep_state_map, 0);
            } else if (remote_generation == local_generation) {
                /*
                 * If the max remote version is greater then we request the
                 * remote endpoint send us all the data for this endpoint
                 * with version greater than the max version number we have
                 * locally for this endpoint.
                 *
                 * If the max remote version is lesser, then we send all
                 * the data we have locally for this endpoint with version
                 * greater than the max remote version.
                 */
                if (max_remote_version > max_local_version) {
                    logger.trace("examine_gossiper(): requesting version > {} from {}", max_local_version, g_digest.get_endpoint());
                    delta_gossip_digest_list.emplace_back(g_digest.get_endpoint(), remote_generation, max_local_version);
                } else if (max_remote_version < max_local_version) {
                    /* send all data with generation = localgeneration and version > max_remote_version */
                    send_all(g_digest, delta_ep_state_map, max_remote_version);
                }
            }
        } else {
            /* We are here since we have no data for this endpoint locally so request everything. */
            request_all(g_digest, delta_gossip_digest_list, remote_generation);
        }
    }
}

future<> gossiper::start_gossiping(int generation_number, bind_messaging_port do_bind) {
    return start_gossiping(generation_number, std::map<application_state, versioned_value>(), do_bind);
}

future<> gossiper::start_gossiping(int generation_nbr, std::map<application_state, versioned_value> preload_local_states, bind_messaging_port do_bind) {
    // Although gossiper runs on cpu0 only, we need to listen incoming gossip
    // message on all cpus and forard them to cpu0 to process.
    return get_gossiper().invoke_on_all([do_bind] (gossiper& g) {
        g.init_messaging_service_handler(do_bind);
    }).then([this, generation_nbr, preload_local_states] {
        build_seeds_list();
        /* initialize the heartbeat state for this localEndpoint */
        maybe_initialize_local_state(generation_nbr);
        endpoint_state& local_state = endpoint_state_map[get_broadcast_address()];
        for (auto& entry : preload_local_states) {
            local_state.add_application_state(entry.first, entry.second);
        }

        auto generation = local_state.get_heart_beat_state().get_generation();

        return replicate(get_broadcast_address(), local_state).then([] {
            //notify snitches that Gossiper is about to start
            return locator::i_endpoint_snitch::get_local_snitch_ptr()->gossiper_starting();
        }).then([this, generation] {
            logger.trace("gossip started with generation {}", generation);
            _enabled = true;
            _nr_run = 0;
            _scheduled_gossip_task.arm(INTERVAL);
            return make_ready_future<>();
        });
    });
}

future<> gossiper::do_shadow_round() {
    return seastar::async([this, g = this->shared_from_this()] {
        build_seeds_list();
        _in_shadow_round = true;
        auto t = clk::now();

        // When peer node receives a syn message, it will send back a ack message.
        // So, we need to register gossip message handlers before sending syn message.
        get_gossiper().invoke_on_all([] (gossiper& g) {
            g.init_messaging_service_handler();
        }).get();

        auto& cfg = service::get_local_storage_service().db().local().get_config();
        while (this->_in_shadow_round) {
            // send a completely empty syn
            for (inet_address seed : _seeds) {
                std::vector<gossip_digest> digests;
                gossip_digest_syn message(get_cluster_name(), get_partitioner_name(), digests);
                auto id = get_msg_addr(seed);
                logger.trace("Sending a GossipDigestSyn (ShadowRound) to {} ...", id);
                ms().send_gossip_digest_syn(id, std::move(message)).handle_exception([id] (auto ep) {
                    logger.trace("Fail to send GossipDigestSyn (ShadowRound) to {}: {}", id, ep);
                });
            }
            sleep(std::chrono::seconds(1)).get();
            if (this->_in_shadow_round) {
                if (clk::now() > t + std::chrono::milliseconds(cfg.shadow_round_ms())) {
                    throw std::runtime_error(sprint("Unable to gossip with any seeds (ShadowRound)"));
                }
                logger.info("Connect seeds again ... ({} seconds passed)", std::chrono::duration_cast<std::chrono::seconds>(clk::now() - t).count());
            }
        }
    });
}

void gossiper::build_seeds_list() {
    for (inet_address seed : get_seeds() ) {
        if (seed == get_broadcast_address()) {
            continue;
        }
        _seeds.emplace(seed);
    }
}

void gossiper::maybe_initialize_local_state(int generation_nbr) {
    heart_beat_state hb_state(generation_nbr);
    endpoint_state local_state(hb_state);
    local_state.mark_alive();
    inet_address ep = get_broadcast_address();
    auto it = endpoint_state_map.find(ep);
    if (it == endpoint_state_map.end()) {
        endpoint_state_map.emplace(ep, local_state);
    }
}

void gossiper::add_saved_endpoint(inet_address ep) {
    if (ep == get_broadcast_address()) {
        logger.debug("Attempt to add self as saved endpoint");
        return;
    }

    //preserve any previously known, in-memory data about the endpoint (such as DC, RACK, and so on)
    auto ep_state = endpoint_state(heart_beat_state(0));
    auto es = get_endpoint_state_for_endpoint_ptr(ep);
    if (es) {
        ep_state = *es;
        logger.debug("not replacing a previous ep_state for {}, but reusing it: {}", ep, ep_state);
        ep_state.set_heart_beat_state_and_update_timestamp(heart_beat_state(0));
    }
    ep_state.mark_dead();
    endpoint_state_map[ep] = ep_state;
    _unreachable_endpoints[ep] = now();
    logger.trace("Adding saved endpoint {} {}", ep, ep_state.get_heart_beat_state().get_generation());
}

future<> gossiper::add_local_application_state(application_state state, versioned_value value) {
    return add_local_application_state({ {std::move(state), std::move(value)} });
}

future<> gossiper::add_local_application_state(std::initializer_list<std::pair<application_state, utils::in<versioned_value>>> args) {
    using in_pair_type = std::pair<application_state, utils::in<versioned_value>>;
    using out_pair_type = std::pair<application_state, versioned_value>;
    using vector_type = std::vector<out_pair_type>;

    return add_local_application_state(boost::copy_range<vector_type>(args | boost::adaptors::transformed([](const in_pair_type& p) {
        return out_pair_type(p.first, p.second.move());
    })));
}


// Depends on:
// - before_change callbacks
// - on_change callbacks
// #2894. Similar to origin fix, but relies on non-interruptability to ensure we apply
// values "in order".
//
// NOTE: having the values being actual versioned values here is sort of pointless, because
// we overwrite the version to ensure the set is monotonic. However, it does not break anything,
// and changing this tends to spread widely (see versioned_value::factory), so that can be its own
// change later, if needed.
// Retaining the slightly broken signature is also cosistent with origin. Hooray.
//
future<> gossiper::add_local_application_state(std::vector<std::pair<application_state, versioned_value>> states) {
    if (states.empty()) {
        return make_ready_future<>();
    }
    return container().invoke_on(0, [states = std::move(states)] (gossiper& gossiper) mutable {
        return seastar::async([g = gossiper.shared_from_this(), states = std::move(states)]() mutable {
            auto& gossiper = *g;
            inet_address ep_addr = gossiper.get_broadcast_address();
            // for symmetry with other apply, use endpoint lock for our own address.
            auto permit = gossiper.lock_endpoint(ep_addr).get0();
            auto es = gossiper.get_endpoint_state_for_endpoint_ptr(ep_addr);
            if (!es) {
                auto err = sprint("endpoint_state_map does not contain endpoint = %s, application_states = %s",
                                  ep_addr, states);
                logger.error("{}", err);
                throw std::runtime_error(err);
            }

            endpoint_state ep_state_before = *es;

            for (auto& p : states) {
                auto& state = p.first;
                auto& value = p.second;
                // Fire "before change" notifications:
                // Not explicit, but apparently we allow this to defer (inside out implicit seastar::async)
                gossiper.do_before_change_notifications(ep_addr, ep_state_before, state, value);
            }

            es = gossiper.get_endpoint_state_for_endpoint_ptr(ep_addr);
            if (!es) {
                return;
            }

            for (auto& p : states) {
                auto& state = p.first;
                auto& value = p.second;
                // Notifications may have taken some time, so preventively raise the version
                // of the new value, otherwise it could be ignored by the remote node
                // if another value with a newer version was received in the meantime:
                value = storage_service_value_factory().clone_with_higher_version(value);
                // Add to local application state
                es->add_application_state(state, value);
            }
            for (auto& p : states) {
                auto& state = p.first;
                auto& value = p.second;
                // fire "on change" notifications:
                // now we might defer again, so this could be reordered. But we've
                // ensured the whole set of values are monotonically versioned and
                // applied to endpoint state.
                gossiper.replicate(ep_addr, state, value).get();
                gossiper.do_on_change_notifications(ep_addr, state, value);
            }
        }).handle_exception([] (auto ep) {
            logger.warn("Fail to apply application_state: {}", ep);
        });
    });
}

future<> gossiper::do_stop_gossiping() {
    if (!is_enabled()) {
        logger.info("gossip is already stopped");
        return make_ready_future<>();
    }
    return seastar::async([this, g = this->shared_from_this()] {
        _enabled = false;
        auto* my_ep_state = get_endpoint_state_for_endpoint_ptr(get_broadcast_address());
        if (my_ep_state) {
            logger.info("My status = {}", get_gossip_status(*my_ep_state));
        }
        if (my_ep_state && !is_silent_shutdown_state(*my_ep_state)) {
            logger.info("Announcing shutdown");
            add_local_application_state(application_state::STATUS, storage_service_value_factory().shutdown(true)).get();
            for (inet_address addr : _live_endpoints) {
                msg_addr id = get_msg_addr(addr);
                logger.trace("Sending a GossipShutdown to {}", id);
                ms().send_gossip_shutdown(id, get_broadcast_address()).then_wrapped([id] (auto&&f) {
                    try {
                        f.get();
                        logger.trace("Got GossipShutdown Reply");
                    } catch (...) {
                        logger.warn("Fail to send GossipShutdown to {}: {}", id, std::current_exception());
                    }
                    return make_ready_future<>();
                }).get();
            }
            auto& cfg = service::get_local_storage_service().db().local().get_config();
            sleep(std::chrono::milliseconds(cfg.shutdown_announce_in_ms())).get();
        } else {
            logger.warn("No local state or state is in silent shutdown, not announcing shutdown");
        }
        _scheduled_gossip_task.cancel();
        timer_callback_lock().get();
        //
        // Release the timer semaphore since storage_proxy may be waiting for
        // it.
        // Gossiper timer is promised to be neither running nor scheduled.
        //
        timer_callback_unlock();
        get_gossiper().invoke_on_all([] (gossiper& g) {
            if (engine().cpu_id() == 0) {
                get_local_failure_detector().unregister_failure_detection_event_listener(&g);
            }
            g.uninit_messaging_service_handler();
            g._features_condvar.broken();
            return make_ready_future<>();
        }).get();
    });
}

future<> stop_gossiping() {
    return smp::submit_to(0, [] {
        if (get_gossiper().local_is_initialized()) {
            return get_local_gossiper().do_stop_gossiping();
        }
        return make_ready_future<>();
    });
}

future<> gossiper::stop() {
    return make_ready_future();
}

bool gossiper::is_enabled() const {
    return _enabled;
}

void gossiper::goto_shadow_round() {
    _in_shadow_round = true;
}

void gossiper::finish_shadow_round() {
    _in_shadow_round = false;
}

bool gossiper::is_in_shadow_round() const {
    return _in_shadow_round;
}

void gossiper::add_expire_time_for_endpoint(inet_address endpoint, clk::time_point expire_time) {
    char expire_time_buf[100];
    auto expire_time_tm = clk::to_time_t(expire_time);
    auto now_ = now();
    strftime(expire_time_buf, sizeof(expire_time_buf), "%Y-%m-%d %T", std::localtime(&expire_time_tm));
    auto diff = std::chrono::duration_cast<std::chrono::seconds>(expire_time - now_).count();
    logger.info("Node {} will be removed from gossip at [{}]: (expire = {}, now = {}, diff = {} seconds)",
            endpoint, expire_time_buf, expire_time.time_since_epoch().count(),
            now_.time_since_epoch().count(), diff);
    _expire_time_endpoint_map[endpoint] = expire_time;
}

clk::time_point gossiper::compute_expire_time() {
    return now() + A_VERY_LONG_TIME;
}

void gossiper::dump_endpoint_state_map() {
    for (auto& x : endpoint_state_map) {
        logger.debug("ep={}, eps={}", x.first, x.second);
    }
}

void gossiper::debug_show() {
    auto reporter = std::make_shared<timer<std::chrono::steady_clock>>();
    reporter->set_callback ([reporter] {
        auto& gossiper = gms::get_local_gossiper();
        gossiper.dump_endpoint_state_map();
    });
    reporter->arm_periodic(std::chrono::milliseconds(1000));
}

bool gossiper::is_alive(inet_address ep) const {
    if (ep == get_broadcast_address()) {
        return true;
    }
    auto* eps = get_endpoint_state_for_endpoint_ptr(std::move(ep));
    // we could assert not-null, but having isAlive fail screws a node over so badly that
    // it's worth being defensive here so minor bugs don't cause disproportionate
    // badness.  (See CASSANDRA-1463 for an example).
    if (eps) {
        return eps->is_alive();
    }
    logger.warn("unknown endpoint {}", ep);
    return false;
}

const versioned_value* gossiper::get_application_state_ptr(inet_address endpoint, application_state appstate) const {
    auto* eps = get_endpoint_state_for_endpoint_ptr(std::move(endpoint));
    if (!eps) {
        return nullptr;
    }
    return eps->get_application_state_ptr(appstate);
}

/**
 * This method is used to mark a node as shutdown; that is it gracefully exited on its own and told us about it
 * @param endpoint endpoint that has shut itself down
 */
// Runs inside seastar::async context
void gossiper::mark_as_shutdown(const inet_address& endpoint) {
    auto es = get_endpoint_state_for_endpoint_ptr(endpoint);
    if (es) {
        auto& ep_state = *es;
        ep_state.add_application_state(application_state::STATUS, storage_service_value_factory().shutdown(true));
        ep_state.get_heart_beat_state().force_highest_possible_version_unsafe();
        mark_dead(endpoint, ep_state);
        get_local_failure_detector().force_conviction(endpoint);
    }
}

void gossiper::force_newer_generation() {
    auto es = get_endpoint_state_for_endpoint_ptr(get_broadcast_address());
    if (es) {
        es->get_heart_beat_state().force_newer_generation_unsafe();
    }
}

static sstring do_get_gossip_status(const gms::versioned_value* app_state) {
    if (!app_state) {
        return "";
    }
    auto value = app_state->value;
    std::vector<sstring> pieces;
    boost::split(pieces, value, boost::is_any_of(","));
    if (pieces.empty()) {
        return "";
    }
    return pieces[0];
}

sstring gossiper::get_gossip_status(const endpoint_state& ep_state) const {
    return do_get_gossip_status(ep_state.get_application_state_ptr(application_state::STATUS));
}

sstring gossiper::get_gossip_status(const inet_address& endpoint) const {
    return do_get_gossip_status(get_application_state_ptr(endpoint, application_state::STATUS));
}

future<> gossiper::wait_for_gossip(std::chrono::milliseconds initial_delay, stdx::optional<int32_t> force_after) {
    static constexpr std::chrono::milliseconds GOSSIP_SETTLE_MIN_WAIT_MS{5000};
    static constexpr std::chrono::milliseconds GOSSIP_SETTLE_POLL_INTERVAL_MS{1000};
    static constexpr int32_t GOSSIP_SETTLE_POLL_SUCCESSES_REQUIRED = 3;

    return seastar::async([this, initial_delay, force_after] {
        int32_t total_polls = 0;
        int32_t num_okay = 0;
        int32_t ep_size = endpoint_state_map.size();

        auto delay = initial_delay;

        sleep(GOSSIP_SETTLE_MIN_WAIT_MS).get();
        while (num_okay < GOSSIP_SETTLE_POLL_SUCCESSES_REQUIRED) {
            sleep(delay).get();
            delay = GOSSIP_SETTLE_POLL_INTERVAL_MS;

            int32_t current_size = endpoint_state_map.size();
            total_polls++;
            if (current_size == ep_size && _msg_processing == 0) {
                logger.debug("Gossip looks settled");
                num_okay++;
            } else {
                logger.info("Gossip not settled after {} polls.", total_polls);
                num_okay = 0;
            }
            ep_size = current_size;
            if (force_after && *force_after > 0 && total_polls > *force_after) {
                logger.warn("Gossip not settled but startup forced by skip_wait_for_gossip_to_settle. Gossp total polls: {}", total_polls);
                break;
            }
        }
        if (total_polls > GOSSIP_SETTLE_POLL_SUCCESSES_REQUIRED) {
            logger.info("Gossip settled after {} extra polls; proceeding", total_polls - GOSSIP_SETTLE_POLL_SUCCESSES_REQUIRED);
        } else {
            logger.info("No gossip backlog; proceeding");
        }
    });
}

future<> gossiper::wait_for_gossip_to_settle() {
    static constexpr std::chrono::milliseconds GOSSIP_SETTLE_MIN_WAIT_MS{5000};

    auto& cfg = service::get_local_storage_service().db().local().get_config();
    auto force_after = cfg.skip_wait_for_gossip_to_settle();
    if (force_after == 0) {
        return make_ready_future<>();
    }
    logger.info("Waiting for gossip to settle before accepting client requests...");
    return wait_for_gossip(GOSSIP_SETTLE_MIN_WAIT_MS, force_after);
}

future<> gossiper::wait_for_range_setup() {
    logger.info("Waiting for pending range setup...");
    auto ring_delay = service::get_local_storage_service().get_ring_delay();
    return wait_for_gossip(ring_delay);
}

bool gossiper::is_safe_for_bootstrap(inet_address endpoint) {
    auto* eps = get_endpoint_state_for_endpoint_ptr(endpoint);

    // if there's no previous state, or the node was previously removed from the cluster, we're good
    if (!eps || is_dead_state(*eps)) {
        return true;
    }

    sstring status = get_gossip_status(*eps);

    logger.debug("is_safe_for_bootstrap: node {} status {}", endpoint, status);

    // these states are not allowed to join the cluster as it would not be safe
    std::unordered_set<sstring> unsafe_statuses{
        sstring(""), // failed bootstrap but we did start gossiping
        sstring(versioned_value::STATUS_NORMAL), // node is legit in the cluster or it was stopped with kill -9
        sstring(versioned_value::SHUTDOWN) // node was shutdown
    };

    return !unsafe_statuses.count(status);
}

std::set<sstring> to_feature_set(sstring features_string) {
    std::set<sstring> features;
    boost::split(features, features_string, boost::is_any_of(","));
    features.erase("");
    return features;
}

std::set<sstring> gossiper::get_supported_features(inet_address endpoint) const {
    auto app_state = get_application_state_ptr(endpoint, application_state::SUPPORTED_FEATURES);
    if (!app_state) {
        return {};
    }
    return to_feature_set(app_state->value);
}

std::set<sstring> gossiper::get_supported_features() const {
    std::unordered_map<inet_address, std::set<sstring>> features_map;
    std::set<sstring> common_features;

    for (auto& x : endpoint_state_map) {
        auto endpoint = x.first;
        auto features = get_supported_features(endpoint);
        if (features.empty()) {
            return std::set<sstring>();
        }
        if (common_features.empty()) {
            common_features = features;
        }
        features_map.emplace(endpoint, std::move(features));
    }

    for (auto& x : features_map) {
        auto& features = x.second;
        std::set<sstring> result;
        std::set_intersection(features.begin(), features.end(),
                common_features.begin(), common_features.end(),
                std::inserter(result, result.end()));
        common_features = std::move(result);
    }
    common_features.erase("");
    return common_features;
}

std::set<sstring> gossiper::get_supported_features(std::unordered_map<gms::inet_address, sstring> peer_features_string) {
    std::set<sstring> common_features;
    // Convert feature string split by "," to std::set
    std::unordered_map<gms::inet_address, std::set<sstring>> features_map;
    for (auto& x : peer_features_string) {
        std::set<sstring> features = to_feature_set(x.second);
        if (features.empty()) {
            return std::set<sstring>();
        }
        if (common_features.empty()) {
            common_features = features;
        }
        features_map.emplace(x.first, features);
    }

    for (auto& x : features_map) {
        auto& features = x.second;
        std::set<sstring> result;
        std::set_intersection(features.begin(), features.end(),
                common_features.begin(), common_features.end(),
                std::inserter(result, result.end()));
        common_features = std::move(result);
    }
    common_features.erase("");
    return common_features;
}

void gossiper::check_knows_remote_features(sstring local_features_string) const {
    std::set<sstring> local_features = to_feature_set(local_features_string);
    auto local_endpoint = get_broadcast_address();
    auto common_features = get_supported_features();
    if (boost::range::includes(local_features, common_features)) {
        logger.info("Feature check passed. Local node {} features = {}, Remote common_features = {}",
                local_endpoint, local_features, common_features);
    } else {
        throw std::runtime_error(sprint("Feature check failed. This node can not join the cluster because it does not understand the feature. Local node %s features = %s, Remote common_features = %s", local_endpoint, local_features, common_features));
    }
}

void gossiper::check_knows_remote_features(sstring local_features_string, std::unordered_map<inet_address, sstring> peer_features_string) const {
    std::set<sstring> local_features = to_feature_set(local_features_string);
    auto local_endpoint = get_broadcast_address();
    auto common_features = get_supported_features(peer_features_string);
    if (boost::range::includes(local_features, common_features)) {
        logger.info("Feature check passed. Local node {} features = {}, Remote common_features = {}",
                local_endpoint, local_features, common_features);
    } else {
        throw std::runtime_error(sprint("Feature check failed. This node can not join the cluster because it does not understand the feature. Local node %s features = %s, Remote common_features = %s", local_endpoint, local_features, common_features));
    }
}

static bool check_features(std::set<sstring> features, std::set<sstring> need_features) {
    logger.debug("Checking if need_features {} in features {}", need_features, features);
    return boost::range::includes(features, need_features);
}

future<> gossiper::wait_for_feature_on_all_node(std::set<sstring> features) {
    return _features_condvar.wait([this, features = std::move(features)] {
        return check_features(get_supported_features(), features);
    });
}

future<> gossiper::wait_for_feature_on_node(std::set<sstring> features, inet_address endpoint) {
    return _features_condvar.wait([this, features = std::move(features), endpoint = std::move(endpoint)] {
        return check_features(get_supported_features(endpoint), features);
    });
}

void gossiper::register_feature(feature* f) {
    if (check_features(get_local_gossiper().get_supported_features(), {f->name()})) {
        f->enable();
    } else {
        _registered_features.emplace(f->name(), std::vector<feature*>()).first->second.emplace_back(f);
    }
}

void gossiper::unregister_feature(feature* f) {
    auto&& fsit = _registered_features.find(f->name());
    if (fsit == _registered_features.end()) {
        return;
    }
    auto&& fs = fsit->second;
    auto it = std::find(fs.begin(), fs.end(), f);
    if (it != fs.end()) {
        fs.erase(it);
    }
}

// Runs inside seastar::async context
void gossiper::maybe_enable_features() {
    if (_registered_features.empty()) {
        _features_condvar.broadcast();
        return;
    }

    auto&& features = get_supported_features();
    container().invoke_on_all([&features] (gossiper& g) {
        for (auto it = g._registered_features.begin(); it != g._registered_features.end();) {
            if (features.find(it->first) != features.end()) {
                for (auto&& f : it->second) {
                    f->enable();
                }
                it = g._registered_features.erase(it);
            } else {
                ++it;
            }
        }
        g._features_condvar.broadcast();
    }).get();
}

feature::feature(sstring name, bool enabled)
        : _name(name)
        , _enabled(enabled) {
    if (!_enabled) {
        get_local_gossiper().register_feature(this);
    } else {
        _pr.set_value();
    }
}

feature::~feature() {
    if (!_enabled) {
        auto& gossiper = get_gossiper();
        if (gossiper.local_is_initialized()) {
            gossiper.local().unregister_feature(this);
        }
    }
}

feature& feature::operator=(feature&& other) {
    if (!_enabled) {
        get_local_gossiper().unregister_feature(this);
    }
    _name = other._name;
    _enabled = other._enabled;
    _pr = std::move(other._pr);
    if (!_enabled) {
        get_local_gossiper().register_feature(this);
    }
    return *this;
}

void feature::enable() {
    if (engine().cpu_id() == 0) {
        logger.info("Feature {} is enabled", name());
    }
    if (!_enabled) {
        _enabled = true;
        _pr.set_value();
    }
}

} // namespace gms
