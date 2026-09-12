#include "fsx/journal.hpp"
#include "fsx/crc.hpp"
#include "fsx/endian.hpp"
#include "fsx/file_util.hpp"
#include "fsx/uuid.hpp"
#include <algorithm>
#include <array>
#include <filesystem>
#include <limits>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace fsx {
namespace {
constexpr std::array<char, 8> HEADER_MAGIC{'F','S','X','J','N','L','1','0'};
constexpr std::uint32_t HEADER_VERSION = 1;
constexpr std::uint32_t RECORD_MAGIC = 0x52585346U;
constexpr std::uint16_t RECORD_VERSION = 1;

void copy_bytes(std::span<std::byte> dst, std::size_t off, std::span<const std::byte> src) {
    std::copy(src.begin(), src.end(), dst.begin() + static_cast<std::ptrdiff_t>(off));
}

std::array<std::byte, 4096> encode_header(const JournalIdentity& id,
                                          std::uint64_t sequence,
                                          OperationPhase phase,
                                          std::uint64_t record_count) {
    std::array<std::byte, 4096> b{};
    for (std::size_t i = 0; i < HEADER_MAGIC.size(); ++i) {
        b[i] = std::byte(static_cast<unsigned char>(HEADER_MAGIC[i]));
    }
    write_le32(b, 8, HEADER_VERSION);
    write_le32(b, 12, 4096);
    write_le64(b, 16, sequence);
    copy_bytes(b, 24, id.operation_uuid);
    copy_bytes(b, 40, id.filesystem_uuid);
    write_le64(b, 56, id.device_size);
    write_le64(b, 64, id.target_size);
    write_le32(b, 72, static_cast<std::uint32_t>(phase));
    write_le64(b, 80, record_count);
    write_le64(b, 88, TransactionJournal::record_bytes);
    write_le64(b, 96, TransactionJournal::records_offset);
    write_le32(b, 4092, 0);
    write_le32(b, 4092, crc32c(std::span<const std::byte>(b).first(4092))^0xffffffffU);
    return b;
}

struct DecodedHeader {
    JournalIdentity id{};
    std::uint64_t sequence{};
    OperationPhase phase{OperationPhase::prepared};
    std::uint64_t record_count{};
};

Result<DecodedHeader> decode_header(std::span<const std::byte> b) {
    if (b.size() < 4096) {
        return Error{Errc::corrupt, 0, "journal header is truncated"};
    }
    for (std::size_t i = 0; i < HEADER_MAGIC.size(); ++i) {
        if (std::to_integer<unsigned char>(b[i]) != static_cast<unsigned char>(HEADER_MAGIC[i])) {
            return Error{Errc::corrupt, 0, "journal header magic mismatch"};
        }
    }
    if (read_le<std::uint32_t>(b, 8) != HEADER_VERSION || read_le<std::uint32_t>(b, 12) != 4096) {
        return Error{Errc::unsupported, 0, "unsupported journal header version"};
    }
    const auto stored = read_le<std::uint32_t>(b, 4092);
    std::array<std::byte, 4096> tmp{};
    std::copy_n(b.begin(), 4096, tmp.begin());
    write_le32(tmp, 4092, 0);
    if (stored != (crc32c(std::span<const std::byte>(tmp).first(4092)) ^ 0xffffffffU)) {
        return Error{Errc::corrupt, 0, "journal header checksum mismatch"};
    }
    DecodedHeader h;
    h.sequence = read_le<std::uint64_t>(b, 16);
    std::copy_n(b.begin()+24, 16, h.id.operation_uuid.begin());
    std::copy_n(b.begin()+40, 16, h.id.filesystem_uuid.begin());
    h.id.device_size = read_le<std::uint64_t>(b, 56);
    h.id.target_size = read_le<std::uint64_t>(b, 64);
    const auto ph = read_le<std::uint32_t>(b, 72);
    if (ph < 1 || ph > 11) {
        return Error{Errc::corrupt, 0, "invalid journal phase"};
    }
    h.phase = static_cast<OperationPhase>(ph);
    h.record_count = read_le<std::uint64_t>(b, 80);
    if (read_le<std::uint64_t>(b, 88) != TransactionJournal::record_bytes
        || read_le<std::uint64_t>(b, 96) != TransactionJournal::records_offset) {
        return Error{Errc::unsupported, 0, "unsupported journal record layout"};
    }
    return h;
}

std::array<std::byte, 128> encode_record(const JournalRecord& r) {
    std::array<std::byte, 128> b{};
    write_le32(b, 0, RECORD_MAGIC);
    write_le16(b, 4, RECORD_VERSION);
    write_le16(b, 6, static_cast<std::uint16_t>(r.type));
    write_le64(b, 8, r.sequence);
    write_le64(b, 16, r.transaction_id);
    write_le64(b, 24, r.source_block);
    write_le64(b, 32, r.destination_block);
    write_le64(b, 40, r.block_count);
    write_le64(b, 48, r.inode);
    write_le64(b, 56, r.aux);
    write_le32(b, 64, r.data_crc32c);
    write_le32(b, 68, r.state);
    write_le32(b, 124, 0);
    write_le32(b, 124, crc32c(std::span<const std::byte>(b).first(124))^0xffffffffU);
    return b;
}

Result<JournalRecord> decode_record(std::span<const std::byte> b) {
    if (b.size()<128 || read_le<std::uint32_t>(b, 0) != RECORD_MAGIC
        || read_le<std::uint16_t>(b, 4) != RECORD_VERSION) return Error{
        Errc::corrupt, 0,"journal record header invalid"
    }
    ;
    const auto stored = read_le<std::uint32_t>(b, 124);
    std::array<std::byte, 128> tmp{};
    std::copy_n(b.begin(), 128, tmp.begin());
    write_le32(tmp, 124, 0);
    if (stored != (crc32c(std::span<const std::byte>(tmp).first(124))^0xffffffffU)) return Error{
        Errc::corrupt, 0,"journal record checksum mismatch"
    }
    ;
    const auto rt = read_le<std::uint16_t>(b, 6);
    if (rt<1 || rt>12)return Error{
        Errc::corrupt, 0,"journal record type invalid"
    }
    ;
    JournalRecord r;
    r.type = static_cast<JournalRecordType>(rt);
    r.sequence = read_le<std::uint64_t>(b, 8);
    r.transaction_id = read_le<std::uint64_t>(b, 16);
    r.source_block = read_le<std::uint64_t>(b, 24);
    r.destination_block = read_le<std::uint64_t>(b, 32);
    r.block_count = read_le<std::uint64_t>(b, 40);
    r.inode = read_le<std::uint64_t>(b, 48);
    r.aux = read_le<std::uint64_t>(b, 56);
    r.data_crc32c = read_le<std::uint32_t>(b, 64);
    r.state = read_le<std::uint32_t>(b, 68);
    return r;
}
}

const char* operation_phase_name(OperationPhase p) noexcept {
    switch (p) {
    case OperationPhase::prepared:return "prepared";
    case OperationPhase::relocating:return "relocating";
    case OperationPhase::paused:return "paused";
    case OperationPhase::metadata_finalize:return "metadata_finalize";
    case OperationPhase::filesystem_shrunk:return "filesystem_shrunk";
    case OperationPhase::partition_pending:return "partition_pending";
    case OperationPhase::complete:return "complete";
    case OperationPhase::failed:return "failed";
    case OperationPhase::staged:return "staged";
    case OperationPhase::metadata_paused:return "metadata_paused";
    case OperationPhase::compacted:return "compacted";
    }
    return "unknown";
}
const char* journal_record_type_name(JournalRecordType t) noexcept {
    switch (t) {
    case JournalRecordType::relocation_intent:return "relocation_intent";
    case JournalRecordType::copy_verified:return "copy_verified";
    case JournalRecordType::metadata_committed:return "metadata_committed";
    case JournalRecordType::checkpoint:return "checkpoint";
    case JournalRecordType::note:return "note";
    case JournalRecordType::destination_reserved:return "destination_reserved";
    case JournalRecordType::owner_switched:return "owner_switched";
    case JournalRecordType::source_released:return "source_released";
    case JournalRecordType::filesystem_marked_dirty:return "filesystem_marked_dirty";
    case JournalRecordType::filesystem_marked_clean:return "filesystem_marked_clean";
    case JournalRecordType::partition_intent:return "partition_intent";
    case JournalRecordType::partition_committed:return "partition_committed";
    }
    return "unknown";
}

std::array<std::byte, 16> TransactionJournal::random_uuid() {
    return random_uuid_v4();
}

Result<TransactionJournal> TransactionJournal::create(std::string path,
                                                      const JournalIdentity& id, std::uint64_t cap) {
    if (cap<records_offset+record_bytes*16) return Error{
        Errc::invalid_argument, 0,"journal capacity is too small"
    }
    ;
    std::error_code ec;
    if (std::filesystem::exists(path, ec) && !ec)return Error{
        Errc::unsafe, 0,"refusing to overwrite existing transaction journal"
    }
    ;
    if (ec)return Error{Errc::open_failed, 0,"cannot inspect transaction journal path: "+ec.message()};
    const auto temp_uuid = random_uuid_v4();
    const auto nonce = read_le<std::uint64_t>(temp_uuid, 0);
    const std::string temp = path+".part."+std::to_string(static_cast<long long>(::getpid()))+"."+std::to_string(nonce);
    struct Cleanup{
        std::string path;
        bool keep{
            false
        }
        ;
        ~Cleanup() {
            if (!keep)::unlink(path.c_str());
        }
    }
    cleanup{
        temp
    }
    ;
    auto dr = BlockDevice::create_file(temp, cap);
    if (!dr)return dr.error();
    TransactionJournal j;
    j.device_ = std::move(dr.value());
    j.writable_ = true;
    j.status_.identity = id;
    j.status_.phase = OperationPhase::prepared;
    j.status_.header_sequence = 1;
    j.status_.records_valid = true;
    auto a = j.write_header(0, 1, OperationPhase::prepared, 0);
    if (!a)return a.error();
    auto b = j.write_header(1, 0, OperationPhase::prepared, 0);
    if (!b)return b.error();
    auto f = j.device_.flush();
    if (!f)return f.error();
    auto pub = publish_file_noreplace(temp, path,"transaction journal");
    if (!pub)return pub.error();
    cleanup.keep = true;
    auto reopened = BlockDevice::open_write(path);
    if (!reopened)return reopened.error();
    j.device_ = std::move(reopened.value());
    j.status_.header_a_valid = j.status_.header_b_valid = true;
    return j;
}
Result<TransactionJournal> TransactionJournal::open(std::string path, bool writable) {
    auto dr = writable?BlockDevice::open_write(path):BlockDevice::open_read(path);
    if (!dr)return dr.error();
    TransactionJournal j;
    j.device_ = std::move(dr.value());
    j.writable_ = writable;
    auto r = j.load();
    if (!r)return r.error();
    return j;
}
Result<void> TransactionJournal::write_header(std::uint64_t slot,
                                              std::uint64_t seq, OperationPhase ph, std::uint64_t count) {
    if (!writable_)return Error{
        Errc::permission, 0,"journal is read-only"
    }
    ;
    auto b = encode_header(status_.identity, seq, ph, count);
    return device_.write_exact(slot*header_bytes, b);
}
Result<void> TransactionJournal::load() {
    std::array<std::byte, 4096>a{
    }
    , b{};
    auto ra = device_.read_exact(0, a);
    if (!ra)return ra.error();
    auto rb = device_.read_exact(header_bytes, b);
    if (!rb)return rb.error();
    auto da = decode_header(a), db = decode_header(b);
    status_.header_a_valid = static_cast<bool>(da);
    status_.header_b_valid = static_cast<bool>(db);
    if (!da && !db)return Error{
        Errc::corrupt, 0,"both journal headers are invalid"
    }
    ;
    DecodedHeader h;
    if (da && db) {
        if (da.value().id.operation_uuid != db.value().id.operation_uuid
            || da.value().id.filesystem_uuid != db.value().id.filesystem_uuid
            || da.value().id.device_size != db.value().id.device_size
            || da.value().id.target_size != db.value().id.target_size)
            return Error{Errc::corrupt, 0,"journal headers describe different operations"};
        h = da.value().sequence >= db.value().sequence?da.value():db.value();
    }
    else h = da?da.value():db.value();
    status_.identity = h.id;
    status_.phase = h.phase;
    status_.header_sequence = h.sequence;
    status_.record_count = h.record_count;
    const auto max = (device_.geometry().size_bytes>records_offset)
        ?(device_.geometry().size_bytes-records_offset)/record_bytes:0;
    if (status_.record_count>max)return Error{
        Errc::corrupt, 0,"journal record count exceeds journal capacity"
    }
    ;
    status_.records_valid = true;
    for (std::uint64_t i = 0; i<status_.record_count; ++i) {
        std::array<std::byte, 128>rec{};
        auto rr = device_.read_exact(records_offset+i*record_bytes, rec);
        if (!rr)return rr.error();
        auto d = decode_record(rec);
        if (!d) {
            status_.records_valid = false;
            return d.error();
        }
        if (d.value().sequence != i+1) {
            status_.records_valid = false;
            return Error{
                Errc::corrupt, 0,"journal record sequence is not contiguous"
            }
            ;
        }
        status_.last_transaction_id = std::max(status_.last_transaction_id, d.value().transaction_id);
        if (d.value().type == JournalRecordType::metadata_committed)++status_.committed_transactions;
    }
    return {};
}
Result<void> TransactionJournal::append(JournalRecord r) {
    return append_batch(std::vector<JournalRecord>{r});
}
Result<void> TransactionJournal::append_batch(const std::vector<JournalRecord>& records) {
    if (!writable_)return Error{Errc::permission, 0,"journal is read-only"};
    if (records.empty())return {};
    const auto capacity_records = device_.geometry().size_bytes > records_offset
        ? (device_.geometry().size_bytes - records_offset) / record_bytes : 0;
    const auto add = static_cast<std::uint64_t>(records.size());
    if (status_.record_count > capacity_records || add > capacity_records - status_.record_count)
        return Error{Errc::io, 0,"journal is full"};
    const auto new_count = status_.record_count+add;
    for (std::size_t i = 0; i<records.size(); ++i) {
        auto r = records[i];
        r.sequence = status_.record_count+static_cast<std::uint64_t>(i)+1;
        auto enc = encode_record(r);
        const auto record_offset = records_offset
            + (status_.record_count + static_cast<std::uint64_t>(i)) * record_bytes;
        auto wr = device_.write_exact(record_offset, enc);
        if (!wr)return wr.error();
    }
    auto fl = device_.flush();
    if (!fl)return fl.error();
    const auto seq = status_.header_sequence+1;
    auto wh = write_header(seq&1ULL, seq, status_.phase, new_count);
    if (!wh)return wh.error();
    fl = device_.flush();
    if (!fl)return fl.error();
    status_.header_sequence = seq;
    status_.record_count = new_count;
    for (const auto&r:records) {
        status_.last_transaction_id = std::max(status_.last_transaction_id, r.transaction_id);
        if (r.type == JournalRecordType::metadata_committed)++status_.committed_transactions;
    }
    return {};
}
Result<void> TransactionJournal::checkpoint(OperationPhase ph) {
    if (!writable_)return Error{
        Errc::permission, 0,"journal is read-only"
    }
    ;
    JournalRecord r;
    r.type = JournalRecordType::checkpoint;
    r.transaction_id = status_.last_transaction_id;
    r.aux = static_cast<std::uint64_t>(ph);
    auto ar = append(r);
    if (!ar)return ar.error();
    const auto seq = status_.header_sequence+1;
    auto wh = write_header(seq&1ULL, seq, ph, status_.record_count);
    if (!wh)return wh.error();
    auto fl = device_.flush();
    if (!fl)return fl.error();
    status_.header_sequence = seq;
    status_.phase = ph;
    return {};
}
Result<void> TransactionJournal::flush() {
    return device_.flush();
}
Result<std::vector<JournalRecord>> TransactionJournal::records()const{
    if (status_.record_count>static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))return Error{
        Errc::unsupported, 0,"journal record count exceeds host address space"
    }
    ;
    std::vector<JournalRecord>out;
    out.reserve(static_cast<std::size_t>(status_.record_count));
    for (std::uint64_t i = 0; i<status_.record_count; ++i) {
        std::array<std::byte, 128>rec{};
        auto rr = device_.read_exact(records_offset+i*record_bytes, rec);
        if (!rr)return rr.error();
        auto d = decode_record(rec);
        if (!d)return d.error();
        out.push_back(d.value());
    }
    return out;
}
}
