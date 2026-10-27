#include "Reliable.h"
#include <algorithm>

namespace rts::net {

void writePacketHeader(ByteWriter& w, PacketKind kind, uint32_t token) {
    w.u16(kPacketMagic); w.u8(uint8_t(kind)); w.u32(token);
}

bool readPacketHeader(ByteReader& r, PacketKind& kind, uint32_t& token) {
    if (r.u16() != kPacketMagic) return false;
    uint8_t k = r.u8();
    token = r.u32();
    if (!r.ok() || k > uint8_t(PacketKind::Disconnect)) return false;
    kind = PacketKind(k);
    return true;
}

namespace {
constexpr size_t kHeader = 7;              // magic + kind + token
constexpr size_t kDataHeader = 4 + 1;      // ack + count
constexpr size_t kMsgHeader = 4 + 2;       // seq + len|more
constexpr size_t kFragment = kMaxDatagram - kHeader - kDataHeader - kMsgHeader;
constexpr uint16_t kMoreBit = 0x8000;
constexpr uint64_t kKeepaliveMs = 250;
constexpr size_t kMaxInFlight = 2048;      // messages; beyond that the link is considered dead
constexpr size_t kMaxEarly = 4096;
} // namespace

void ReliableChannel::queue(const Bytes& msg) {
    if (failed_) return;
    size_t off = 0;
    do {
        size_t n = std::min(kFragment, msg.size() - off);
        Out o;
        o.seq = nextSeq_++;
        o.data.assign(msg.begin() + long(off), msg.begin() + long(off + n));
        off += n;
        o.more = off < msg.size();
        out_.push_back(std::move(o));
    } while (off < msg.size());
    if (out_.size() > kMaxInFlight * 8) failed_ = true; // peer stopped acking: avoid unbounded memory
}

bool ReliableChannel::onData(ByteReader& r, uint64_t now) {
    lastHeard_ = now;
    uint32_t ack = r.u32();
    uint8_t count = r.u8();
    if (!r.ok()) return false;
    // Cumulative ack: drop everything below it; take an RTT sample from a never-resent message.
    while (!out_.empty() && int32_t(out_.front().seq - ack) < 0) {
        const Out& o = out_.front();
        if (o.sends == 1 && o.firstSent) {
            uint32_t sample = uint32_t(std::min<uint64_t>(now - o.firstSent, 10000));
            uint32_t diff = sample > srtt_ ? sample - srtt_ : srtt_ - sample;
            rttvar_ = (rttvar_ * 3 + diff) / 4;
            srtt_ = (srtt_ * 7 + sample) / 8;
        }
        out_.pop_front();
    }
    for (uint8_t i = 0; i < count; ++i) {
        uint32_t seq = r.u32();
        uint16_t lenFlags = r.u16();
        uint16_t len = lenFlags & ~kMoreBit;
        if (!r.ok() || len > kFragment) return false;
        Bytes data(len);
        for (uint16_t k = 0; k < len; ++k) data[k] = r.u8();
        if (!r.ok()) return false;
        ackDirty_ = true;
        if (int32_t(seq - expect_) < 0) continue;              // duplicate
        if (early_.size() < kMaxEarly) early_.emplace(seq, std::make_pair(std::move(data), (lenFlags & kMoreBit) != 0));
    }
    for (auto it = early_.find(expect_); it != early_.end(); it = early_.find(expect_)) {
        partial_.insert(partial_.end(), it->second.first.begin(), it->second.first.end());
        bool more = it->second.second;
        early_.erase(it);
        ++expect_;
        if (partial_.size() > kMaxMessageSize) { failed_ = true; return false; }
        if (!more) { in_.push_back(std::move(partial_)); partial_.clear(); }
    }
    return true;
}

bool ReliableChannel::recv(Bytes& msg) {
    if (in_.empty()) return false;
    msg = std::move(in_.front());
    in_.pop_front();
    return true;
}

void ReliableChannel::flush(uint64_t now, uint32_t token, const std::function<void(const Bytes&)>& out) {
    const uint32_t rto = std::clamp<uint32_t>(srtt_ + 4 * rttvar_, 40, 1000);
    size_t i = 0;
    int packets = 0;
    do {
        ByteWriter w;
        writePacketHeader(w, PacketKind::Data, token);
        w.u32(expect_);
        const size_t countAt = w.buf.size();
        w.u8(0);
        uint8_t count = 0;
        size_t inFlight = 0;
        for (; i < out_.size() && inFlight < kMaxInFlight; ++i, ++inFlight) {
            Out& o = out_[i];
            if (o.sends && now - o.lastSent < rto) continue;
            if (w.buf.size() + kMsgHeader + o.data.size() > kMaxDatagram || count == 255) break;
            w.u32(o.seq);
            w.u16(uint16_t(o.data.size()) | (o.more ? kMoreBit : 0));
            w.buf.insert(w.buf.end(), o.data.begin(), o.data.end());
            if (!o.sends) o.firstSent = now;
            o.lastSent = now;
            ++o.sends;
            ++count;
        }
        w.buf[countAt] = count;
        if (count == 0 && !ackDirty_ && now - lastSend_ < kKeepaliveMs) break;
        out(w.buf);
        ackDirty_ = false;
        lastSend_ = now;
        if (count == 0) break;
    } while (i < out_.size() && ++packets < 64);
}

} // namespace rts::net
