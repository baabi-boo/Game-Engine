#include <ge/persistence/journal.hpp>

#include <ge/core/binary.hpp>
#include <ge/core/runtime.hpp>
#include <ge/io/filesystem.hpp>
#include <ge/world/state.hpp>

#include <algorithm>
#include <charconv>
#include <limits>
#include <map>
#include <new>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace ge::persistence {
namespace {

constexpr std::size_t log_header_bytes = 64;
constexpr std::size_t checkpoint_header_bytes = 64;
constexpr std::size_t record_header_bytes = 44;
constexpr std::size_t record_overhead = record_header_bytes + 4;
constexpr std::size_t manifest_entry_bytes = 40;
constexpr std::uint64_t maximum_id = std::numeric_limits<std::uint64_t>::max();
using Values = std::map<std::string, JournalValue, std::less<>>;
enum class RecordKind : std::uint8_t { Begin = 1, Put, Remove, Commit, Abort };

struct Fault final : std::runtime_error {
    Fault(JournalError error, std::string message, std::size_t position = 0)
        : std::runtime_error(std::move(message)), code(error), offset(position) {}
    JournalError code;
    std::size_t offset;
};

[[noreturn]] void fail(JournalError error, std::string message, std::size_t offset = 0) {
    throw Fault(error, std::move(message), offset);
}

JournalResult failure(JournalError error, std::string message, std::size_t offset = 0) {
    return {error, offset, std::move(message)};
}

template<class Function>
JournalResult protect(Function&& function) {
    try {
        function();
        return {};
    } catch (const Fault& error) {
        return failure(error.code, error.what(), error.offset);
    } catch (const io::IoException& error) {
        auto code = JournalError::IoFailure;
        if (error.code() == io::IoError::not_found) { code = JournalError::NotFound; }
        if (error.code() == io::IoError::limit_exceeded) { code = JournalError::Limit; }
        if (error.code() == io::IoError::already_exists) { code = JournalError::Conflict; }
        if (error.code() == io::IoError::invalid_path) { code = JournalError::InvalidArgument; }
        return failure(code, error.path() + ": " + error.what());
    } catch (const core::BinaryException& error) {
        auto code = JournalError::Corrupt;
        if (error.error() == core::BinaryError::truncated) { code = JournalError::Truncated; }
        if (error.error() == core::BinaryError::limit_exceeded) { code = JournalError::Limit; }
        return failure(code, error.what(), error.offset());
    } catch (const std::bad_alloc&) {
        return failure(JournalError::AllocationFailure, "journal allocation failed");
    } catch (const std::length_error& error) {
        return failure(JournalError::Limit, error.what());
    } catch (const std::exception& error) {
        return failure(JournalError::InvalidArgument, error.what());
    }
}

void require_result(const JournalResult& result) {
    if (!result) {
        fail(result.error, result.message, result.offset);
    }
}

std::size_t sum(std::size_t left, std::size_t right, std::size_t maximum) {
    std::size_t result{};
    if (!core::checked_add(left, right, result) || result > maximum) {
        fail(JournalError::Limit, "journal byte budget exceeded");
    }
    return result;
}

void validate_limits(const JournalLimits& limits) {
    constexpr std::size_t hard_bytes = 1024U * 1024U * 1024U;
    if (limits.max_journal_bytes < 512 || limits.max_journal_bytes > hard_bytes ||
        limits.max_checkpoint_bytes < 512 || limits.max_checkpoint_bytes > hard_bytes ||
        limits.max_value_bytes == 0 || limits.max_value_bytes > limits.max_state_bytes ||
        limits.max_state_bytes > limits.max_checkpoint_bytes ||
        limits.max_transaction_bytes == 0 || limits.max_transaction_bytes > hard_bytes ||
        limits.max_entries == 0 || limits.max_entries > 1000000 ||
        limits.max_operations == 0 || limits.max_operations > 1000000 ||
        limits.max_records < 2 || limits.max_records > 10000000 ||
        limits.max_key_bytes == 0 || limits.max_key_bytes > 4096 ||
        limits.max_label_bytes > 4096 || limits.max_slots == 0 || limits.max_slots > 65536 ||
        limits.retained_generations < 2 || limits.retained_generations > 32 ||
        limits.generation_probes == 0 || limits.generation_probes > 65536) {
        throw std::invalid_argument("invalid save journal limits");
    }
}

void validate_text(std::string_view text, std::size_t maximum, bool empty = false) {
    if ((!empty && text.empty()) || text.size() > maximum || !core::valid_utf8(text)) {
        fail(JournalError::InvalidArgument, "invalid or oversized journal text");
    }
    for (const char character : text) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte < 32U || byte == 127U) {
            fail(JournalError::InvalidArgument, "journal text contains a control character");
        }
    }
}

void validate_slot(std::string_view slot) {
    if (slot.empty() || slot.size() > 64 || slot == "." || slot == "..") {
        fail(JournalError::InvalidArgument, "save slot must contain 1 to 64 portable characters");
    }
    for (const char character : slot) {
        if (!((character >= 'a' && character <= 'z') ||
              (character >= 'A' && character <= 'Z') ||
              (character >= '0' && character <= '9') ||
              character == '_' || character == '-')) {
            fail(JournalError::InvalidArgument, "save slot contains an unsupported character");
        }
    }
}

core::BinaryLimits binary_limits(const JournalLimits& limits, std::size_t maximum) {
    core::BinaryLimits result;
    result.max_total_bytes = maximum;
    result.max_string_bytes = std::max(limits.max_key_bytes, limits.max_label_bytes);
    result.max_chunk_bytes = maximum;
    result.max_frame_bytes = maximum;
    return result;
}

void version(core::ByteReader& reader) {
    if (reader.read_u16() != journal_format_version || reader.read_u16() != 0) {
        fail(JournalError::UnsupportedVersion, "unsupported journal format version");
    }
}

void finish_crc(core::ByteWriter& writer) {
    const auto checksum = core::crc32(writer.bytes());
    writer.write_u32(checksum);
}

void check_crc(std::span<const std::byte> bytes) {
    if (bytes.size() < 4) {
        fail(JournalError::Truncated, "missing file checksum");
    }
    core::ByteReader trailer(bytes.last(4));
    if (trailer.read_u32() != core::crc32(bytes.first(bytes.size() - 4))) {
        fail(JournalError::Corrupt, "file checksum mismatch");
    }
}

std::size_t measure(const Values& values, const JournalLimits& limits) {
    if (values.size() > limits.max_entries) {
        fail(JournalError::Limit, "committed entry limit exceeded");
    }
    std::size_t total{};
    std::size_t encoded = checkpoint_header_bytes + 4;
    for (const auto& [key, value] : values) {
        validate_text(key, limits.max_key_bytes);
        if (value.version == 0 || value.bytes.size() > limits.max_value_bytes) {
            fail(JournalError::Limit, "invalid value version or oversized value");
        }
        total = sum(total, key.size(), limits.max_state_bytes);
        total = sum(total, value.bytes.size(), limits.max_state_bytes);
        encoded = sum(encoded, 24, limits.max_checkpoint_bytes);
        encoded = sum(encoded, key.size(), limits.max_checkpoint_bytes);
        encoded = sum(encoded, value.bytes.size(), limits.max_checkpoint_bytes);
    }
    return total;
}

void write_value(core::ByteWriter& writer, std::string_view key, const JournalValue& value,
                 const JournalLimits& limits) {
    writer.write_string(key, limits.max_key_bytes);
    writer.write_u32(value.type);
    writer.write_u16(value.version);
    writer.write_u16(0);
    writer.write_u64(static_cast<std::uint64_t>(value.bytes.size()));
    writer.write_u32(core::crc32(value.bytes));
    writer.write_bytes(value.bytes);
}

std::pair<std::string, JournalValue> read_value(core::ByteReader& reader,
                                               const JournalLimits& limits) {
    auto key = reader.read_string();
    validate_text(key, limits.max_key_bytes);
    JournalValue value;
    value.type = reader.read_u32();
    value.version = reader.read_u16();
    if (value.version == 0 || reader.read_u16() != 0) {
        fail(JournalError::Corrupt, "invalid value envelope");
    }
    const auto length = reader.read_u64();
    const auto checksum = reader.read_u32();
    if (length > limits.max_value_bytes) {
        fail(JournalError::Limit, "value payload exceeds configured bytes");
    }
    if (length > reader.remaining()) {
        fail(JournalError::Truncated, "value payload exceeds remaining bytes");
    }
    const auto bytes = reader.read_bytes(static_cast<std::size_t>(length));
    if (core::crc32(bytes) != checksum) {
        fail(JournalError::Corrupt, "value payload checksum mismatch");
    }
    value.bytes.assign(bytes.begin(), bytes.end());
    return {std::move(key), std::move(value)};
}

std::uint64_t state_hash(const Values& values, const JournalLimits& limits) {
    core::Fnv1a64 digest;
    core::ByteWriter header(limits.max_key_bytes + 32);
    header.write_u64(static_cast<std::uint64_t>(values.size()));
    digest.update(header.bytes());
    for (const auto& [key, value] : values) {
        header.clear();
        header.write_string(key, limits.max_key_bytes);
        header.write_u32(value.type);
        header.write_u16(value.version);
        header.write_u64(static_cast<std::uint64_t>(value.bytes.size()));
        digest.update(header.bytes());
        digest.update(value.bytes);
    }
    return digest.value();
}

struct Generation {
    std::uint64_t id{};
    std::uint64_t sequence{};
    std::uint64_t committed{};
    TransactionId high_transaction{};
    std::uint64_t checkpoint_hash{};
    bool operator==(const Generation&) const = default;
};

std::size_t manifest_limit(const JournalLimits& limits) {
    return 20 + manifest_entry_bytes * limits.retained_generations;
}

std::vector<std::byte> encode_manifest(const std::vector<Generation>& generations,
                                      const JournalLimits& limits) {
    core::ByteWriter writer(manifest_limit(limits));
    writer.write_bytes("GEJMAN01");
    writer.write_u16(journal_format_version);
    writer.write_u16(0);
    writer.write_u32(static_cast<std::uint32_t>(generations.size()));
    for (const auto& generation : generations) {
        writer.write_u64(generation.id);
        writer.write_u64(generation.sequence);
        writer.write_u64(generation.committed);
        writer.write_u64(generation.high_transaction);
        writer.write_u64(generation.checkpoint_hash);
    }
    finish_crc(writer);
    return writer.take();
}

std::vector<Generation> decode_manifest(std::span<const std::byte> bytes,
                                        const JournalLimits& limits) {
    check_crc(bytes);
    core::ByteReader reader(bytes.first(bytes.size() - 4), core::ByteOrder::little,
                            binary_limits(limits, manifest_limit(limits)));
    reader.expect("GEJMAN01");
    version(reader);
    const auto count = reader.read_u32();
    if (count == 0 || count > limits.retained_generations ||
        static_cast<std::size_t>(count) != reader.remaining() / manifest_entry_bytes) {
        fail(JournalError::Corrupt, "invalid manifest generation count");
    }
    std::vector<Generation> result;
    result.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        Generation generation;
        generation.id = reader.read_u64();
        generation.sequence = reader.read_u64();
        generation.committed = reader.read_u64();
        generation.high_transaction = reader.read_u64();
        generation.checkpoint_hash = reader.read_u64();
        if (generation.id == 0 || generation.committed > generation.sequence ||
            generation.high_transaction > generation.sequence ||
            (!result.empty() && generation.id >= result.back().id)) {
            fail(JournalError::Corrupt, "unordered or invalid generation manifest");
        }
        result.push_back(generation);
    }
    reader.require_end();
    return result;
}

std::vector<std::byte> encode_checkpoint(const Generation& generation,
                                        const Values& values, const JournalLimits& limits) {
    const auto state_bytes = measure(values, limits);
    core::ByteWriter writer(limits.max_checkpoint_bytes);
    writer.write_bytes("GEJCHK01");
    writer.write_u16(journal_format_version);
    writer.write_u16(0);
    writer.write_u32(static_cast<std::uint32_t>(checkpoint_header_bytes));
    writer.write_u64(generation.id);
    writer.write_u64(generation.sequence);
    writer.write_u64(generation.committed);
    writer.write_u64(generation.high_transaction);
    writer.write_u32(static_cast<std::uint32_t>(values.size()));
    writer.write_u32(0);
    writer.write_u64(static_cast<std::uint64_t>(state_bytes));
    for (const auto& [key, value] : values) {
        write_value(writer, key, value, limits);
    }
    finish_crc(writer);
    return writer.take();
}

Values decode_checkpoint(std::span<const std::byte> bytes, const Generation& expected,
                          const JournalLimits& limits) {
    if (core::fnv1a64(bytes) != expected.checkpoint_hash) {
        fail(JournalError::Corrupt, "checkpoint does not match its manifest");
    }
    check_crc(bytes);
    core::ByteReader reader(bytes.first(bytes.size() - 4), core::ByteOrder::little,
                            binary_limits(limits, limits.max_checkpoint_bytes));
    reader.expect("GEJCHK01");
    version(reader);
    if (reader.read_u32() != checkpoint_header_bytes || reader.read_u64() != expected.id ||
        reader.read_u64() != expected.sequence || reader.read_u64() != expected.committed ||
        reader.read_u64() != expected.high_transaction) {
        fail(JournalError::Corrupt, "checkpoint identity mismatch");
    }
    const auto count = reader.read_u32();
    const auto reserved = reader.read_u32();
    const auto bytes_declared = reader.read_u64();
    if (reserved != 0 || count > limits.max_entries || (count != 0 && expected.committed == 0) ||
        static_cast<std::size_t>(count) > reader.remaining() / 25 ||
        bytes_declared > limits.max_state_bytes) {
        fail(JournalError::Limit, "checkpoint entry count or storage is invalid");
    }
    Values result;
    std::string previous;
    std::size_t used{};
    for (std::uint32_t index = 0; index < count; ++index) {
        auto [key, value] = read_value(reader, limits);
        if (!previous.empty() && key <= previous) {
            fail(JournalError::Corrupt, "checkpoint keys are not canonical");
        }
        used = sum(used, key.size(), limits.max_state_bytes);
        used = sum(used, value.bytes.size(), limits.max_state_bytes);
        previous = key;
        result.emplace(std::move(key), std::move(value));
    }
    reader.require_end();
    if (used != bytes_declared) {
        fail(JournalError::Corrupt, "checkpoint aggregate size mismatch");
    }
    return result;
}

std::vector<std::byte> encode_log_header(const Generation& generation) {
    core::ByteWriter writer(log_header_bytes);
    writer.write_bytes("GEJLOG01");
    writer.write_u16(journal_format_version);
    writer.write_u16(0);
    writer.write_u32(static_cast<std::uint32_t>(log_header_bytes));
    writer.write_u64(generation.id);
    writer.write_u64(generation.sequence);
    writer.write_u64(generation.committed);
    writer.write_u64(generation.high_transaction);
    writer.write_u64(generation.checkpoint_hash);
    writer.write_u32(0);
    finish_crc(writer);
    return writer.take();
}

std::string generation_path(std::string_view directory, std::uint64_t generation,
                             bool checkpoint) {
    const auto name = "g" + std::to_string(generation) + (checkpoint ? ".checkpoint" : ".journal");
    return io::join_virtual_path(directory, name);
}

struct Pending {
    TransactionId id{};
    Values candidate;
    std::size_t operations{};
    std::size_t bytes{};
    core::Fnv1a64 digest;
};

struct State {
    io::VirtualFileSystem* files{};
    std::string root;
    std::string slot;
    std::string directory;
    std::string manifest_path;
    std::string log_path;
    std::vector<Generation> generations;
    std::vector<std::byte> manifest;
    Generation generation;
    Values values;
    std::vector<std::byte> log;
    std::unique_ptr<Pending> pending;
    std::uint64_t sequence{};
    std::uint64_t committed{};
    TransactionId high_transaction{};
    std::uint32_t tail_checksum{};
    std::size_t records{};
    bool repair{};
    bool memory{};
    RecoveryReport recovery;
};

void validate_io(const io::VirtualFileSystem& files, const JournalLimits& limits) {
    const auto maximum = std::max(limits.max_journal_bytes, limits.max_checkpoint_bytes);
    if (maximum > files.limits().max_read_bytes || maximum > files.limits().max_write_bytes ||
        maximum > files.limits().max_file_bytes) {
        fail(JournalError::Limit, "journal limits exceed the supplied VFS transfer limits");
    }
}

std::optional<std::uint64_t> current_manifest(State& state, const JournalLimits& limits) {
    const auto metadata = state.files->metadata(state.manifest_path);
    if (state.manifest.empty()) {
        if (metadata) {
            fail(JournalError::Conflict, "save manifest appeared during creation");
        }
        return std::nullopt;
    }
    if (!metadata || state.files->read(state.manifest_path, manifest_limit(limits)) != state.manifest) {
        fail(JournalError::Conflict, "save manifest changed; reopen before publication");
    }
    return io::metadata_cache_key(*metadata);
}

std::uint64_t current_log(State& state, const JournalLimits& limits) {
    const auto metadata = state.files->metadata(state.log_path);
    if (!metadata || state.files->read(state.log_path, limits.max_journal_bytes) != state.log) {
        fail(JournalError::Conflict, "journal changed; recover before publication");
    }
    return io::metadata_cache_key(*metadata);
}

struct Append {
    std::vector<std::byte> frame;
    std::vector<std::byte> log;
    std::uint32_t checksum{};
};

Append prepare_append(const State& state, RecordKind kind, TransactionId transaction,
                       std::span<const std::byte> payload, const JournalLimits& limits) {
    if (state.repair) {
        fail(JournalError::RequiresCheckpoint, "recovered save requires an explicit checkpoint");
    }
    if (state.sequence == maximum_id || state.records >= limits.max_records) {
        fail(JournalError::Exhausted, "journal sequence or record capacity exhausted");
    }
    const auto length = sum(record_overhead, payload.size(), limits.max_journal_bytes);
    const auto total = sum(state.log.size(), length, limits.max_journal_bytes);
    if (kind == RecordKind::Begin || kind == RecordKind::Put || kind == RecordKind::Remove) {
        (void)sum(total, record_overhead + 20, limits.max_journal_bytes);
        if (state.records + 2 > limits.max_records || state.sequence >= maximum_id - 1) {
            fail(JournalError::Exhausted, "transaction must retain space for commit or abort");
        }
    }
    core::ByteWriter writer(length);
    writer.write_bytes("JREC");
    writer.write_u32(static_cast<std::uint32_t>(length));
    writer.write_u16(journal_format_version);
    writer.write_u8(static_cast<std::uint8_t>(kind));
    writer.write_u8(0);
    writer.write_u64(state.sequence + 1);
    writer.write_u64(transaction);
    writer.write_u32(static_cast<std::uint32_t>(payload.size()));
    writer.write_u32(core::crc32(payload));
    writer.write_u32(state.tail_checksum);
    finish_crc(writer);
    writer.write_bytes(payload);
    const auto checksum = core::crc32(writer.bytes());
    writer.write_u32(checksum);
    Append result;
    result.checksum = checksum;
    result.frame = writer.take();
    result.log.reserve(total);
    result.log.insert(result.log.end(), state.log.begin(), state.log.end());
    result.log.insert(result.log.end(), result.frame.begin(), result.frame.end());
    return result;
}

void publish_append(State& state, Append& append, const JournalLimits& limits) {
    (void)current_manifest(state, limits);
    const auto expected = current_log(state, limits);
    state.files->write(state.log_path, append.log, {true, false, expected});
    state.log.swap(append.log);
    state.tail_checksum = append.checksum;
    ++state.sequence;
    ++state.records;
}

void require_transaction(const State& state, TransactionId transaction) {
    if (!state.pending) {
        fail(JournalError::NoTransaction, "no journal transaction is active");
    }
    if (transaction == 0 || transaction != state.pending->id) {
        fail(JournalError::WrongTransaction, "transaction does not own the active draft");
    }
}

void operation_budget(const Pending& pending, std::size_t bytes, const JournalLimits& limits) {
    if (pending.operations >= limits.max_operations) {
        fail(JournalError::Limit, "transaction operation limit exceeded");
    }
    (void)sum(pending.bytes, bytes, limits.max_transaction_bytes);
}

std::size_t replay_record(State& state, std::span<const std::byte> remaining,
                           const JournalLimits& limits, RecoveryReport& report) {
    if (remaining.size() < record_header_bytes) {
        fail(JournalError::Truncated, "incomplete journal record header");
    }
    check_crc(remaining.first(record_header_bytes));
    core::ByteReader header(remaining.first(record_header_bytes));
    header.expect("JREC");
    const auto length = header.read_u32();
    if (header.read_u16() != journal_format_version) {
        fail(JournalError::UnsupportedVersion, "unsupported record version");
    }
    const auto kind = static_cast<RecordKind>(header.read_u8());
    const auto flags = header.read_u8();
    const auto sequence = header.read_u64();
    const auto transaction = header.read_u64();
    const auto payload_bytes = header.read_u32();
    const auto payload_checksum = header.read_u32();
    const auto preceding = header.read_u32();
    (void)header.read_u32();
    if (flags != 0 || length < record_overhead ||
        static_cast<std::size_t>(length) - record_overhead != payload_bytes ||
        length > limits.max_journal_bytes) {
        fail(JournalError::Corrupt, "invalid record envelope");
    }
    if (length > remaining.size()) {
        fail(JournalError::Truncated, "incomplete journal record payload or trailer");
    }
    if (state.sequence == maximum_id || sequence != state.sequence + 1 || transaction == 0 ||
        transaction > sequence ||
        preceding != state.tail_checksum || state.records >= limits.max_records) {
        fail(JournalError::Corrupt, "record sequence, chain, transaction, or count is invalid");
    }
    const auto frame = remaining.first(length);
    check_crc(frame);
    const auto payload = frame.subspan(record_header_bytes, payload_bytes);
    if (core::crc32(payload) != payload_checksum) {
        fail(JournalError::Corrupt, "record payload checksum mismatch");
    }
    core::ByteReader reader(payload, core::ByteOrder::little,
                            binary_limits(limits, limits.max_journal_bytes));
    switch (kind) {
    case RecordKind::Begin: {
        if (state.pending || transaction <= state.high_transaction) {
            fail(JournalError::Corrupt, "nested or reused transaction identifier");
        }
        validate_text(reader.read_string_view(), limits.max_label_bytes, true);
        reader.require_end();
        auto pending = std::make_unique<Pending>();
        pending->id = transaction;
        pending->candidate = state.values;
        state.pending = std::move(pending);
        state.high_transaction = transaction;
        break;
    }
    case RecordKind::Put: {
        require_transaction(state, transaction);
        operation_budget(*state.pending, payload.size(), limits);
        auto [key, value] = read_value(reader, limits);
        reader.require_end();
        state.pending->candidate.insert_or_assign(std::move(key), std::move(value));
        (void)measure(state.pending->candidate, limits);
        ++state.pending->operations;
        state.pending->bytes += payload.size();
        state.pending->digest.update(frame);
        break;
    }
    case RecordKind::Remove: {
        require_transaction(state, transaction);
        operation_budget(*state.pending, payload.size(), limits);
        const auto key = reader.read_string();
        validate_text(key, limits.max_key_bytes);
        reader.require_end();
        state.pending->candidate.erase(key);
        ++state.pending->operations;
        state.pending->bytes += payload.size();
        state.pending->digest.update(frame);
        break;
    }
    case RecordKind::Commit: {
        require_transaction(state, transaction);
        const auto count = reader.read_u32();
        const auto operations_hash = reader.read_u64();
        const auto resulting_hash = reader.read_u64();
        reader.require_end();
        if (count != state.pending->operations || operations_hash != state.pending->digest.value() ||
            resulting_hash != state_hash(state.pending->candidate, limits)) {
            fail(JournalError::Corrupt, "commit operation digest or resulting state mismatch");
        }
        state.values.swap(state.pending->candidate);
        state.pending.reset();
        state.committed = sequence;
        ++report.committed_transactions;
        break;
    }
    case RecordKind::Abort:
        require_transaction(state, transaction);
        reader.require_end();
        state.pending.reset();
        ++report.aborted_transactions;
        break;
    default:
        fail(JournalError::UnsupportedVersion, "unknown journal record kind");
    }
    core::ByteReader trailer(frame.last(4));
    state.tail_checksum = trailer.read_u32();
    state.sequence = sequence;
    ++state.records;
    return length;
}

RecoveryReport replay(State& state, const JournalLimits& limits, RecoveryPolicy policy) {
    if (state.log.size() < log_header_bytes) {
        fail(JournalError::Truncated, "journal header is incomplete");
    }
    const auto expected_header = encode_log_header(state.generation);
    if (!std::equal(expected_header.begin(), expected_header.end(), state.log.begin())) {
        fail(JournalError::Corrupt, "journal header does not match checkpoint generation");
    }
    state.sequence = state.generation.sequence;
    state.committed = state.generation.committed;
    state.high_transaction = state.generation.high_transaction;
    RecoveryReport report;
    report.generation = state.generation.id;
    std::size_t offset = log_header_bytes;
    while (offset < state.log.size()) {
        std::size_t consumed{};
        auto result = protect([&] {
            consumed = replay_record(state, std::span(state.log).subspan(offset), limits, report);
        });
        if (!result) {
            if (policy != RecoveryPolicy::CommittedPrefix ||
                result.error == JournalError::AllocationFailure || result.error == JournalError::IoFailure) {
                fail(result.error, result.message, offset);
            }
            report.suffix_error = result.error;
            report.suffix_message = std::move(result.message);
            report.failure_offset = offset;
            report.discarded_bytes = state.log.size() - offset;
            report.requires_checkpoint = true;
            break;
        }
        offset += consumed;
    }
    if (state.pending) {
        report.discarded_transactions = 1;
        report.requires_checkpoint = true;
        state.pending.reset();
    }
    report.accepted_bytes = offset;
    report.records = state.records;
    report.last_sequence = state.sequence;
    report.committed_sequence = state.committed;
    state.repair = report.requires_checkpoint;
    return report;
}

bool memory_backing(io::VirtualFileSystem& files, std::string_view path) {
    const auto metadata = files.metadata(path);
    if (!metadata) {
        fail(JournalError::IoFailure, "published generation is not visible through its mount");
    }
    for (const auto& mount : files.mounts()) {
        if (mount.id == metadata->mount) {
            return mount.kind == io::MountKind::memory;
        }
    }
    fail(JournalError::Conflict, "generation mount disappeared");
}

std::optional<std::uint64_t> owned_generation(std::string_view filename) {
    if (filename.size() < 3 || filename.front() != 'g') {
        return std::nullopt;
    }
    const auto dot = filename.find('.');
    if (dot == std::string_view::npos ||
        (filename.substr(dot) != ".checkpoint" && filename.substr(dot) != ".journal")) {
        return std::nullopt;
    }
    const auto digits = filename.substr(1, dot - 1);
    if (digits.empty() || digits.front() == '0') {
        return std::nullopt;
    }
    std::uint64_t value{};
    const auto result = std::from_chars(digits.data(), digits.data() + digits.size(), value);
    if (result.ec != std::errc{} || result.ptr != digits.data() + digits.size() || value == 0) {
        return std::nullopt;
    }
    return value;
}

void publish_checkpoint(State& state, const JournalLimits& limits) {
    if (state.pending) {
        fail(JournalError::TransactionActive, "commit or abort before checkpointing");
    }
    (void)current_manifest(state, limits);
    if (!state.log.empty()) {
        (void)current_log(state, limits);
    }
    Generation generation;
    generation.id = state.generations.empty() ? 0 : state.generations.front().id;
    generation.sequence = state.sequence;
    generation.committed = state.committed;
    generation.high_transaction = state.high_transaction;
    std::string checkpoint_path;
    std::string log_path;
    bool found{};
    for (std::size_t attempt = 0; attempt < limits.generation_probes; ++attempt) {
        if (generation.id == maximum_id) {
            fail(JournalError::Exhausted, "checkpoint generation space exhausted");
        }
        ++generation.id;
        checkpoint_path = generation_path(state.directory, generation.id, true);
        log_path = generation_path(state.directory, generation.id, false);
        if (!state.files->exists(checkpoint_path) && !state.files->exists(log_path)) {
            found = true;
            break;
        }
    }
    if (!found) {
        fail(JournalError::Exhausted, "too many orphan generations; inspect and prune first");
    }
    const auto checkpoint = encode_checkpoint(generation, state.values, limits);
    generation.checkpoint_hash = core::fnv1a64(checkpoint);
    auto log = encode_log_header(generation);
    std::vector<Generation> generations;
    generations.reserve(limits.retained_generations);
    generations.push_back(generation);
    for (const auto& previous : state.generations) {
        if (generations.size() == limits.retained_generations) {
            break;
        }
        if (previous.id <= state.generation.id) {
            generations.push_back(previous);
        }
    }
    auto manifest = encode_manifest(generations, limits);
    state.files->write(checkpoint_path, checkpoint, {false, true, std::nullopt});
    state.files->write(log_path, log, {false, true, std::nullopt});
    if (state.files->read(checkpoint_path, limits.max_checkpoint_bytes) != checkpoint ||
        state.files->read(log_path, limits.max_journal_bytes) != log) {
        fail(JournalError::IoFailure, "new generation readback differs; old manifest preserved");
    }
    const auto memory = memory_backing(*state.files, checkpoint_path) ||
        memory_backing(*state.files, log_path);
    if (!state.log.empty()) {
        (void)current_log(state, limits);
    }
    const auto expected = current_manifest(state, limits);
    state.files->write(state.manifest_path, manifest, {!state.manifest.empty(), true, expected});
    state.generations.swap(generations);
    state.manifest.swap(manifest);
    state.log.swap(log);
    state.log_path.swap(log_path);
    state.generation = generation;
    state.tail_checksum = 0;
    state.records = 0;
    state.repair = false;
    state.memory = memory;
}

std::unique_ptr<State> load_slot(io::VirtualFileSystem& files, std::string_view root,
                                  std::string_view slot, bool create,
                                  RecoveryPolicy policy, const JournalLimits& limits) {
    validate_slot(slot);
    validate_io(files, limits);
    if (policy != RecoveryPolicy::Strict && policy != RecoveryPolicy::CommittedPrefix &&
        policy != RecoveryPolicy::PreviousGeneration) {
        fail(JournalError::InvalidArgument, "invalid journal recovery policy");
    }
    auto state = std::make_unique<State>();
    state->files = &files;
    state->root = io::normalize_virtual_path(root);
    state->slot = slot;
    state->directory = io::join_virtual_path(state->root, slot);
    state->manifest_path = io::join_virtual_path(state->directory, "manifest.bin");
    if (!files.exists(state->manifest_path)) {
        if (!create) {
            fail(JournalError::NotFound, "save slot manifest does not exist");
        }
        if (files.exists(state->directory)) {
            for (const auto& entry : files.list(state->directory, {false, false, 0})) {
                if (owned_generation(io::virtual_filename(entry.path))) {
                    fail(JournalError::Conflict, "unselected generation files exist; refusing implicit creation");
                }
            }
        }
        publish_checkpoint(*state, limits);
        state->recovery.generation = state->generation.id;
        state->recovery.accepted_bytes = log_header_bytes;
        return state;
    }
    state->manifest = files.read(state->manifest_path, manifest_limit(limits));
    state->generations = decode_manifest(state->manifest, limits);
    JournalResult first_error;
    for (std::size_t index = 0; index < state->generations.size(); ++index) {
        state->generation = state->generations[index];
        state->pending.reset();
        state->records = 0;
        state->tail_checksum = 0;
        auto result = protect([&] {
            const auto checkpoint_path = generation_path(state->directory, state->generation.id, true);
            const auto checkpoint = files.read(checkpoint_path, limits.max_checkpoint_bytes);
            state->values = decode_checkpoint(checkpoint, state->generation, limits);
            state->log_path = generation_path(state->directory, state->generation.id, false);
            state->log = files.read(state->log_path, limits.max_journal_bytes);
            state->recovery = replay(*state, limits, policy);
            state->memory = memory_backing(files, checkpoint_path) ||
                memory_backing(files, state->log_path) || memory_backing(files, state->manifest_path);
        });
        if (result) {
            state->recovery.used_previous_generation = index != 0;
            state->repair = state->repair || index != 0;
            state->recovery.requires_checkpoint = state->repair;
            if (index != 0) {
                state->recovery.suffix_error = first_error.error;
                state->recovery.suffix_message = "newest generation rejected: " + first_error.message;
                state->recovery.failure_offset = first_error.offset;
            }
            return state;
        }
        if (index == 0) {
            first_error = result;
        }
        if (policy != RecoveryPolicy::PreviousGeneration || result.error == JournalError::AllocationFailure) {
            require_result(result);
        }
    }
    require_result(first_error);
    fail(JournalError::Corrupt, "no usable manifest generation");
}

} // namespace

struct SaveJournal::Impl {
    explicit Impl(JournalLimits settings) : limits(settings) { validate_limits(limits); }
    State& opened() {
        if (!state) {
            fail(JournalError::NotOpen, "open a save slot first");
        }
        return *state;
    }
    JournalLimits limits;
    std::unique_ptr<State> state;
    RecoveryReport empty_recovery;
};

std::string_view describe(JournalError error) noexcept {
    switch (error) {
    case JournalError::None: return "success";
    case JournalError::InvalidArgument: return "invalid argument";
    case JournalError::Limit: return "limit exceeded";
    case JournalError::NotOpen: return "journal not open";
    case JournalError::NotFound: return "save not found";
    case JournalError::TransactionActive: return "transaction active";
    case JournalError::NoTransaction: return "no transaction";
    case JournalError::WrongTransaction: return "wrong transaction";
    case JournalError::Conflict: return "save conflict";
    case JournalError::Corrupt: return "corrupt save";
    case JournalError::UnsupportedVersion: return "unsupported version";
    case JournalError::Truncated: return "truncated save";
    case JournalError::IoFailure: return "I/O failure";
    case JournalError::AllocationFailure: return "allocation failure";
    case JournalError::RequiresCheckpoint: return "checkpoint required";
    case JournalError::DocumentFailure: return "scene document failure";
    case JournalError::StateFailure: return "world state failure";
    case JournalError::Exhausted: return "identifier or capacity exhausted";
    }
    return "unknown journal error";
}

SaveJournal::SaveJournal(JournalLimits limits) : impl_(std::make_unique<Impl>(limits)) {}
SaveJournal::~SaveJournal() = default;
const JournalLimits& SaveJournal::limits() const noexcept { return impl_->limits; }
const RecoveryReport& SaveJournal::last_recovery() const noexcept {
    return impl_->state ? impl_->state->recovery : impl_->empty_recovery;
}

JournalStatus SaveJournal::inspect() const {
    JournalStatus result;
    if (!impl_->state) {
        return result;
    }
    const auto& state = *impl_->state;
    result.open = true;
    result.requires_checkpoint = state.repair;
    result.memory_backed = state.memory;
    result.root = state.root;
    result.slot = state.slot;
    result.generation = state.generation.id;
    result.last_sequence = state.sequence;
    result.committed_sequence = state.committed;
    result.active_transaction = state.pending ? state.pending->id : 0;
    result.entries = state.values.size();
    result.state_bytes = measure(state.values, impl_->limits);
    result.journal_bytes = state.log.size();
    result.records = state.records;
    result.pending_operations = state.pending ? state.pending->operations : 0;
    result.retained_generations = state.generations.size();
    return result;
}

std::vector<JournalEntryInfo> SaveJournal::entries() const {
    std::vector<JournalEntryInfo> result;
    if (impl_->state) {
        result.reserve(impl_->state->values.size());
        for (const auto& [key, value] : impl_->state->values) {
            result.push_back({key, value.type, value.version, value.bytes.size(), core::crc32(value.bytes)});
        }
    }
    return result;
}

const JournalValue* SaveJournal::find(std::string_view key) const noexcept {
    if (!impl_->state) {
        return nullptr;
    }
    const auto found = impl_->state->values.find(key);
    return found == impl_->state->values.end() ? nullptr : &found->second;
}

JournalResult SaveJournal::open(io::VirtualFileSystem& files, std::string_view root,
                                std::string_view slot, bool create_missing,
                                RecoveryPolicy policy, RecoveryReport* report) {
    auto result = protect([&] {
        if (impl_->state && impl_->state->pending) {
            fail(JournalError::TransactionActive, "abort or commit before opening another save");
        }
        auto candidate = load_slot(files, root, slot, create_missing, policy, impl_->limits);
        if (report) {
            *report = candidate->recovery;
        }
        impl_->state.swap(candidate);
    });
    if (!result && report) {
        *report = {};
        report->status = result;
        report->failure_offset = result.offset;
    }
    return result;
}

JournalResult SaveJournal::recover(RecoveryPolicy policy, RecoveryReport& report) {
    const auto result = protect([&] {
        auto& state = impl_->opened();
        auto candidate = load_slot(*state.files, state.root, state.slot, false, policy, impl_->limits);
        report = candidate->recovery;
        impl_->state.swap(candidate);
    });
    if (!result) {
        report = {};
        report.status = result;
        report.failure_offset = result.offset;
    }
    return result;
}

JournalResult SaveJournal::close() {
    return protect([&] {
        if (impl_->state && impl_->state->pending) {
            fail(JournalError::TransactionActive, "abort or commit before closing the journal");
        }
        impl_->state.reset();
    });
}

JournalResult SaveJournal::begin(TransactionId& transaction, std::string_view label) {
    return protect([&] {
        auto& state = impl_->opened();
        if (state.pending) {
            fail(JournalError::TransactionActive, "only one journal transaction may be active");
        }
        if (state.high_transaction == maximum_id) {
            fail(JournalError::Exhausted, "transaction identifier space exhausted");
        }
        validate_text(label, impl_->limits.max_label_bytes, true);
        auto pending = std::make_unique<Pending>();
        pending->id = state.high_transaction + 1;
        pending->candidate = state.values;
        core::ByteWriter payload(impl_->limits.max_label_bytes + 10);
        payload.write_string(label, impl_->limits.max_label_bytes);
        auto append = prepare_append(state, RecordKind::Begin, pending->id, payload.bytes(), impl_->limits);
        publish_append(state, append, impl_->limits);
        state.high_transaction = pending->id;
        transaction = pending->id;
        state.pending = std::move(pending);
    });
}

JournalResult SaveJournal::put(TransactionId transaction, std::string_view key,
                               std::span<const std::byte> bytes, std::uint32_t type,
                               std::uint16_t value_version) {
    return protect([&] {
        auto& state = impl_->opened();
        require_transaction(state, transaction);
        validate_text(key, impl_->limits.max_key_bytes);
        if (value_version == 0 || bytes.size() > impl_->limits.max_value_bytes) {
            fail(JournalError::Limit, "value version or byte count is invalid");
        }
        const auto envelope = sum(24, key.size(), impl_->limits.max_journal_bytes);
        const auto payload_size = sum(envelope, bytes.size(), impl_->limits.max_journal_bytes);
        operation_budget(*state.pending, payload_size, impl_->limits);
        JournalValue value{type, value_version, {bytes.begin(), bytes.end()}};
        core::ByteWriter payload(impl_->limits.max_journal_bytes);
        write_value(payload, key, value, impl_->limits);
        operation_budget(*state.pending, payload.size(), impl_->limits);
        auto candidate = state.pending->candidate;
        candidate.insert_or_assign(std::string(key), std::move(value));
        (void)measure(candidate, impl_->limits);
        auto append = prepare_append(state, RecordKind::Put, transaction, payload.bytes(), impl_->limits);
        publish_append(state, append, impl_->limits);
        state.pending->candidate.swap(candidate);
        state.pending->digest.update(append.frame);
        ++state.pending->operations;
        state.pending->bytes += payload.size();
    });
}

JournalResult SaveJournal::remove(TransactionId transaction, std::string_view key) {
    return protect([&] {
        auto& state = impl_->opened();
        require_transaction(state, transaction);
        validate_text(key, impl_->limits.max_key_bytes);
        core::ByteWriter payload(impl_->limits.max_key_bytes + 10);
        payload.write_string(key, impl_->limits.max_key_bytes);
        operation_budget(*state.pending, payload.size(), impl_->limits);
        auto candidate = state.pending->candidate;
        candidate.erase(std::string(key));
        auto append = prepare_append(state, RecordKind::Remove, transaction, payload.bytes(), impl_->limits);
        publish_append(state, append, impl_->limits);
        state.pending->candidate.swap(candidate);
        state.pending->digest.update(append.frame);
        ++state.pending->operations;
        state.pending->bytes += payload.size();
    });
}

JournalResult SaveJournal::commit(TransactionId transaction) {
    return protect([&] {
        auto& state = impl_->opened();
        require_transaction(state, transaction);
        core::ByteWriter payload(20);
        payload.write_u32(static_cast<std::uint32_t>(state.pending->operations));
        payload.write_u64(state.pending->digest.value());
        payload.write_u64(state_hash(state.pending->candidate, impl_->limits));
        auto append = prepare_append(state, RecordKind::Commit, transaction, payload.bytes(), impl_->limits);
        publish_append(state, append, impl_->limits);
        state.values.swap(state.pending->candidate);
        state.pending.reset();
        state.committed = state.sequence;
    });
}

JournalResult SaveJournal::abort(TransactionId transaction) {
    return protect([&] {
        auto& state = impl_->opened();
        require_transaction(state, transaction);
        auto append = prepare_append(state, RecordKind::Abort, transaction, {}, impl_->limits);
        publish_append(state, append, impl_->limits);
        state.pending.reset();
    });
}

JournalResult SaveJournal::checkpoint() {
    return protect([&] { publish_checkpoint(impl_->opened(), impl_->limits); });
}

JournalResult SaveJournal::prune(RetentionReport& report) {
    report = {};
    return protect([&] {
        auto& state = impl_->opened();
        (void)current_manifest(state, impl_->limits);
        (void)current_log(state, impl_->limits);
        const auto files = state.files->list(state.directory, {false, false, 0});
        for (const auto& file : files) {
            const auto generation = owned_generation(io::virtual_filename(file.path));
            if (!generation) {
                continue;
            }
            const bool retained = std::any_of(state.generations.begin(), state.generations.end(),
                [&](const Generation& candidate) { return candidate.id == *generation; });
            if (retained) {
                ++report.retained_files;
                continue;
            }
            const auto result = protect([&] {
                if (state.files->remove(file.path)) {
                    ++report.removed_files;
                }
            });
            if (!result) {
                ++report.failed_files;
                if (report.first_failure.empty()) {
                    report.first_failure = result.message;
                }
            }
        }
        if (report.failed_files != 0) {
            fail(JournalError::IoFailure, "some obsolete generation files could not be removed");
        }
    });
}

JournalResult SaveJournal::list_slots(io::VirtualFileSystem& files, std::string_view root,
                                      std::vector<SaveSlotInfo>& destination) const {
    return protect([&] {
        const auto directory = io::normalize_virtual_path(root);
        std::vector<SaveSlotInfo> result;
        if (files.exists(directory)) {
            const auto entries = files.list(directory, {false, true, 0});
            for (const auto& entry : entries) {
                if (entry.kind != io::FileKind::directory) {
                    continue;
                }
                SaveSlotInfo slot;
                slot.name = io::virtual_filename(entry.path);
                const auto valid = protect([&] { validate_slot(slot.name); });
                if (!valid) {
                    continue;
                }
                const auto manifest_path = io::join_virtual_path(entry.path, "manifest.bin");
                if (!files.exists(manifest_path)) {
                    continue;
                }
                if (result.size() >= impl_->limits.max_slots) {
                    fail(JournalError::Limit, "save slot listing limit exceeded");
                }
                slot.status = protect([&] {
                    const auto bytes = files.read(manifest_path, manifest_limit(impl_->limits));
                    const auto generations = decode_manifest(bytes, impl_->limits);
                    slot.generation = generations.front().id;
                    slot.retained_generations = generations.size();
                });
                result.push_back(std::move(slot));
            }
        }
        std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
            return left.name < right.name;
        });
        destination.swap(result);
    });
}

JournalResult SaveJournal::save_document(std::string_view key,
                                         const scene::SceneDocumentStore& document) {
    return protect([&] {
        (void)impl_->opened();
        validate_text(key, impl_->limits.max_key_bytes);
        std::vector<std::byte> bytes;
        const auto encoded = document.encode(bytes);
        if (!encoded) {
            fail(JournalError::DocumentFailure, encoded.message);
        }
        if (bytes.size() > impl_->limits.max_value_bytes) {
            fail(JournalError::Limit, "scene document exceeds the journal value limit");
        }
        TransactionId transaction{};
        require_result(begin(transaction, "scene document"));
        require_result(put(transaction, key, bytes, scene_value_type, 1));
        require_result(commit(transaction));
    });
}

JournalResult SaveJournal::capture_scene(std::string_view key, scene::Scene& source,
                                         scene::SceneArchiveLimits limits) {
    return protect([&] {
        (void)impl_->opened();
        validate_text(key, impl_->limits.max_key_bytes);
        limits.max_archive_bytes = std::min(limits.max_archive_bytes, impl_->limits.max_value_bytes);
        limits.max_section_bytes = std::min(limits.max_section_bytes, limits.max_archive_bytes);
        scene::SceneDocumentStore document(limits);
        const auto captured = document.capture(source);
        if (!captured) {
            fail(JournalError::DocumentFailure, captured.message);
        }
        require_result(save_document(key, document));
    });
}

JournalResult SaveJournal::load_document(std::string_view key,
                                         scene::SceneDocumentStore& destination,
                                         scene::SceneDecodeOptions options) const {
    return protect([&] {
        (void)impl_->opened();
        const auto* value = find(key);
        if (!value) {
            fail(JournalError::NotFound, "committed scene document key was not found");
        }
        if (value->type != scene_value_type || value->version != 1) {
            fail(JournalError::UnsupportedVersion, "value is not a supported GESCENE1 document");
        }
        const auto decoded = destination.decode(value->bytes, options);
        if (!decoded) {
            fail(JournalError::DocumentFailure, decoded.message);
        }
    });
}

JournalResult SaveJournal::put_state(TransactionId transaction, std::string_view key,
                                     const world::StateSnapshot& snapshot) {
    return protect([&] {
        require_transaction(impl_->opened(), transaction);
        std::vector<std::uint8_t> bytes;
        try {
            bytes = world::encode_state(snapshot);
        } catch (const std::bad_alloc&) {
            throw;
        } catch (const std::exception& error) {
            fail(JournalError::StateFailure, error.what());
        }
        require_result(put(transaction, key, std::as_bytes(std::span(bytes)), state_value_type, 1));
    });
}

JournalResult SaveJournal::restore_state(std::string_view key,
                                         world::PersistentState& destination) const {
    return protect([&] {
        (void)impl_->opened();
        const auto* value = find(key);
        if (!value) {
            fail(JournalError::NotFound, "committed world state key was not found");
        }
        if (value->type != state_value_type || value->version != 1) {
            fail(JournalError::UnsupportedVersion, "value is not a supported GESTATE1 snapshot");
        }
        if (value->bytes.size() > world::max_state_file_bytes) {
            fail(JournalError::Limit, "stored world state exceeds GESTATE1 limits");
        }
        std::vector<std::uint8_t> bytes;
        bytes.reserve(value->bytes.size());
        for (const auto byte : value->bytes) {
            bytes.push_back(std::to_integer<std::uint8_t>(byte));
        }
        try {
            destination.restore(bytes);
        } catch (const std::bad_alloc&) {
            throw;
        } catch (const std::exception& error) {
            fail(JournalError::StateFailure, error.what());
        }
    });
}

namespace {

core::CommandResult command_result(const JournalResult& result, std::string success) {
    return {static_cast<bool>(result), result ? std::move(success)
        : std::string(describe(result.error)) + ": " + result.message};
}

TransactionId parse_transaction(std::string_view text) {
    TransactionId result{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || result == 0) {
        throw std::invalid_argument("transaction must be a nonzero unsigned integer");
    }
    return result;
}

RecoveryPolicy parse_policy(std::string_view text) {
    if (text == "strict") { return RecoveryPolicy::Strict; }
    if (text == "prefix") { return RecoveryPolicy::CommittedPrefix; }
    if (text == "previous") { return RecoveryPolicy::PreviousGeneration; }
    throw std::invalid_argument("recovery policy must be strict, prefix, or previous");
}

JournalResult open_command(core::Kernel& host, std::string_view slot, bool create,
                            RecoveryPolicy policy = RecoveryPolicy::Strict,
                            RecoveryReport* report = nullptr) {
    auto* files = host.services().find<io::VirtualFileSystem>();
    if (!files) {
        return failure(JournalError::IoFailure, "VFS service is not registered");
    }
    return host.services().get<SaveJournal>().open(*files,
        host.config().text("persistence.root", "/user/saves"), slot, create, policy, report);
}

std::string recovery_summary(const RecoveryReport& report) {
    std::ostringstream text;
    text << "generation=" << report.generation << " committed_sequence=" << report.committed_sequence
         << " accepted_bytes=" << report.accepted_bytes << " discarded_bytes=" << report.discarded_bytes
         << " unfinished_transactions=" << report.discarded_transactions
         << " previous_generation=" << report.used_previous_generation
         << " checkpoint_required=" << report.requires_checkpoint;
    if (report.suffix_error != JournalError::None) {
        text << " issue_at=" << report.failure_offset << " issue=" << report.suffix_message;
    }
    return text.str();
}

} // namespace

void register_journal(core::Kernel& kernel) {
    kernel.services().emplace<SaveJournal>();
    (void)kernel.register_module({"persistence.journal", "Transactional VFS saves and committed-only recovery", 1});
    (void)kernel.add_command("journal.open", [](core::Kernel& host, core::CommandArgs args) {
        if (args.empty() || args.size() > 2 || (args.size() == 2 && args[1] != "create")) {
            return core::CommandResult{false, "usage: journal.open slot [create]"};
        }
        RecoveryReport report;
        const auto result = open_command(host, args[0], args.size() == 2, RecoveryPolicy::Strict, &report);
        return command_result(result, recovery_summary(report));
    }, "Open a slot under persistence.root; create requires an empty slot namespace");
    (void)kernel.add_command("journal.begin", [](core::Kernel& host, core::CommandArgs args) {
        if (args.size() > 1) {
            return core::CommandResult{false, "usage: journal.begin [label]"};
        }
        TransactionId transaction{};
        const auto result = host.services().get<SaveJournal>().begin(transaction,
            args.empty() ? std::string_view{} : std::string_view(args[0]));
        return command_result(result, "transaction=" + std::to_string(transaction));
    }, "Persist a begin record and create an isolated private draft");
    (void)kernel.add_command("journal.put", [](core::Kernel& host, core::CommandArgs args) {
        if (args.size() != 3) {
            return core::CommandResult{false, "usage: journal.put transaction key text"};
        }
        return command_result(host.services().get<SaveJournal>().put(parse_transaction(args[0]),
            args[1], core::as_bytes(args[2])), "put record published; value remains uncommitted");
    }, "Append a bounded opaque text value to the active transaction");
    (void)kernel.add_command("journal.remove", [](core::Kernel& host, core::CommandArgs args) {
        if (args.size() != 2) {
            return core::CommandResult{false, "usage: journal.remove transaction key"};
        }
        return command_result(host.services().get<SaveJournal>().remove(parse_transaction(args[0]), args[1]),
            "remove record published; committed state is unchanged");
    }, "Record an idempotent key removal in the active draft");
    (void)kernel.add_command("journal.commit", [](core::Kernel& host, core::CommandArgs args) {
        if (args.size() != 1) {
            return core::CommandResult{false, "usage: journal.commit transaction"};
        }
        return command_result(host.services().get<SaveJournal>().commit(parse_transaction(args[0])),
            "transaction logically committed; filesystem power-loss durability is not promised");
    }, "Publish a commit digest before exposing its complete state");
    (void)kernel.add_command("journal.abort", [](core::Kernel& host, core::CommandArgs args) {
        if (args.size() != 1) {
            return core::CommandResult{false, "usage: journal.abort transaction"};
        }
        return command_result(host.services().get<SaveJournal>().abort(parse_transaction(args[0])),
            "abort record published; private draft discarded");
    }, "Persist an abort while retaining all preceding committed values");
    (void)kernel.add_command("journal.recover", [](core::Kernel& host, core::CommandArgs args) {
        if (args.size() > 2) {
            return core::CommandResult{false, "usage: journal.recover [slot [strict|prefix|previous]]"};
        }
        auto& service = host.services().get<SaveJournal>();
        RecoveryReport report;
        const auto policy = args.size() == 2 ? parse_policy(args[1]) : RecoveryPolicy::Strict;
        const auto current = service.inspect();
        const bool same = args.empty() || (current.open && current.slot == args[0] &&
            current.root == io::normalize_virtual_path(host.config().text("persistence.root", "/user/saves")));
        const auto result = same ? service.recover(policy, report)
            : open_command(host, args[0], false, policy, &report);
        return command_result(result, recovery_summary(report));
    }, "Recover committed values; explicit prefix/previous modes require checkpoint before writes");
    (void)kernel.add_command("journal.checkpoint", [](core::Kernel& host, core::CommandArgs args) {
        if (!args.empty()) {
            return core::CommandResult{false, "usage: journal.checkpoint"};
        }
        return command_result(host.services().get<SaveJournal>().checkpoint(),
            "new checkpoint generation published; retained generations remain available");
    }, "Compact the current committed state and acknowledge any selected recovery loss");
    (void)kernel.add_command("journal.prune", [](core::Kernel& host, core::CommandArgs args) {
        if (!args.empty()) {
            return core::CommandResult{false, "usage: journal.prune"};
        }
        RetentionReport report;
        const auto result = host.services().get<SaveJournal>().prune(report);
        return command_result(result, "removed_files=" + std::to_string(report.removed_files) +
            " retained_files=" + std::to_string(report.retained_files));
    }, "Remove only recognized generation files absent from the current retention manifest");
    (void)kernel.add_command("journal.save", [](core::Kernel& host, core::CommandArgs args) {
        if (args.empty() || args.size() > 2) {
            return core::CommandResult{false, "usage: journal.save slot [key]"};
        }
        auto* document = host.services().find<scene::SceneDocumentStore>();
        if (!document) {
            return core::CommandResult{false, "scene document service is not registered"};
        }
        const auto opened = open_command(host, args[0], true);
        if (!opened) { return command_result(opened, {}); }
        return command_result(host.services().get<SaveJournal>().save_document(
            args.size() == 2 ? std::string_view(args[1]) : std::string_view("scene"), *document),
            "current scene document committed to the save slot");
    }, "Save the SceneDocumentStore including its preserved extension envelopes");
    (void)kernel.add_command("journal.capture", [](core::Kernel& host, core::CommandArgs args) {
        if (args.empty() || args.size() > 2) {
            return core::CommandResult{false, "usage: journal.capture slot [key]"};
        }
        auto* live_scene = host.services().find<scene::Scene>();
        if (!live_scene) {
            return core::CommandResult{false, "scene service is not registered"};
        }
        const auto opened = open_command(host, args[0], true);
        if (!opened) { return command_result(opened, {}); }
        return command_result(host.services().get<SaveJournal>().capture_scene(
            args.size() == 2 ? std::string_view(args[1]) : std::string_view("scene"), *live_scene),
            "live scene core fields captured and committed");
    }, "Capture live scene hierarchy and core fields; arbitrary component codecs remain owner supplied");
    (void)kernel.add_command("journal.load", [](core::Kernel& host, core::CommandArgs args) {
        if (args.empty() || args.size() > 2) {
            return core::CommandResult{false, "usage: journal.load slot [key]"};
        }
        auto* document = host.services().find<scene::SceneDocumentStore>();
        if (!document) {
            return core::CommandResult{false, "scene document service is not registered"};
        }
        const auto opened = open_command(host, args[0], false);
        if (!opened) { return command_result(opened, {}); }
        return command_result(host.services().get<SaveJournal>().load_document(
            args.size() == 2 ? std::string_view(args[1]) : std::string_view("scene"), *document),
            "committed scene document loaded; scene.document.import performs explicit additive import");
    }, "Decode a committed scene value into SceneDocumentStore without replacing live entities");
    (void)kernel.add_command("journal.slots", [](core::Kernel& host, core::CommandArgs args) {
        if (!args.empty()) {
            return core::CommandResult{false, "usage: journal.slots"};
        }
        auto* files = host.services().find<io::VirtualFileSystem>();
        if (!files) { return core::CommandResult{false, "VFS service is not registered"}; }
        std::vector<SaveSlotInfo> slots;
        const auto result = host.services().get<SaveJournal>().list_slots(*files,
            host.config().text("persistence.root", "/user/saves"), slots);
        std::ostringstream text;
        for (const auto& slot : slots) {
            text << slot.name << " generation=" << slot.generation
                 << " retained=" << slot.retained_generations << " status=" << describe(slot.status.error) << '\n';
        }
        return command_result(result, text.str());
    }, "Inspect bounded manifests for the portable save slot names under persistence.root");
    (void)kernel.add_command("journal.inspect", [](core::Kernel& host, core::CommandArgs args) {
        if (!args.empty()) {
            return core::CommandResult{false, "usage: journal.inspect"};
        }
        auto& service = host.services().get<SaveJournal>();
        const auto state = service.inspect();
        std::ostringstream text;
        text << "open=" << state.open << " slot=" << state.slot << " generation=" << state.generation
             << " committed_sequence=" << state.committed_sequence << " last_sequence=" << state.last_sequence
             << " active_transaction=" << state.active_transaction << " state_bytes=" << state.state_bytes
             << " journal_bytes=" << state.journal_bytes << " checkpoint_required=" << state.requires_checkpoint
             << " backing=" << (state.memory_backed ? "volatile-memory" : "VFS-native-or-unopened") << '\n';
        for (const auto& entry : service.entries()) {
            text << entry.key << " type=" << entry.type << " version=" << entry.version
                 << " bytes=" << entry.bytes << " crc=" << entry.checksum << '\n';
        }
        return core::CommandResult{true, text.str()};
    }, "Inspect only committed entries, pending ownership, recovery requirements and storage class");
    (void)kernel.add_system({"persistence.journal.metrics", core::Phase::EndFrame, 1000, {}},
        [](core::Kernel& host, const core::Frame&) {
            const auto state = host.services().get<SaveJournal>().inspect();
            host.metric("persistence.entries", static_cast<double>(state.entries));
            host.metric("persistence.state_bytes", static_cast<double>(state.state_bytes));
            host.metric("persistence.journal_bytes", static_cast<double>(state.journal_bytes));
            host.metric("persistence.pending_operations", static_cast<double>(state.pending_operations));
            host.metric("persistence.repair_required", state.requires_checkpoint ? 1.0 : 0.0);
        });
}

} // namespace ge::persistence


