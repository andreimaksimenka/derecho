/**
 * @file rpc_manager.cpp
 *
 * @date Feb 7, 2017
 * @author edward
 */

#include <cassert>

#include "rpc_manager.h"

namespace derecho {

namespace rpc {

RPCManager::~RPCManager() {
    thread_shutdown = true;
    if(rpc_thread.joinable()) {
        rpc_thread.join();
    }
    connections.destroy();
}

std::exception_ptr RPCManager::handle_receive(
        const Opcode& indx, const node_id_t& received_from, char const* const buf,
        std::size_t payload_size, const std::function<char*(int)>& out_alloc) {
    using namespace remote_invocation_utilities;
    assert(payload_size);
    auto reply_header_size = header_space();
    recv_ret reply_return = receivers->at(indx)(
            &dsm, received_from, buf,
            [&out_alloc, &reply_header_size](std::size_t size) {
        return out_alloc(size + reply_header_size) + reply_header_size;
    });
    auto* reply_buf = reply_return.payload;
    if(reply_buf) {
        reply_buf -= reply_header_size;
        const auto id = reply_return.opcode;
        const auto size = reply_return.size;
        populate_header(reply_buf, size, id, nid);
    }
    return reply_return.possible_exception;
}

std::exception_ptr RPCManager::handle_receive(
        char* buf, std::size_t size,
        const std::function<char*(int)>& out_alloc) {
    using namespace remote_invocation_utilities;
    std::size_t payload_size = size;
    Opcode indx;
    node_id_t received_from;
    retrieve_header(&dsm, buf, payload_size, indx, received_from);
    return handle_receive(indx, received_from, buf + header_space(),
            payload_size, out_alloc);
}

void RPCManager::rpc_message_handler(node_id_t sender_id, char* msg_buf, uint32_t payload_size) {
    // extract the destination vector
    size_t dest_size = ((size_t*)msg_buf)[0];
    msg_buf += sizeof(size_t);
    payload_size -= sizeof(size_t);
    bool in_dest = false;
    for(size_t i = 0; i < dest_size; ++i) {
        auto n = ((node_id_t*)msg_buf)[0];
        msg_buf += sizeof(node_id_t);
        payload_size -= sizeof(node_id_t);
        if(n == nid) {
            in_dest = true;
        }
    }
    if(in_dest || dest_size == 0) {
        auto max_payload_size = view_manager.curr_view->multicast_group->max_msg_size
                - sizeof(header);
        size_t reply_size = 0;
        handle_receive(
                msg_buf, payload_size, [this, &reply_size, &max_payload_size](size_t size) -> char* {
            reply_size = size;
            if(reply_size <= max_payload_size) {
                return replySendBuffer.get();
            } else {
                return nullptr;
            }
        });
        if(reply_size > 0) {
            node_id_t id = sender_id;
            if(id == nid) {
                handle_receive(
                        replySendBuffer.get(), reply_size,
                        [](size_t size) -> char* { assert(false); });
                if(dest_size == 0) {
                    std::lock_guard<std::mutex> lock(pending_results_mutex);
                    toFulfillQueue.front().get().fulfill_map(view_manager.curr_view->members);
                    fulfilledList.push_back(std::move(toFulfillQueue.front()));
                    toFulfillQueue.pop();
                }
            } else {
                connections.write(id, replySendBuffer.get(), reply_size);
            }
        }
    }
}

void RPCManager::p2p_message_handler(int32_t sender_id, char* msg_buf, uint32_t buffer_size) {
    using namespace remote_invocation_utilities;
    const std::size_t header_size = header_space();
    connections.read(sender_id, msg_buf, header_size);
    std::size_t payload_size;
    Opcode indx;
    node_id_t received_from;
    retrieve_header(nullptr, msg_buf, payload_size, indx, received_from);
    connections.read(sender_id, msg_buf + header_size, payload_size);
    size_t reply_size = 0;
    handle_receive(
            indx, received_from, msg_buf + header_size, payload_size,
            [&msg_buf, &buffer_size, &reply_size](size_t _size) -> char* {
        reply_size = _size;
        if(reply_size <= buffer_size) {
            return msg_buf;
        } else {
            return nullptr;
        }
    });
    if(reply_size > 0) {
        connections.write(received_from, msg_buf, reply_size);
    }
}

void RPCManager::new_view_callback(std::vector<node_id_t> new_members, std::vector<node_id_t> old_members) {
    std::vector<node_id_t> removed_members;
    std::vector<node_id_t> joined_members;
    std::set_difference(old_members.begin(), old_members.end(),
                        new_members.begin(), new_members.end(),
                        std::back_inserter(removed_members));
    std::set_difference(new_members.begin(), new_members.end(),
                        old_members.begin(), old_members.end(),
                        std::back_inserter(joined_members));
    for(const auto& removed_id : removed_members) {
        connections.delete_node(removed_id);
    }
    //HACK: By the time this is called, curr_view will have been installed, so we can
    //reach into it in order to get the IP addresses. Really, the new view callbacks
    //should just get the whole view.
    for(const auto& joiner_id : joined_members) {
        if(joiner_id != nid)
            connections.add_node(joiner_id,
                    view_manager.curr_view->member_ips[view_manager.curr_view->rank_of(joiner_id)]);
    }

    std::lock_guard<std::mutex> lock(pending_results_mutex);
    for(auto& pending : fulfilledList) {
        for(auto removed_id : removed_members) {
            pending.get().set_exception_for_removed_node(removed_id);
        }
    }
}

int RPCManager::populate_nodelist_header(const std::vector<node_id_t>& dest_nodes, char* buffer,
                                         std::size_t& max_payload_size) {
    int header_size = 0;
    // Put the list of destination nodes in another layer of "header"
    ((size_t*)buffer)[0] = dest_nodes.size();
    buffer += sizeof(size_t);
    header_size += sizeof(size_t);
    for(auto& n : dest_nodes) {
        ((node_id_t*)buffer)[0] = n;
        buffer += sizeof(node_id_t);
        header_size += sizeof(node_id_t);
    }
    //Two return values: the size of the header we just created,
    //and the maximum payload size based on that
    max_payload_size = view_manager.curr_view->multicast_group->max_msg_size
            - sizeof(derecho::header) - header_size;
    return header_size;
}

void RPCManager::finish_rpc_send(uint32_t subgroup_id, const std::vector<node_id_t>& dest_nodes, PendingBase& pending_results_handle) {
    while(!view_manager.curr_view->multicast_group->send(subgroup_id)) {
    }
    std::lock_guard<std::mutex> lock(pending_results_mutex);
    if(dest_nodes.size()) {
        pending_results_handle.fulfill_map(dest_nodes);
        fulfilledList.push_back(pending_results_handle);
    } else {
        toFulfillQueue.push(pending_results_handle);
    }
}

void RPCManager::finish_p2p_send(node_id_t dest_node, char* msg_buf, std::size_t size, PendingBase& pending_results_handle) {
    connections.write(dest_node, msg_buf, size);
    pending_results_handle.fulfill_map({dest_node});
    std::lock_guard<std::mutex> lock(pending_results_mutex);
    fulfilledList.push_back(pending_results_handle);
}


void RPCManager::rpc_process_loop() {
    auto max_payload_size = view_manager.curr_view->multicast_group->max_msg_size - sizeof(header);
    std::unique_ptr<char[]> rpcBuffer = std::unique_ptr<char[]>(new char[max_payload_size]);
    while(!thread_shutdown) {
        auto other_id = connections.probe_all();
        if(other_id < 0) {
            continue;
        }
        p2p_message_handler(other_id, rpcBuffer.get(), max_payload_size);
    }
}

}

}


