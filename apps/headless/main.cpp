#include <ge/content/package.hpp>
#include <ge/core/identity.hpp>
#include <ge/world/state.hpp>
#include <ge/world/streaming.hpp>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

using ge::world::MemoryUsage;
using ge::world::PersistentState;
using ge::world::RegionState;
using ge::world::RequestToken;
using ge::world::StateEntry;
using ge::world::StreamingPolicy;

struct Options {
    std::optional<std::filesystem::path> package;
    std::optional<std::filesystem::path> save_state;
    std::optional<std::filesystem::path> restore_state;
    std::uint64_t budget_bytes{96U};
    bool budget_supplied{false};
    bool inspect{false};
    bool help{false};
    bool version{false};
};

[[nodiscard]] Options parse_options(int argc, char* argv[]) {
    Options options;
    const auto argument_value = [&](int& index, std::string_view option) {
        if (index + 1 >= argc) {
            throw std::invalid_argument(std::string(option) + " requires a value");
        }
        const std::string_view value{argv[++index]};
        if (value.empty() || value.starts_with("--")) {
            throw std::invalid_argument(std::string(option) + " requires a nonempty value");
        }
        return value;
    };
    const auto flag = [](bool& destination, std::string_view option) {
        if (destination) {
            throw std::invalid_argument("duplicate option: " + std::string(option));
        }
        destination = true;
    };

    for (int index = 1; index < argc; ++index) {
        const std::string_view option{argv[index]};
        if (option == "--package" || option == "--save-state" || option == "--restore-state") {
            auto& destination = option == "--package" ? options.package
                : option == "--save-state" ? options.save_state : options.restore_state;
            if (destination) {
                throw std::invalid_argument("duplicate option: " + std::string(option));
            }
            destination = std::filesystem::path{argument_value(index, option)};
        } else if (option == "--budget-bytes") {
            flag(options.budget_supplied, option);
            const auto value = argument_value(index, option);
            std::uint64_t parsed{};
            const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
            if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || parsed == 0U) {
                throw std::invalid_argument("--budget-bytes requires a positive uint64 decimal integer");
            }
            options.budget_bytes = parsed;
        } else if (option == "--inspect") {
            flag(options.inspect, option);
        } else if (option == "--help") {
            flag(options.help, option);
        } else if (option == "--version") {
            flag(options.version, option);
        } else {
            throw std::invalid_argument("unknown argument: " + std::string(option));
        }
    }

    if (options.help || options.version) {
        if (argc != 2) {
            throw std::invalid_argument("--help and --version must each be used alone");
        }
        return options;
    }
    if (!options.package) {
        throw std::invalid_argument("--package PATH is required (see --help)");
    }
    if (options.inspect && (options.save_state || options.restore_state || options.budget_supplied)) {
        throw std::invalid_argument("--inspect accepts only --package PATH");
    }
    return options;
}

void write_string(std::ostream& output, std::string_view value) {
    constexpr std::string_view hex{"0123456789abcdef"};
    output << '"';
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (character == '"' || character == '\\') {
            output << '\\' << character;
        } else if (byte < 0x20U) {
            output << "\\u00" << hex[byte >> 4U] << hex[byte & 15U];
        } else {
            output << character;
        }
    }
    output << '"';
}

[[nodiscard]] std::string source_digest(const ge::WorldPackage& package) {
    constexpr std::string_view hex{"0123456789abcdef"};
    std::string result;
    result.reserve(package.source_digest.size() * 2U);
    for (const auto byte : package.source_digest) {
        result.push_back(hex[byte >> 4U]);
        result.push_back(hex[byte & 15U]);
    }
    return result;
}

void write_package_metadata(std::ostream& output, const ge::WorldPackage& package) {
    output << "\"schema_version\":" << ge::package_schema_version
           << ",\"baseline_id\":" << package.baseline_id
           << ",\"region_count\":" << package.regions.size()
           << ",\"entity_count\":" << package.entities.size()
           << ",\"source_sha256\":";
    write_string(output, source_digest(package));
}

[[nodiscard]] std::string inspect_package(const ge::WorldPackage& package) {
    std::ostringstream output;
    output << "{\"mode\":\"inspect\",";
    write_package_metadata(output, package);
    output << ",\"regions\":[";
    bool separator = false;
    for (const auto& region : package.regions) {
        if (separator) {
            output << ',';
        }
        separator = true;
        output << "{\"id\":" << region.id
               << ",\"required_mask\":" << region.required_mask
               << ",\"budget_bytes\":" << region.budget_bytes
               << ",\"first_entity\":" << region.first_entity
               << ",\"entity_count\":" << region.entity_count << '}';
    }
    output << "]}";
    return output.str();
}

void write_memory(std::ostream& output, const MemoryUsage& memory) {
    output << "{\"capacity_bytes\":" << memory.capacity_bytes
           << ",\"pending_bytes\":" << memory.pending_bytes
           << ",\"resident_bytes\":" << memory.resident_bytes
           << ",\"retiring_bytes\":" << memory.retiring_bytes
           << ",\"reserved_bytes\":" << memory.reserved_bytes
           << ",\"available_bytes\":" << memory.available_bytes << '}';
}

struct Event {
    std::string_view action;
    std::uint32_t region_id;
    std::optional<RequestToken> token;
    bool accepted;
    RegionState state;
    std::uint32_t required_mask;
    std::uint32_t ready_mask;
    MemoryUsage memory;
};

class Evidence final {
public:
    explicit Evidence(const StreamingPolicy& policy) : policy_(policy) {}

    void require(bool condition, std::string_view name) {
        const auto [position, inserted] = checks_.try_emplace(std::string(name), condition);
        if (!inserted) {
            position->second = position->second && condition;
        }
        if (!condition) {
            throw std::runtime_error("lifecycle check failed: " + std::string(name));
        }
    }

    void record(std::string_view action, std::uint32_t region_id,
                std::optional<RequestToken> token, bool accepted) {
        const auto memory = policy_.memory_usage();
        require(memory.reserved_bytes <= memory.capacity_bytes &&
                    memory.pending_bytes <= memory.reserved_bytes &&
                    memory.resident_bytes <= memory.reserved_bytes - memory.pending_bytes &&
                    memory.retiring_bytes == memory.reserved_bytes - memory.pending_bytes -
                                                 memory.resident_bytes &&
                    memory.available_bytes == memory.capacity_bytes - memory.reserved_bytes,
                "budget_respected");
        peak_reserved_bytes_ = std::max(peak_reserved_bytes_, memory.reserved_bytes);
        const auto status = policy_.status(region_id);
        events_.push_back(Event{action, region_id, token, accepted, policy_.state(region_id),
                                status ? status->required_mask : 0U,
                                status ? status->ready_mask : 0U, memory});
    }

    void record(std::string_view action, const RequestToken& token, bool accepted) {
        record(action, token.region_id, token, accepted);
    }

    [[nodiscard]] std::uint64_t peak_reserved_bytes() const noexcept {
        return peak_reserved_bytes_;
    }

    void write(std::ostream& output) const {
        output << "\"checks\":{";
        bool separator = false;
        for (const auto& [name, passed] : checks_) {
            if (separator) {
                output << ',';
            }
            separator = true;
            write_string(output, name);
            output << ':' << (passed ? "true" : "false");
        }
        output << "},\"events\":[";
        separator = false;
        for (const auto& event : events_) {
            if (separator) {
                output << ',';
            }
            separator = true;
            output << "{\"action\":";
            write_string(output, event.action);
            output << ",\"region_id\":" << event.region_id
                   << ",\"accepted\":" << (event.accepted ? "true" : "false")
                   << ",\"token\":";
            if (event.token) {
                output << "{\"region_id\":" << event.token->region_id
                       << ",\"generation\":" << event.token->generation
                       << ",\"content_revision\":" << event.token->content_revision
                       << ",\"coordinate_epoch\":" << event.token->coordinate_epoch << '}';
            } else {
                output << "null";
            }
            output << ",\"state\":";
            write_string(output, ge::world::state_name(event.state));
            output << ",\"required_mask\":" << event.required_mask
                   << ",\"ready_mask\":" << event.ready_mask << ",\"memory\":";
            write_memory(output, event.memory);
            output << '}';
        }
        output << ']';
    }

private:
    const StreamingPolicy& policy_;
    std::map<std::string, bool> checks_;
    std::vector<Event> events_;
    std::uint64_t peak_reserved_bytes_{};
};

[[nodiscard]] std::int32_t get_value(const PersistentState& state, ge::PersistentId id) {
    const auto value = state.get(id);
    if (!value) {
        throw std::runtime_error("authored persistent ID is missing from state");
    }
    return *value;
}

[[nodiscard]] std::uint64_t next_sequence(const PersistentState& state) {
    if (state.sequence() == std::numeric_limits<std::uint64_t>::max()) {
        throw std::runtime_error("persistent snapshot sequence is exhausted");
    }
    return state.sequence() + 1U;
}

struct ResidentEntity {
    ge::PersistentId id;
    ge::RuntimeHandle handle;
};

[[nodiscard]] std::string run_scenario(const ge::WorldPackage& package, const Options& options) {
    for (const auto& region : package.regions) {
        if (region.budget_bytes > options.budget_bytes) {
            throw std::runtime_error("region " + std::to_string(region.id) +
                " cannot fit within --budget-bytes " + std::to_string(options.budget_bytes));
        }
    }

    std::vector<StateEntry> initial_state;
    initial_state.reserve(package.entities.size());
    for (const auto& entity : package.entities) {
        initial_state.push_back(StateEntry{ge::PersistentId{entity.id}, entity.initial_value});
    }
    const std::span<const StateEntry> initial_values{initial_state};
    PersistentState state{package.baseline_id, initial_values};
    const auto& first_region = package.regions.front();
    const auto& first_entity = package.entities[first_region.first_entity];
    const ge::PersistentId changed_id{first_entity.id};
    std::optional<std::int32_t> restored_value;
    std::uint64_t restored_sequence{};
    if (options.restore_state) {
        state.restore(*options.restore_state);
        restored_value = get_value(state, changed_id);
        restored_sequence = state.sequence();
    }
    const auto value_before_mutation = get_value(state, changed_id);
    const std::uint64_t sequence_steps = options.save_state ? 2U : 1U;
    if (state.sequence() > std::numeric_limits<std::uint64_t>::max() - sequence_steps) {
        throw std::runtime_error("insufficient snapshot sequence space for this lifecycle");
    }

    StreamingPolicy policy{options.budget_bytes};
    ge::GenerationPool handles{static_cast<std::uint32_t>(package.entities.size())};
    Evidence evidence{policy};
    // Fixture labels are independent of stable IDs. No origin shift is performed.
    constexpr std::uint64_t content_revision{1U};
    constexpr std::uint64_t coordinate_epoch{1U};
    std::uint64_t budget_denials{};
    std::optional<ge::RuntimeHandle> stale_handle;
    bool handle_reuse_verified = false;
    std::set<std::uint32_t> visited_regions;
    std::vector<std::uint32_t> activation_order;

    const auto begin = [&](const ge::RegionDefinition& region) {
        const auto token = policy.begin(region.id, content_revision, coordinate_epoch,
                                        region.required_mask, region.budget_bytes);
        evidence.require(token.has_value(), "region_admitted");
        evidence.record("begin", *token, true);
        evidence.require(policy.state(region.id) == RegionState::Loading,
                         "request_begins_loading");
        return *token;
    };

    // Probe an authored, currently unloaded neighbor. With the supplied 64/96
    // fixture it is denied. A larger explicit budget can admit it, in which case
    // its reservation is canceled and acknowledged before the scenario proceeds.
    const auto probe_budget = [&](const ge::RegionDefinition& neighbor) {
        evidence.require(policy.state(neighbor.id) == RegionState::Unloaded,
                         "probe_region_unloaded");
        const auto before = policy.memory_usage();
        const bool should_fit = neighbor.budget_bytes <= before.available_bytes;
        const auto token = policy.begin(neighbor.id, content_revision, coordinate_epoch,
                                        neighbor.required_mask, neighbor.budget_bytes);
        evidence.require(token.has_value() == should_fit, "budget_admission_matches_capacity");
        if (!token) {
            ++budget_denials;
            evidence.require(policy.memory_usage() == before &&
                                 policy.state(neighbor.id) == RegionState::Unloaded,
                             "denied_admission_preserved_state");
            evidence.record("budget_admission_rejected", neighbor.id, std::nullopt, false);
            return;
        }
        evidence.record("budget_probe_admitted", *token, true);
        evidence.require(policy.cancel(*token), "probe_canceled");
        evidence.record("budget_probe_canceled", *token, true);
        evidence.require(policy.memory_usage().reserved_bytes ==
                             before.reserved_bytes + neighbor.budget_bytes,
                         "retirement_retained_reservation");
        evidence.require(policy.acknowledge_retirement(*token), "probe_acknowledged");
        evidence.record("budget_probe_acknowledged", *token, true);
        evidence.require(policy.memory_usage() == before, "retirement_ack_released_reservation");
    };

    const auto activate = [&](const ge::RegionDefinition& region, const RequestToken& token) {
        evidence.require(policy.mark_ready(token, region.required_mask), "fixture_readiness_published");
        evidence.record("symbolic_readiness_published", token, true);
        const auto ready = policy.status(region.id);
        evidence.require(ready && ready->state == RegionState::Ready && ready->fully_ready(),
                         "readiness_required");
        evidence.require(policy.activate(token), "ready_region_activated");
        evidence.record("activate", token, true);
        visited_regions.insert(region.id);
        activation_order.push_back(region.id);

        std::vector<ResidentEntity> residents;
        residents.reserve(region.entity_count);
        for (std::size_t offset = 0; offset < region.entity_count; ++offset) {
            const auto& entity = package.entities[static_cast<std::size_t>(region.first_entity) + offset];
            const auto handle = handles.acquire();
            evidence.require(handle.has_value() && handles.alive(*handle), "runtime_handle_acquired");
            const ge::PersistentId id{entity.id};
            evidence.require(state.get(id).has_value(), "known_entities_materialized");
            if (stale_handle && handle->index == stale_handle->index && !handle_reuse_verified) {
                evidence.require(handle->generation != stale_handle->generation &&
                                     !handles.alive(*stale_handle) && !handles.release(*stale_handle) &&
                                     handles.alive(*handle),
                                 "stale_handle_rejected");
                handle_reuse_verified = true;
            }
            residents.push_back(ResidentEntity{id, *handle});
        }
        return residents;
    };

    const auto retire = [&](const ge::RegionDefinition& region, const RequestToken& token,
                            const std::vector<ResidentEntity>& residents,
                            const ge::RegionDefinition* neighbor) {
        const auto before = policy.memory_usage();
        evidence.require(policy.retire(token), "active_region_retired");
        evidence.record("retire", token, true);
        evidence.require(policy.state(region.id) == RegionState::Retiring &&
                             policy.memory_usage().reserved_bytes == before.reserved_bytes &&
                             policy.memory_usage().retiring_bytes == region.budget_bytes,
                         "retirement_retained_reservation");
        if (neighbor != nullptr) {
            probe_budget(*neighbor);
        }
        // Reverse release makes the first entity's slot the next reused slot.
        for (auto iterator = residents.rbegin(); iterator != residents.rend(); ++iterator) {
            evidence.require(handles.release(iterator->handle) && !handles.alive(iterator->handle),
                             "runtime_handles_released");
        }
        evidence.require(handles.size() == 0U, "runtime_handles_released");
        evidence.require(policy.acknowledge_retirement(token), "retirement_acknowledged");
        evidence.record("acknowledge_retirement", token, true);
        evidence.require(policy.state(region.id) == RegionState::Unloaded &&
                             policy.memory_usage().reserved_bytes == before.reserved_bytes - region.budget_bytes,
                         "retirement_ack_released_reservation");
    };

    // Deliberately withhold one required bit, then cancel and retry the request.
    const auto canceled_token = begin(first_region);
    const auto partial_mask = first_region.required_mask & (first_region.required_mask - 1U);
    if (partial_mask != 0U) {
        evidence.require(policy.mark_ready(canceled_token, partial_mask), "partial_readiness_published");
        evidence.record("partial_readiness_published", canceled_token, true);
    }
    const auto incomplete_status = policy.status(first_region.id);
    const auto incomplete_memory = policy.memory_usage();
    evidence.require(!policy.activate(canceled_token) &&
                         policy.status(first_region.id) == incomplete_status &&
                         policy.memory_usage() == incomplete_memory,
                     "incomplete_readiness_rejected");
    evidence.record("incomplete_activation_rejected", canceled_token, false);
    evidence.require(policy.cancel(canceled_token), "incomplete_request_canceled");
    evidence.record("cancel", canceled_token, true);
    evidence.require(policy.memory_usage().reserved_bytes == first_region.budget_bytes &&
                         policy.memory_usage().retiring_bytes == first_region.budget_bytes,
                     "retirement_retained_reservation");
    const auto retry_while_retiring = policy.begin(first_region.id, content_revision, coordinate_epoch,
                                                  first_region.required_mask, first_region.budget_bytes);
    evidence.require(!retry_while_retiring && policy.state(first_region.id) == RegionState::Retiring,
                     "retry_waited_for_retirement_ack");
    evidence.record("retry_while_retiring_rejected", first_region.id, std::nullopt, false);
    evidence.require(policy.acknowledge_retirement(canceled_token), "retirement_acknowledged");
    evidence.record("acknowledge_cancellation", canceled_token, true);
    evidence.require(policy.memory_usage().reserved_bytes == 0U, "retirement_ack_released_reservation");

    const auto first_token = begin(first_region);
    evidence.require(first_token.generation > canceled_token.generation,
                     "retry_issued_new_generation");
    const auto retry_status = policy.status(first_region.id);
    const auto retry_memory = policy.memory_usage();
    evidence.require(!policy.mark_ready(canceled_token, first_region.required_mask) &&
                         !policy.activate(canceled_token) &&
                         policy.status(first_region.id) == retry_status &&
                         policy.memory_usage() == retry_memory,
                     "stale_token_rejected");
    evidence.record("stale_completion_rejected", canceled_token, false);
    const auto* neighbor = package.regions.size() > 1U ? &package.regions[1U] : nullptr;
    if (neighbor != nullptr) {
        probe_budget(*neighbor);
    }
    const auto first_residents = activate(first_region, first_token);
    evidence.require(!first_residents.empty() && first_residents.front().id == changed_id &&
                         handles.alive(first_residents.front().handle),
                     "mutation_uses_live_handle");
    const auto changed_value = value_before_mutation == std::numeric_limits<std::int32_t>::max()
        ? static_cast<std::int32_t>(value_before_mutation - 1)
        : static_cast<std::int32_t>(value_before_mutation + 1);
    evidence.require(state.set(changed_id, changed_value) &&
                         get_value(state, changed_id) == changed_value &&
                         changed_value != value_before_mutation,
                     "persistent_value_mutated");
    stale_handle = first_residents.front().handle;
    if (neighbor != nullptr) {
        probe_budget(*neighbor);
    }
    retire(first_region, first_token, first_residents, neighbor);

    for (std::size_t index = 1U; index < package.regions.size(); ++index) {
        const auto& region = package.regions[index];
        const auto token = begin(region);
        const auto residents = activate(region, token);
        retire(region, token, residents, nullptr);
    }
    evidence.require(visited_regions.size() == package.regions.size(), "all_regions_visited");

    const auto reloaded_token = begin(first_region);
    const auto reloaded_residents = activate(first_region, reloaded_token);
    evidence.require(get_value(state, changed_id) == changed_value &&
                         first_entity.initial_value == initial_state[first_region.first_entity].value,
                     "mutation_survived_reload");
    evidence.require(handle_reuse_verified, "stale_handle_rejected");
    retire(first_region, reloaded_token, reloaded_residents, nullptr);
    evidence.require(policy.live_regions() == 0U && policy.memory_usage().reserved_bytes == 0U &&
                         handles.size() == 0U,
                     "final_resources_retired");

    const auto snapshot = state.capture(next_sequence(state));
    PersistentState fresh_state{package.baseline_id, initial_values};
    fresh_state.restore(snapshot);
    evidence.require(fresh_state.sequence() == snapshot.sequence() &&
                         fresh_state.baseline_id() == state.baseline_id() &&
                         get_value(fresh_state, changed_id) == changed_value,
                     "snapshot_restored");
    const auto same_values = [&](const PersistentState& candidate) {
        return candidate.size() == state.size() &&
               std::all_of(initial_state.begin(), initial_state.end(), [&](const StateEntry& entry) {
                   return candidate.get(entry.id) == state.get(entry.id);
               });
    };
    evidence.require(same_values(fresh_state), "all_values_restored");
    if (options.save_state) {
        state.save(*options.save_state, next_sequence(state));
        PersistentState saved_state{package.baseline_id, initial_values};
        saved_state.restore(*options.save_state);
        evidence.require(saved_state.sequence() == state.sequence() && same_values(saved_state),
                         "save_round_trip");
    }

    std::ostringstream output;
    output << "{\"mode\":\"headless-policy-fixture\",\"status\":\"ok\",";
    write_package_metadata(output, package);
    output << ",\"byte_accounting\":\"modeled reservations; not measured resource allocation\""
           << ",\"readiness\":\"symbolic fixture signals\""
           << ",\"budget_bytes\":" << options.budget_bytes
           << ",\"peak_reserved_bytes\":" << evidence.peak_reserved_bytes()
           << ",\"final_reserved_bytes\":" << policy.memory_usage().reserved_bytes
           << ",\"budget_denials\":" << budget_denials
           << ",\"budget_pressure_observed\":" << (budget_denials != 0U ? "true" : "false")
           << ",\"persistent_id\":" << changed_id.value
           << ",\"initial_value\":" << first_entity.initial_value
           << ",\"value_before_mutation\":" << value_before_mutation
           << ",\"restored_value\":";
    if (restored_value) {
        output << *restored_value;
    } else {
        output << "null";
    }
    output << ",\"final_value\":" << get_value(state, changed_id)
           << ",\"restored_sequence\":" << restored_sequence
           << ",\"snapshot_sequence\":" << snapshot.sequence()
           << ",\"state_sequence\":" << state.sequence()
           << ",\"save_state_written\":" << (options.save_state ? "true" : "false")
           << ",\"visited_regions\":[";
    bool separator = false;
    for (const auto id : visited_regions) {
        if (separator) {
            output << ',';
        }
        separator = true;
        output << id;
    }
    output << "],\"activation_order\":[";
    separator = false;
    for (const auto id : activation_order) {
        if (separator) {
            output << ',';
        }
        separator = true;
        output << id;
    }
    output << "],";
    evidence.write(output);
    output << '}';
    return output.str();
}

void print_help() {
    std::cout
        << "Usage: ge_headless --package PATH [--inspect]\n"
        << "       ge_headless --package PATH [--budget-bytes N]\n"
        << "                   [--restore-state PATH] [--save-state PATH]\n"
        << "       ge_headless --help | --version\n\n"
        << "  --package PATH       Required cooked GEPACK01 package.\n"
        << "  --inspect            Validate the package and print JSON metadata only.\n"
        << "  --budget-bytes N     Positive modeled reservation capacity (default: 96).\n"
        << "  --restore-state PATH Restore compatible state before the scenario mutation.\n"
        << "  --save-state PATH    Publish and verify state after the scenario.\n"
        << "  --help               Show this usage text; use alone.\n"
        << "  --version            Show the application version; use alone.\n\n"
        << "Data commands emit one JSON object. Errors go to stderr and return nonzero.\n"
        << "This synchronous fixture models readiness and reservations; it does not run\n"
        << "physics, asynchronous I/O, or a performance benchmark.\n";
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        const auto options = parse_options(argc, argv);
        if (options.help) {
            print_help();
        } else if (options.version) {
            std::cout << "Game Engine 0.1.0\n";
        } else {
            const auto package = ge::read_package(*options.package);
            const auto result = options.inspect ? inspect_package(package) : run_scenario(package, options);
            std::cout << result << '\n';
        }
        if (!std::cout) {
            throw std::runtime_error("failed to write command output");
        }
        return 0;
    } catch (const std::invalid_argument& error) {
        std::cerr << "ge_headless: " << error.what() << '\n';
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "ge_headless: " << error.what() << '\n';
        return 1;
    }
}
