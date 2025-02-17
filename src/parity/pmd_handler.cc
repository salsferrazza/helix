#include "helix/parity/pmd_handler.hh"

#include "helix/compat/endian.h"
#include "helix/order_book.hh"

#include <unordered_map>
#include <stdexcept>
#include <cstring>
#include <cstdint>
#include <cassert>
#include <cstdio>
#include <memory>
#include <string>

using namespace helix::core;
using namespace std;

namespace helix {

namespace parity {

static side_type pmd_side(char c)
{
    switch (c) {
    case 'B': return side_type::buy;
    case 'S': return side_type::sell;
    default:  throw invalid_argument(string("invalid argument: ") + to_string(c));
    }
}

trade_sign pmd_trade_sign(side_type s)
{
    switch (s) {
    case side_type::buy:  return trade_sign::seller_initiated;
    case side_type::sell: return trade_sign::buyer_initiated;
    default:              throw invalid_argument(string("invalid argument"));
    }
}

pmd_handler::pmd_handler()
{
}

void pmd_handler::subscribe(std::string sym, size_t max_orders)
{
    helix::core::order_book ob{sym, 0, max_orders};
    ob.set_state(trading_state::trading);
    auto padding = PMD_INSTRUMENT_LEN - sym.size();
    if (padding > 0) {
        sym.insert(sym.size(), padding, ' ');
    }
    _symbols.insert(sym);
    _order_book_id_map.emplace(sym, std::move(ob));
}

void pmd_handler::register_callback(core::event_callback callback)
{
    _process_event = callback;
}

size_t pmd_handler::parse(const net::packet_view& packet)
{
    auto* msg = packet.cast<pmd_message>();
    switch (msg->MessageType) {
    case 'V': return process_msg<pmd_version>(packet);
    case 'S': return process_msg<pmd_second>(packet);
    case 'A': return process_msg<pmd_order_added>(packet);
    case 'E': return process_msg<pmd_order_executed>(packet);
    case 'X': return process_msg<pmd_order_canceled>(packet);
    case 'D': return process_msg<pmd_order_deleted>(packet);
    case 'B': return process_msg<pmd_broken_trade>(packet);
    default:  throw unknown_message_type("unknown type: " + std::string(1, msg->MessageType));
    }
}

template<typename T>
size_t pmd_handler::process_msg(const net::packet_view& packet)
{
    process_msg(packet.cast<T>());
    return sizeof(T);
}

void pmd_handler::process_msg(const pmd_version* m)
{
}

void pmd_handler::process_msg(const pmd_second* m)
{
    _seconds = be32toh(m->Second);
}

void pmd_handler::process_msg(const pmd_order_added* m)
{
    std::string symbol{m->Instrument, PMD_INSTRUMENT_LEN};
    auto it = _order_book_id_map.find(symbol);
    if (it != _order_book_id_map.end()) {
        auto& ob = it->second;
        uint64_t order_id  = be64toh(m->OrderNumber);
        uint64_t price     = be32toh(m->Price);
        uint32_t quantity  = be32toh(m->Quantity);
        auto     side      = pmd_side(m->Side);
        uint64_t timestamp = to_timestamp(be32toh(m->Timestamp));
        order o{order_id, price, quantity, side, timestamp};
        ob.add(std::move(o));
        ob.set_timestamp(timestamp);
        _order_id_map.insert({order_id, ob});
        _process_event(make_ob_event(timestamp, &ob));
    }
}

void pmd_handler::process_msg(const pmd_order_executed* m)
{
    uint64_t order_id = be64toh(m->OrderNumber);
    auto it = _order_id_map.find(order_id);
    if (it != _order_id_map.end()) {
        auto& ob = it->second;
        uint32_t quantity  = be32toh(m->Quantity);
        uint64_t timestamp = to_timestamp(be32toh(m->Timestamp));
        auto result = ob.execute(order_id, quantity);
        ob.set_timestamp(timestamp);
        trade t{ob.symbol(), timestamp, result.price, quantity, pmd_trade_sign(result.side)};
        _process_event(make_event(timestamp, &ob, &t, sweep_event(result)));
    }
}

void pmd_handler::process_msg(const pmd_order_canceled* m)
{
    uint64_t order_id = be64toh(m->OrderNumber);
    auto it = _order_id_map.find(order_id);
    if (it != _order_id_map.end()) {
        auto& ob = it->second;
        uint32_t quantity  = be32toh(m->CanceledQuantity);
        uint64_t timestamp = to_timestamp(be32toh(m->Timestamp));
        ob.cancel(order_id, quantity);
        ob.set_timestamp(timestamp);
        _process_event(make_ob_event(timestamp, &ob));
    }
}

void pmd_handler::process_msg(const pmd_order_deleted* m)
{
    uint64_t order_id = be64toh(m->OrderNumber);
    auto it = _order_id_map.find(order_id);
    if (it != _order_id_map.end()) {
        auto& ob = it->second;
        uint64_t timestamp = to_timestamp(be32toh(m->Timestamp));
        ob.remove(order_id);
        ob.set_timestamp(timestamp);
        _process_event(make_ob_event(timestamp, &ob));
    }
}

void pmd_handler::process_msg(const pmd_broken_trade* m)
{
}

event_mask pmd_handler::sweep_event(const execution& e) const
{
    if (e.remaining > 0) {
        return 0;
    }
    return ev_sweep;
}

uint64_t pmd_handler::to_timestamp(uint64_t nanoseconds) const
{
    return _seconds * 1000 + nanoseconds / 1000000;
}

}

}
