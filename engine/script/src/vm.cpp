#include <ge/script/vm.hpp>
#include <ge/core/binary.hpp>
#include <ge/core/runtime.hpp>
#include <ge/io/filesystem.hpp>
#include <ge/physics/dynamics.hpp>
#include <ge/scene/scene.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <deque>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace ge::script {
namespace {

bool valid_limits(const VmLimits& value) {
    return value.max_source_bytes > 0 && value.max_source_bytes <= 16U * 1024U * 1024U &&
        value.max_binary_bytes > 0 && value.max_binary_bytes <= 64U * 1024U * 1024U &&
        value.max_string_bytes <= 65536 && value.max_programs > 0 && value.max_programs <= 4096 &&
        value.max_instructions > 0 && value.max_instructions <= 262144 &&
        value.max_constants <= 65536 && value.max_globals <= 4096 &&
        value.max_functions > 0 && value.max_functions <= 4096 &&
        value.max_imports <= 4096 && value.max_parameters <= 256 &&
        value.max_locals <= 4096 && value.max_local_slots <= 65536 &&
        value.max_stack > 0 && value.max_stack <= 4096 &&
        value.max_call_depth > 0 && value.max_call_depth <= 256 &&
        value.max_instances > 0 && value.max_instances <= 4096 &&
        value.max_verifier_values > 0 && value.max_verifier_values <= 16U * 1024U * 1024U &&
        value.slice_gas > 0 && value.slice_gas <= 1000000 &&
        value.frame_gas >= value.slice_gas && value.frame_gas <= 10000000 &&
        value.max_total_gas >= value.slice_gas && value.max_total_gas <= 1000000000 &&
        value.max_host_calls_per_slice > 0 && value.max_host_calls_per_slice <= 4096 &&
        value.max_sleep_frames <= 1000000000;
}

bool valid_name(std::string_view text) {
    if (text.empty() || text.size() > 128) return false;
    return std::all_of(text.begin(), text.end(), [](char character) {
        const auto value = static_cast<unsigned char>(character);
        return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
            (value >= '0' && value <= '9') || value == '_' || value == '.';
    });
}

bool valid_type(ValueType type) {
    return type <= ValueType::Entity || type == ValueType::Any;
}

bool valid_value(const Value& value, const VmLimits& limits) {
    if (value.data.valueless_by_exception()) return false;
    if (const auto* number = std::get_if<double>(&value.data)) return std::isfinite(*number);
    if (const auto* vector = std::get_if<math::Vec3>(&value.data)) return math::finite(*vector);
    if (const auto* string = std::get_if<std::string>(&value.data)) {
        return string->size() <= limits.max_string_bytes && core::valid_utf8(*string);
    }
    if (const auto* entity = std::get_if<ecs::Entity>(&value.data)) {
        return (entity->index == 0) == (entity->generation == 0);
    }
    return true;
}

bool compatible(ValueType expected, ValueType actual) {
    return expected == ValueType::Any || actual == ValueType::Any || expected == actual;
}

template<class T>
T parse_integer(std::string_view text) {
    T result{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
        throw std::invalid_argument("invalid integer: " + std::string(text));
    }
    return result;
}

double parse_number(std::string_view text) {
    double result{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
        !std::isfinite(result)) throw std::invalid_argument("expected finite number");
    return result;
}

struct Token { std::string text; std::uint32_t column{}; bool quoted{}; };

std::vector<Token> tokenize(std::string_view line) {
    std::vector<Token> result;
    std::size_t cursor = 0;
    while (cursor < line.size()) {
        if (line[cursor] == ' ' || line[cursor] == '\t' || line[cursor] == '\r') {
            ++cursor;
            continue;
        }
        if (line[cursor] == '#' || line[cursor] == ';') break;
        Token token{{}, static_cast<std::uint32_t>(cursor + 1), line[cursor] == '"'};
        if (token.quoted) {
            ++cursor;
            bool closed = false;
            while (cursor < line.size()) {
                char character = line[cursor++];
                if (character == '"') { closed = true; break; }
                if (character == '\\') {
                    if (cursor == line.size()) throw std::invalid_argument("unfinished string escape");
                    character = line[cursor++];
                    if (character == 'n') character = '\n';
                    else if (character == 't') character = '\t';
                    else if (character == 'r') character = '\r';
                    else if (character != '\\' && character != '"') {
                        throw std::invalid_argument("unsupported string escape");
                    }
                }
                token.text.push_back(character);
            }
            if (!closed) throw std::invalid_argument("unterminated string");
            if (cursor < line.size() && line[cursor] != ' ' && line[cursor] != '\t' &&
                line[cursor] != '\r' && line[cursor] != ';' && line[cursor] != '#') {
                throw std::invalid_argument("expected whitespace after string");
            }
        } else {
            const auto begin = cursor;
            while (cursor < line.size() && line[cursor] != ' ' && line[cursor] != '\t' &&
                   line[cursor] != '\r' && line[cursor] != ';' && line[cursor] != '#') ++cursor;
            token.text = line.substr(begin, cursor - begin);
        }
        result.push_back(std::move(token));
        if (result.size() > 16) throw std::invalid_argument("too many tokens on one line");
    }
    return result;
}

Value literal(std::span<const Token> tokens) {
    if (tokens.empty()) throw std::invalid_argument("missing literal type");
    const auto& type = tokens[0].text;
    if (type == "null" && tokens.size() == 1) return {};
    if (type == "vec3" && tokens.size() == 4) {
        return Value(math::Vec3{parse_number(tokens[1].text), parse_number(tokens[2].text),
                                parse_number(tokens[3].text)});
    }
    if (tokens.size() != 2) throw std::invalid_argument("wrong literal operand count");
    const auto& value = tokens[1].text;
    if (type == "bool" && (value == "true" || value == "false")) return Value(value == "true");
    if (type == "int") return Value(parse_integer<std::int64_t>(value));
    if (type == "number") return Value(parse_number(value));
    if (type == "string" && tokens[1].quoted) return Value(value);
    if (type == "entity") {
        if (value == "null") return Value(ecs::Entity{});
        if (const auto entity = ecs::parse_entity(value)) return Value(*entity);
    }
    throw std::invalid_argument("invalid literal type or value");
}

const std::map<std::string, Op, std::less<>> op_names{
    {"nop", Op::Nop}, {"const", Op::Constant}, {"load", Op::LoadLocal},
    {"store", Op::StoreLocal}, {"gload", Op::LoadGlobal}, {"gstore", Op::StoreGlobal},
    {"pop", Op::Pop}, {"dup", Op::Duplicate}, {"add", Op::Add}, {"sub", Op::Subtract},
    {"mul", Op::Multiply}, {"div", Op::Divide}, {"mod", Op::Remainder}, {"neg", Op::Negate},
    {"eq", Op::Equal}, {"ne", Op::NotEqual}, {"lt", Op::Less}, {"le", Op::LessEqual},
    {"gt", Op::Greater}, {"ge", Op::GreaterEqual}, {"not", Op::Not}, {"jmp", Op::Jump},
    {"jfalse", Op::JumpFalse}, {"call", Op::Call}, {"host", Op::Host}, {"ret", Op::Return},
    {"yield", Op::Yield}, {"sleep", Op::Sleep}, {"halt", Op::Halt}
};

bool has_operand(Op op) {
    return op == Op::Constant || op == Op::LoadLocal || op == Op::StoreLocal ||
        op == Op::LoadGlobal || op == Op::StoreGlobal || op == Op::Jump ||
        op == Op::JumpFalse || op == Op::Call || op == Op::Host;
}

ValueType binary_type(Op op, ValueType left, ValueType right) {
    if (op == Op::Equal || op == Op::NotEqual) return ValueType::Boolean;
    if (left == ValueType::Any || right == ValueType::Any) {
        return op >= Op::Less && op <= Op::GreaterEqual ? ValueType::Boolean : ValueType::Any;
    }
    if (op >= Op::Less && op <= Op::GreaterEqual) {
        if (left == right && (left == ValueType::Integer || left == ValueType::Number ||
            left == ValueType::String)) return ValueType::Boolean;
    } else if (left == right && (left == ValueType::Integer || left == ValueType::Number)) {
        if (op != Op::Remainder || left == ValueType::Integer) return left;
    } else if (op == Op::Add && left == ValueType::String && right == left) return left;
    else if ((op == Op::Add || op == Op::Subtract) && left == ValueType::Vector && right == left) {
        return ValueType::Vector;
    } else if ((op == Op::Multiply || op == Op::Divide) && left == ValueType::Vector &&
               right == ValueType::Number) return ValueType::Vector;
    else if (op == Op::Multiply && left == ValueType::Number && right == ValueType::Vector) {
        return ValueType::Vector;
    }
    throw std::invalid_argument("operator operand types are incompatible");
}

} // namespace

ValueType Value::type() const noexcept { return static_cast<ValueType>(data.index()); }

std::string Value::display() const {
    std::ostringstream stream;
    switch (type()) {
    case ValueType::Null: return "null";
    case ValueType::Boolean: return std::get<bool>(data) ? "true" : "false";
    case ValueType::Integer: return std::to_string(std::get<std::int64_t>(data));
    case ValueType::Number: stream << std::get<double>(data); break;
    case ValueType::String: return std::get<std::string>(data);
    case ValueType::Vector: {
        const auto vector = std::get<math::Vec3>(data);
        stream << vector.x << ',' << vector.y << ',' << vector.z;
        break;
    }
    case ValueType::Entity: return ecs::format_entity(std::get<ecs::Entity>(data));
    case ValueType::Any: return "invalid";
    }
    return stream.str();
}

std::optional<ScriptError> verify(const ProgramData& data, const VmLimits& limits) {
    std::uint32_t current = 0;
    try {
        if (!valid_limits(limits)) throw std::invalid_argument("invalid VM limits");
        if (data.instructions.empty() || data.instructions.size() > limits.max_instructions ||
            data.constants.size() > limits.max_constants || data.globals.size() > limits.max_globals ||
            data.functions.empty() || data.functions.size() > limits.max_functions ||
            data.imports.size() > limits.max_imports || data.entry >= data.functions.size()) {
            throw std::invalid_argument("program table bounds exceeded");
        }
        for (const auto& value : data.constants) {
            if (!valid_value(value, limits)) throw std::invalid_argument("invalid constant");
        }
        for (const auto& value : data.globals) {
            if (!valid_value(value, limits)) throw std::invalid_argument("invalid global initializer");
        }
        std::set<std::string> names;
        for (const auto& host : data.imports) {
            if (!valid_name(host.name) || !names.insert(host.name).second ||
                host.parameters.size() > limits.max_parameters || !valid_type(host.result)) {
                throw std::invalid_argument("invalid host import signature");
            }
            for (const auto type : host.parameters) {
                if (!valid_type(type)) throw std::invalid_argument("invalid host parameter type");
            }
        }
        names.clear();
        std::uint32_t previous_end = 0;
        std::size_t verifier_values = 0;
        for (const auto& function : data.functions) {
            current = function.begin;
            if (!valid_name(function.name) || !names.insert(function.name).second ||
                function.begin != previous_end || function.end <= function.begin ||
                function.end > data.instructions.size() || function.parameters > function.locals ||
                function.parameters > limits.max_parameters || function.locals > limits.max_locals ||
                function.locals > limits.max_local_slots) {
                throw std::invalid_argument("invalid function range, name, or local count");
            }
            previous_end = function.end;
            for (current = function.begin; current < function.end; ++current) {
                const auto& instruction = data.instructions[current];
                const auto operand = instruction.operand;
                if (instruction.op > Op::Halt || (!has_operand(instruction.op) && operand != 0)) {
                    throw std::invalid_argument("unknown opcode or noncanonical operand");
                }
                if ((instruction.op == Op::Constant && operand >= data.constants.size()) ||
                    ((instruction.op == Op::LoadLocal || instruction.op == Op::StoreLocal) && operand >= function.locals) ||
                    ((instruction.op == Op::LoadGlobal || instruction.op == Op::StoreGlobal) && operand >= data.globals.size()) ||
                    (instruction.op == Op::Call && operand >= data.functions.size()) ||
                    (instruction.op == Op::Host && operand >= data.imports.size()) ||
                    ((instruction.op == Op::Jump || instruction.op == Op::JumpFalse) &&
                     (operand < function.begin || operand >= function.end))) {
                    throw std::invalid_argument("instruction operand is out of bounds");
                }
            }
            using Stack = std::vector<ValueType>;
            std::vector<std::optional<Stack>> states(function.end - function.begin);
            std::deque<std::uint32_t> queue;
            states[0] = Stack{};
            queue.push_back(function.begin);
            auto propagate = [&](std::uint32_t target, const Stack& stack) {
                if (target >= function.end) throw std::invalid_argument("function falls through its end");
                auto& old = states[target - function.begin];
                if (!old) {
                    if (stack.size() > limits.max_verifier_values - verifier_values) {
                        throw std::invalid_argument("verification memory budget exceeded");
                    }
                    verifier_values += stack.size();
                    old = stack;
                    queue.push_back(target);
                } else {
                    if (old->size() != stack.size()) throw std::invalid_argument("branch stack depths differ");
                    bool changed = false;
                    for (std::size_t i = 0; i < stack.size(); ++i) {
                        if ((*old)[i] != stack[i] && (*old)[i] != ValueType::Any) {
                            (*old)[i] = ValueType::Any;
                            changed = true;
                        }
                    }
                    if (changed) queue.push_back(target);
                }
            };
            while (!queue.empty()) {
                current = queue.front();
                queue.pop_front();
                auto stack = *states[current - function.begin];
                const auto instruction = data.instructions[current];
                const auto operand = instruction.operand;
                auto pop = [&](ValueType expected = ValueType::Any) {
                    if (stack.empty()) throw std::invalid_argument("stack underflow");
                    const auto actual = stack.back();
                    stack.pop_back();
                    if (!compatible(expected, actual)) throw std::invalid_argument("stack value has wrong type");
                    return actual;
                };
                bool continues = true;
                switch (instruction.op) {
                case Op::Constant: stack.push_back(data.constants[operand].type()); break;
                case Op::LoadLocal: stack.push_back(ValueType::Any); break;
                case Op::StoreLocal: (void)pop(); break;
                case Op::LoadGlobal: stack.push_back(data.globals[operand].type()); break;
                case Op::StoreGlobal: (void)pop(data.globals[operand].type()); break;
                case Op::Pop: (void)pop(); break;
                case Op::Duplicate: { const auto type = pop(); stack.push_back(type); stack.push_back(type); break; }
                case Op::Add: case Op::Subtract: case Op::Multiply: case Op::Divide:
                case Op::Remainder: case Op::Equal: case Op::NotEqual: case Op::Less:
                case Op::LessEqual: case Op::Greater: case Op::GreaterEqual: {
                    const auto right = pop();
                    const auto left = pop();
                    stack.push_back(binary_type(instruction.op, left, right));
                    break;
                }
                case Op::Negate: {
                    const auto type = pop();
                    if (type != ValueType::Any && type != ValueType::Integer &&
                        type != ValueType::Number && type != ValueType::Vector) {
                        throw std::invalid_argument("negate requires numeric value");
                    }
                    stack.push_back(type);
                    break;
                }
                case Op::Not: (void)pop(ValueType::Boolean); stack.push_back(ValueType::Boolean); break;
                case Op::Jump: propagate(operand, stack); continues = false; break;
                case Op::JumpFalse: (void)pop(ValueType::Boolean); propagate(operand, stack); break;
                case Op::Call: {
                    const auto& callee = data.functions[operand];
                    for (std::uint32_t i = 0; i < callee.parameters; ++i) (void)pop();
                    if (callee.returns_value) stack.push_back(ValueType::Any);
                    break;
                }
                case Op::Host: {
                    const auto& host = data.imports[operand];
                    for (auto it = host.parameters.rbegin(); it != host.parameters.rend(); ++it) (void)pop(*it);
                    stack.push_back(host.result);
                    break;
                }
                case Op::Return:
                    if (stack.size() != (function.returns_value ? 1U : 0U)) {
                        throw std::invalid_argument("return stack differs from function signature");
                    }
                    continues = false;
                    break;
                case Op::Halt:
                    if (!stack.empty()) throw std::invalid_argument("halt requires empty stack");
                    continues = false;
                    break;
                case Op::Sleep: (void)pop(ValueType::Integer); break;
                case Op::Nop: case Op::Yield: break;
                }
                if (stack.size() > limits.max_stack) throw std::invalid_argument("stack limit exceeded");
                if (continues) propagate(current + 1, stack);
            }
        }
        if (previous_end != data.instructions.size()) throw std::invalid_argument("unowned instructions");
        return std::nullopt;
    } catch (const std::exception& error) {
        const auto source = current < data.instructions.size() ? data.instructions[current].source : SourceLocation{};
        return ScriptError{error.what(), source, current};
    }
}

Program::Program(ProgramData data) : data_(std::move(data)) {}
const ProgramData& Program::data() const noexcept { return data_; }

CompileResult Program::create(ProgramData data, VmLimits limits) {
    if (auto error = verify(data, limits)) return {{}, std::move(*error)};
    return {std::shared_ptr<const Program>(new Program(std::move(data))), {}};
}

CompileResult assemble(std::string_view source, std::span<const HostSignature> hosts, VmLimits limits) {
    SourceLocation location{1, 1};
    try {
        if (!valid_limits(limits) || source.size() > limits.max_source_bytes || !core::valid_utf8(source)) {
            throw std::invalid_argument("source exceeds limits or is not UTF-8");
        }
        ProgramData data;
        std::map<std::string, std::uint32_t, std::less<>> labels, functions, imports;
        struct Fixup { std::uint32_t instruction; std::string name; bool function; };
        std::vector<Fixup> fixups;
        std::optional<std::uint32_t> active;
        std::string entry = "main";
        std::size_t cursor = 0;
        while (cursor < source.size()) {
            const auto newline = source.find('\n', cursor);
            const auto end = newline == std::string_view::npos ? source.size() : newline;
            const auto tokens = tokenize(source.substr(cursor, end - cursor));
            cursor = end == source.size() ? end : end + 1;
            if (!tokens.empty()) {
                location.column = tokens[0].column;
                const auto& name = tokens[0].text;
                auto count = [&](std::size_t expected) {
                    if (tokens.size() != expected) throw std::invalid_argument("wrong operand count for " + name);
                };
                if (name == ".const" || name == ".global") {
                    auto value = literal(std::span<const Token>(tokens).subspan(1));
                    auto& table = name == ".const" ? data.constants : data.globals;
                    const auto maximum = name == ".const" ? limits.max_constants : limits.max_globals;
                    if (table.size() >= maximum || !valid_value(value, limits)) {
                        throw std::invalid_argument("literal table or value limit exceeded");
                    }
                    table.push_back(std::move(value));
                } else if (name == ".entry") {
                    count(2);
                    entry = tokens[1].text;
                } else if (name == ".func") {
                    count(5);
                    if (active || data.functions.size() >= limits.max_functions || !valid_name(tokens[1].text)) {
                        throw std::invalid_argument("invalid or nested function declaration");
                    }
                    const auto index = static_cast<std::uint32_t>(data.functions.size());
                    if (!functions.emplace(tokens[1].text, index).second) throw std::invalid_argument("duplicate function");
                    const auto returns = parse_integer<std::uint32_t>(tokens[4].text);
                    if (returns > 1) throw std::invalid_argument("return count must be 0 or 1");
                    data.functions.push_back({tokens[1].text, static_cast<std::uint32_t>(data.instructions.size()), 0,
                        parse_integer<std::uint32_t>(tokens[2].text), parse_integer<std::uint32_t>(tokens[3].text), returns == 1});
                    active = index;
                } else if (name == ".end") {
                    count(1);
                    if (!active) throw std::invalid_argument(".end outside a function");
                    data.functions[*active].end = static_cast<std::uint32_t>(data.instructions.size());
                    active.reset();
                } else {
                    if (!active) throw std::invalid_argument("instruction or label outside a function");
                    if (name.ends_with(':')) {
                        count(1);
                        const auto label = name.substr(0, name.size() - 1);
                        if (!valid_name(label) || !labels.emplace(std::to_string(*active) + ':' + label,
                            static_cast<std::uint32_t>(data.instructions.size())).second) {
                            throw std::invalid_argument("invalid or duplicate label");
                        }
                    } else {
                        if (data.instructions.size() >= limits.max_instructions) throw std::invalid_argument("instruction limit exceeded");
                        Instruction instruction{Op::Nop, 0, location};
                        if (name == "push") {
                            if (data.constants.size() >= limits.max_constants) throw std::invalid_argument("constant limit exceeded");
                            auto value = literal(std::span<const Token>(tokens).subspan(1));
                            if (!valid_value(value, limits)) throw std::invalid_argument("invalid constant value");
                            instruction = {Op::Constant, static_cast<std::uint32_t>(data.constants.size()), location};
                            data.constants.push_back(std::move(value));
                        } else {
                            const auto operation = op_names.find(name);
                            if (operation == op_names.end()) throw std::invalid_argument("unknown instruction: " + name);
                            instruction.op = operation->second;
                            count(has_operand(instruction.op) ? 2 : 1);
                            if (instruction.op == Op::Call || instruction.op == Op::Jump || instruction.op == Op::JumpFalse) {
                                const bool call = instruction.op == Op::Call;
                                fixups.push_back({static_cast<std::uint32_t>(data.instructions.size()),
                                    call ? tokens[1].text : std::to_string(*active) + ':' + tokens[1].text, call});
                            } else if (instruction.op == Op::Host) {
                                auto found = imports.find(tokens[1].text);
                                if (found == imports.end()) {
                                    const auto host = std::find_if(hosts.begin(), hosts.end(), [&](const HostSignature& value) {
                                        return value.name == tokens[1].text;
                                    });
                                    if (host == hosts.end() || data.imports.size() >= limits.max_imports) {
                                        throw std::invalid_argument("unknown host or import limit exceeded");
                                    }
                                    found = imports.emplace(host->name, static_cast<std::uint32_t>(data.imports.size())).first;
                                    data.imports.push_back(*host);
                                }
                                instruction.operand = found->second;
                            } else if (has_operand(instruction.op)) instruction.operand = parse_integer<std::uint32_t>(tokens[1].text);
                        }
                        data.instructions.push_back(instruction);
                    }
                }
            }
            ++location.line;
            location.column = 1;
        }
        if (active) throw std::invalid_argument("missing .end");
        const auto entry_function = functions.find(entry);
        if (entry_function == functions.end()) throw std::invalid_argument("entry function not found");
        data.entry = entry_function->second;
        for (const auto& fixup : fixups) {
            location = data.instructions[fixup.instruction].source;
            const auto& table = fixup.function ? functions : labels;
            const auto found = table.find(fixup.name);
            if (found == table.end()) throw std::invalid_argument("unresolved label or function: " + fixup.name);
            data.instructions[fixup.instruction].operand = found->second;
        }
        return Program::create(std::move(data), limits);
    } catch (const std::exception& error) {
        return {{}, {error.what(), location, 0}};
    }
}

namespace {

void write_value(core::ByteWriter& writer, const Value& value, std::size_t string_limit) {
    writer.write_u8(static_cast<std::uint8_t>(value.type()));
    switch (value.type()) {
    case ValueType::Null: break;
    case ValueType::Boolean: writer.write_bool(std::get<bool>(value.data)); break;
    case ValueType::Integer: writer.write_i64(std::get<std::int64_t>(value.data)); break;
    case ValueType::Number: writer.write_f64(std::get<double>(value.data)); break;
    case ValueType::String: writer.write_string(std::get<std::string>(value.data), string_limit); break;
    case ValueType::Vector: {
        const auto vector = std::get<math::Vec3>(value.data);
        writer.write_f64(vector.x); writer.write_f64(vector.y); writer.write_f64(vector.z);
        break;
    }
    case ValueType::Entity: {
        const auto entity = std::get<ecs::Entity>(value.data);
        writer.write_u32(entity.index); writer.write_u64(entity.generation);
        break;
    }
    case ValueType::Any: throw std::invalid_argument("invalid serializable value");
    }
}

Value read_value(core::ByteReader& reader) {
    const auto type = static_cast<ValueType>(reader.read_u8());
    switch (type) {
    case ValueType::Null: return {};
    case ValueType::Boolean: return Value(reader.read_bool());
    case ValueType::Integer: return Value(reader.read_i64());
    case ValueType::Number: return Value(reader.read_f64());
    case ValueType::String: return Value(reader.read_string());
    case ValueType::Vector: {
        const double x = reader.read_f64(), y = reader.read_f64(), z = reader.read_f64();
        return Value(math::Vec3{x, y, z});
    }
    case ValueType::Entity: {
        const auto index = reader.read_u32();
        return Value(ecs::Entity{index, reader.read_u64()});
    }
    case ValueType::Any: break;
    }
    throw std::invalid_argument("invalid binary value tag");
}

std::int64_t integer_math(Op op, std::int64_t a, std::int64_t b) {
    constexpr auto low = std::numeric_limits<std::int64_t>::min();
    constexpr auto high = std::numeric_limits<std::int64_t>::max();
    if (op == Op::Add) {
        if ((b > 0 && a > high - b) || (b < 0 && a < low - b)) throw std::overflow_error("integer addition overflow");
        return a + b;
    }
    if (op == Op::Subtract) {
        if ((b > 0 && a < low + b) || (b < 0 && a > high + b)) throw std::overflow_error("integer subtraction overflow");
        return a - b;
    }
    if (op == Op::Multiply) {
        if ((a > 0 && ((b > 0 && a > high / b) || (b < 0 && b < low / a))) ||
            (a < 0 && ((b > 0 && a < low / b) || (b < 0 && a < high / b)))) {
            throw std::overflow_error("integer multiplication overflow");
        }
        return a * b;
    }
    if (b == 0) throw std::domain_error("integer division by zero");
    if (a == low && b == -1) {
        if (op == Op::Remainder) return 0;
        throw std::overflow_error("integer division overflow");
    }
    return op == Op::Remainder ? a % b : a / b;
}

template<class T>
bool compare(Op op, const T& left, const T& right) {
    if (op == Op::Less) return left < right;
    if (op == Op::LessEqual) return left <= right;
    if (op == Op::Greater) return left > right;
    return left >= right;
}

Value binary_value(Op op, const Value& left, const Value& right, const VmLimits& limits) {
    const auto result_type = binary_type(op, left.type(), right.type());
    if (op == Op::Equal || op == Op::NotEqual) return Value((left == right) == (op == Op::Equal));
    if (result_type == ValueType::Boolean) {
        if (left.type() == ValueType::Integer) return Value(compare(op, std::get<std::int64_t>(left.data), std::get<std::int64_t>(right.data)));
        if (left.type() == ValueType::Number) return Value(compare(op, std::get<double>(left.data), std::get<double>(right.data)));
        return Value(compare(op, std::get<std::string>(left.data), std::get<std::string>(right.data)));
    }
    if (result_type == ValueType::Integer) return Value(integer_math(op, std::get<std::int64_t>(left.data), std::get<std::int64_t>(right.data)));
    if (result_type == ValueType::String) {
        const auto& a = std::get<std::string>(left.data);
        const auto& b = std::get<std::string>(right.data);
        if (b.size() > limits.max_string_bytes - a.size()) throw std::length_error("string result exceeds limit");
        return Value(a + b);
    }
    Value result;
    if (result_type == ValueType::Number) {
        const auto a = std::get<double>(left.data), b = std::get<double>(right.data);
        if (op == Op::Add) result = Value(a + b);
        else if (op == Op::Subtract) result = Value(a - b);
        else if (op == Op::Multiply) result = Value(a * b);
        else {
            if (b == 0.0) throw std::domain_error("division by zero");
            result = Value(a / b);
        }
    } else if (op == Op::Add || op == Op::Subtract) {
        const auto a = std::get<math::Vec3>(left.data), b = std::get<math::Vec3>(right.data);
        result = Value(op == Op::Add ? a + b : a - b);
    } else if (left.type() == ValueType::Number) {
        result = Value(std::get<math::Vec3>(right.data) * std::get<double>(left.data));
    } else {
        const auto divisor = std::get<double>(right.data);
        if (op == Op::Divide && divisor == 0.0) throw std::domain_error("vector division by zero");
        result = Value(op == Op::Divide ? std::get<math::Vec3>(left.data) / divisor : std::get<math::Vec3>(left.data) * divisor);
    }
    if (!valid_value(result, limits)) throw std::overflow_error("nonfinite arithmetic result");
    return result;
}

bool terminal(ExecutionState state) {
    return state == ExecutionState::Completed || state == ExecutionState::Faulted || state == ExecutionState::Stopped;
}

std::uint64_t next_tick(std::uint64_t tick, std::uint64_t delay) {
    if (delay > std::numeric_limits<std::uint64_t>::max() - tick) throw std::overflow_error("wake tick overflow");
    return tick + delay;
}

struct BusyGuard {
    bool& flag;
    explicit BusyGuard(bool& value) : flag(value) { flag = true; }
    ~BusyGuard() { flag = false; }
    BusyGuard(const BusyGuard&) = delete;
    BusyGuard& operator=(const BusyGuard&) = delete;
};

} // namespace

std::vector<std::byte> encode_program(const Program& program, VmLimits limits) {
    if (const auto error = verify(program.data(), limits)) throw std::invalid_argument(error->message);
    core::ByteWriter writer(limits.max_binary_bytes);
    writer.write_bytes("GEVM"); writer.write_u16(1); writer.write_u16(0);
    const auto& data = program.data();
    writer.write_u32(data.entry);
    writer.write_u32(static_cast<std::uint32_t>(data.constants.size()));
    writer.write_u32(static_cast<std::uint32_t>(data.globals.size()));
    writer.write_u32(static_cast<std::uint32_t>(data.functions.size()));
    writer.write_u32(static_cast<std::uint32_t>(data.imports.size()));
    writer.write_u32(static_cast<std::uint32_t>(data.instructions.size()));
    for (const auto& value : data.constants) write_value(writer, value, limits.max_string_bytes);
    for (const auto& value : data.globals) write_value(writer, value, limits.max_string_bytes);
    for (const auto& function : data.functions) {
        writer.write_string(function.name, 128);
        writer.write_u32(function.begin); writer.write_u32(function.end);
        writer.write_u32(function.parameters); writer.write_u32(function.locals);
        writer.write_bool(function.returns_value);
    }
    for (const auto& host : data.imports) {
        writer.write_string(host.name, 128);
        writer.write_u32(static_cast<std::uint32_t>(host.parameters.size()));
        for (const auto type : host.parameters) writer.write_u8(static_cast<std::uint8_t>(type));
        writer.write_u8(static_cast<std::uint8_t>(host.result));
    }
    for (const auto& instruction : data.instructions) {
        writer.write_u8(static_cast<std::uint8_t>(instruction.op)); writer.write_u32(instruction.operand);
        writer.write_u32(instruction.source.line); writer.write_u32(instruction.source.column);
    }
    return writer.take();
}

CompileResult decode_program(std::span<const std::byte> bytes, VmLimits limits) {
    try {
        if (!valid_limits(limits) || bytes.size() > limits.max_binary_bytes) throw std::length_error("binary size limit exceeded");
        core::BinaryLimits binary;
        binary.max_total_bytes = limits.max_binary_bytes;
        binary.max_string_bytes = std::max<std::size_t>(128, limits.max_string_bytes);
        core::ByteReader reader(bytes, core::ByteOrder::little, binary);
        reader.expect("GEVM");
        if (reader.read_u16() != 1 || reader.read_u16() != 0) throw std::invalid_argument("unsupported VM binary version");
        ProgramData data;
        data.entry = reader.read_u32();
        const auto constants = reader.read_u32(), globals = reader.read_u32();
        const auto functions = reader.read_u32(), imports = reader.read_u32(), instructions = reader.read_u32();
        if (constants > limits.max_constants || globals > limits.max_globals || functions > limits.max_functions ||
            imports > limits.max_imports || instructions > limits.max_instructions) throw std::length_error("binary table limit exceeded");
        for (std::uint32_t i = 0; i < constants; ++i) data.constants.push_back(read_value(reader));
        for (std::uint32_t i = 0; i < globals; ++i) data.globals.push_back(read_value(reader));
        for (std::uint32_t i = 0; i < functions; ++i) {
            Function function;
            function.name = reader.read_string(); function.begin = reader.read_u32(); function.end = reader.read_u32();
            function.parameters = reader.read_u32(); function.locals = reader.read_u32(); function.returns_value = reader.read_bool();
            data.functions.push_back(std::move(function));
        }
        for (std::uint32_t i = 0; i < imports; ++i) {
            HostSignature host;
            host.name = reader.read_string();
            const auto count = reader.read_u32();
            if (count > limits.max_parameters) throw std::length_error("host parameter limit exceeded");
            for (std::uint32_t j = 0; j < count; ++j) host.parameters.push_back(static_cast<ValueType>(reader.read_u8()));
            host.result = static_cast<ValueType>(reader.read_u8());
            data.imports.push_back(std::move(host));
        }
        for (std::uint32_t i = 0; i < instructions; ++i) {
            Instruction instruction;
            instruction.op = static_cast<Op>(reader.read_u8()); instruction.operand = reader.read_u32();
            instruction.source.line = reader.read_u32(); instruction.source.column = reader.read_u32();
            data.instructions.push_back(instruction);
        }
        reader.require_end();
        return Program::create(std::move(data), limits);
    } catch (const std::exception& error) { return {{}, {error.what(), {}, 0}}; }
}

std::string_view state_name(ExecutionState state) noexcept {
    switch (state) {
    case ExecutionState::Ready: return "ready";
    case ExecutionState::Running: return "running";
    case ExecutionState::Yielded: return "yielded";
    case ExecutionState::Sleeping: return "sleeping";
    case ExecutionState::Paused: return "paused";
    case ExecutionState::Completed: return "completed";
    case ExecutionState::Faulted: return "faulted";
    case ExecutionState::Stopped: return "stopped";
    }
    return "invalid";
}

struct ScriptService::Impl {
    struct RunResult {
        std::uint64_t gas{};
        bool budget_blocked{};
    };
    struct Activation {
        std::uint32_t function{};
        std::uint32_t pc{};
        std::size_t base{};
        std::vector<Value> locals;
    };
    struct Instance {
        InstanceHandle handle{};
        ecs::Entity owner{};
        std::string name;
        std::shared_ptr<const Program> program;
        std::vector<std::shared_ptr<const HostFunction>> imports;
        std::vector<Value> stack, globals;
        std::vector<Activation> calls;
        ExecutionState state{ExecutionState::Ready};
        ExecutionState before_pause{ExecutionState::Ready};
        std::uint32_t last_pc{};
        std::size_t local_slots{};
        std::uint64_t gas_used{}, wake_tick{};
        Value result;
        ScriptError error;
        bool attached{}, auto_paused{};
    };
    core::Kernel& kernel;
    VmLimits limits;
    GenerationPool handles;
    std::vector<std::unique_ptr<Instance>> slots;
    std::map<std::string, std::shared_ptr<const Program>, std::less<>> programs;
    std::map<std::string, std::shared_ptr<const HostFunction>, std::less<>> hosts;
    VmStatistics stats;
    std::size_t cursor{};
    bool busy{};
    Impl(core::Kernel& engine, VmLimits configured)
        : kernel(engine), limits(configured), handles(configured.max_instances), slots(configured.max_instances) {}
    Instance* get(InstanceHandle handle) const {
        return handles.alive({handle.index, handle.generation}) ? slots[handle.index - 1].get() : nullptr;
    }
    void fault(Instance& instance, std::string message) {
        instance.state = ExecutionState::Faulted;
        const auto& code = instance.program->data().instructions;
        instance.error = {message.substr(0, 1024), instance.last_pc < code.size() ? code[instance.last_pc].source : SourceLocation{}, instance.last_pc};
        ++stats.faulted;
    }
    InstanceInfo info(const Instance& instance) const {
        return {instance.handle, instance.owner, instance.name, instance.state, instance.last_pc,
            instance.stack.size(), instance.calls.size(), instance.gas_used, instance.wake_tick, instance.result, instance.error};
    }
    RunResult run(Instance& instance, const core::Frame& frame, std::uint64_t budget) {
        if (terminal(instance.state) || instance.state == ExecutionState::Paused ||
            ((instance.state == ExecutionState::Sleeping || instance.state == ExecutionState::Yielded) && instance.wake_tick > frame.tick)) return {};
        std::uint64_t consumed = 0;
        std::uint32_t host_calls = 0;
        bool budget_blocked = false;
        instance.state = ExecutionState::Running;
        const auto& data = instance.program->data();
        try {
            while (consumed < budget && instance.state == ExecutionState::Running) {
                auto& activation = instance.calls.back();
                instance.last_pc = activation.pc;
                const auto instruction = data.instructions.at(activation.pc);
                const auto operand = instruction.operand;
                const auto cost = instruction.op == Op::Host ? instance.imports[operand]->gas_cost : 1;
                if (cost > limits.max_total_gas - instance.gas_used) throw std::length_error("total instruction gas exhausted");
                if (cost > budget - consumed) { budget_blocked = true; break; }
                if (instruction.op == Op::Host && host_calls >= limits.max_host_calls_per_slice) break;
                consumed += cost;
                instance.gas_used += cost;
                ++activation.pc;
                auto pop = [&]() {
                    if (instance.stack.size() <= activation.base) throw std::runtime_error("runtime stack underflow");
                    Value value = std::move(instance.stack.back());
                    instance.stack.pop_back();
                    return value;
                };
                auto push = [&](Value value) {
                    if (instance.stack.size() >= limits.max_stack) throw std::length_error("runtime stack limit exceeded");
                    instance.stack.push_back(std::move(value));
                };
                switch (instruction.op) {
                case Op::Nop: break;
                case Op::Constant: push(data.constants[operand]); break;
                case Op::LoadLocal: push(activation.locals[operand]); break;
                case Op::StoreLocal: activation.locals[operand] = pop(); break;
                case Op::LoadGlobal: push(instance.globals[operand]); break;
                case Op::StoreGlobal: {
                    auto value = pop();
                    if (value.type() != data.globals[operand].type()) throw std::invalid_argument("global store type mismatch");
                    instance.globals[operand] = std::move(value);
                    break;
                }
                case Op::Pop: (void)pop(); break;
                case Op::Duplicate: { auto value = pop(); push(value); push(std::move(value)); break; }
                case Op::Add: case Op::Subtract: case Op::Multiply: case Op::Divide: case Op::Remainder:
                case Op::Equal: case Op::NotEqual: case Op::Less: case Op::LessEqual: case Op::Greater: case Op::GreaterEqual: {
                    auto right = pop(); auto left = pop();
                    push(binary_value(instruction.op, left, right, limits));
                    break;
                }
                case Op::Not: push(Value(!std::get<bool>(pop().data))); break;
                case Op::Negate: {
                    auto value = pop();
                    if (value.type() == ValueType::Integer) value = Value(integer_math(Op::Subtract, 0, std::get<std::int64_t>(value.data)));
                    else if (value.type() == ValueType::Number) value = Value(-std::get<double>(value.data));
                    else if (value.type() == ValueType::Vector) value = Value(-std::get<math::Vec3>(value.data));
                    else throw std::invalid_argument("negate type mismatch");
                    push(std::move(value));
                    break;
                }
                case Op::Jump: activation.pc = operand; break;
                case Op::JumpFalse: if (!std::get<bool>(pop().data)) activation.pc = operand; break;
                case Op::Call: {
                    const auto& function = data.functions[operand];
                    if (instance.calls.size() >= limits.max_call_depth || function.locals > limits.max_local_slots - instance.local_slots) {
                        throw std::length_error("call depth or local storage limit exceeded");
                    }
                    if (instance.stack.size() - activation.base < function.parameters) throw std::runtime_error("missing function arguments");
                    Activation callee{operand, function.begin, instance.stack.size() - function.parameters, std::vector<Value>(function.locals)};
                    for (std::uint32_t i = 0; i < function.parameters; ++i) callee.locals[i] = instance.stack[callee.base + i];
                    instance.stack.resize(callee.base);
                    instance.calls.push_back(std::move(callee));
                    instance.local_slots += function.locals;
                    break;
                }
                case Op::Host: {
                    const auto host = instance.imports[operand];
                    const auto count = host->signature.parameters.size();
                    if (instance.stack.size() - activation.base < count) throw std::runtime_error("missing host arguments");
                    const auto begin = instance.stack.size() - count;
                    for (std::size_t i = 0; i < count; ++i) {
                        if (!compatible(host->signature.parameters[i], instance.stack[begin + i].type())) throw std::invalid_argument("host argument type mismatch");
                    }
                    HostContext context{kernel, frame, instance.owner, instance.handle};
                    ++host_calls;
                    auto result = host->callback(context, std::span<const Value>(instance.stack).subspan(begin, count));
                    if (!result.success) throw std::runtime_error(result.error.empty() ? "host rejected operation" : result.error.substr(0, 1024));
                    if (!valid_value(result.value, limits) || !compatible(host->signature.result, result.value.type())) {
                        throw std::invalid_argument("host returned invalid value or type");
                    }
                    instance.stack.resize(begin);
                    push(std::move(result.value));
                    break;
                }
                case Op::Return: {
                    const auto& function = data.functions[activation.function];
                    if (instance.stack.size() != activation.base + (function.returns_value ? 1U : 0U)) throw std::runtime_error("invalid return stack");
                    Value result = function.returns_value ? pop() : Value{};
                    instance.local_slots -= activation.locals.size();
                    instance.calls.pop_back();
                    if (instance.calls.empty()) {
                        instance.result = std::move(result);
                        instance.state = ExecutionState::Completed;
                        ++stats.completed;
                    } else if (function.returns_value) push(std::move(result));
                    break;
                }
                case Op::Yield:
                    instance.wake_tick = next_tick(frame.tick, 1);
                    instance.state = ExecutionState::Yielded;
                    break;
                case Op::Sleep: {
                    const auto delay = std::get<std::int64_t>(pop().data);
                    if (delay < 0 || static_cast<std::uint64_t>(delay) > limits.max_sleep_frames) throw std::invalid_argument("sleep duration out of bounds");
                    instance.wake_tick = next_tick(frame.tick, std::max<std::uint64_t>(1, static_cast<std::uint64_t>(delay)));
                    instance.state = ExecutionState::Sleeping;
                    break;
                }
                case Op::Halt: instance.state = ExecutionState::Completed; ++stats.completed; break;
                }
            }
            if (instance.state == ExecutionState::Running) {
                instance.state = ExecutionState::Yielded;
                instance.wake_tick = next_tick(frame.tick, 1);
            }
        } catch (const std::exception& error) { fault(instance, error.what()); }
        catch (...) { fault(instance, "native callback raised an unknown exception"); }
        return {consumed, budget_blocked};
    }
};

ScriptService::ScriptService(core::Kernel& kernel, VmLimits limits) {
    if (!valid_limits(limits)) throw std::invalid_argument("invalid VM configuration");
    impl_ = std::make_unique<Impl>(kernel, limits);
}
ScriptService::~ScriptService() = default;
const VmLimits& ScriptService::limits() const noexcept { return impl_->limits; }

bool ScriptService::register_host(HostFunction function, std::string* error) {
    const auto& signature = function.signature;
    if (impl_->busy || !valid_name(signature.name) || !function.callback ||
        function.gas_cost == 0 || function.gas_cost > limits().slice_gas ||
        signature.parameters.size() > limits().max_parameters || !valid_type(signature.result) ||
        !std::all_of(signature.parameters.begin(), signature.parameters.end(), valid_type) ||
        impl_->hosts.contains(signature.name) || impl_->hosts.size() >= limits().max_imports) {
        if (error) *error = "host registration rejected: duplicate, busy, bounds, signature, or cost";
        return false;
    }
    auto stored = std::make_shared<const HostFunction>(std::move(function));
    impl_->hosts.emplace(stored->signature.name, stored);
    return true;
}

bool ScriptService::unregister_host(std::string_view name) {
    if (impl_->busy) return false;
    const auto found = impl_->hosts.find(name);
    if (found == impl_->hosts.end()) return false;
    impl_->hosts.erase(found);
    return true;
}

std::vector<HostSignature> ScriptService::host_signatures() const {
    std::vector<HostSignature> result;
    for (const auto& [name, host] : impl_->hosts) {
        (void)name;
        result.push_back(host->signature);
    }
    return result;
}

CompileResult ScriptService::load(std::string name, std::string_view source) {
    if (impl_->busy) return {{}, {"VM is executing", {}, 0}};
    auto result = assemble(source, host_signatures(), limits());
    if (result && !install(std::move(name), result.program, &result.error.message)) result.program.reset();
    return result;
}

bool ScriptService::install(std::string name, std::shared_ptr<const Program> program_value, std::string* error) {
    if (impl_->busy || !valid_name(name) || !program_value ||
        (!impl_->programs.contains(name) && impl_->programs.size() >= limits().max_programs)) {
        if (error) *error = "program installation rejected: busy, invalid name, or capacity";
        return false;
    }
    if (const auto invalid = verify(program_value->data(), limits())) {
        if (error) *error = invalid->message;
        return false;
    }
    impl_->programs.insert_or_assign(std::move(name), std::move(program_value));
    return true;
}

bool ScriptService::unload(std::string_view name) {
    if (impl_->busy) return false;
    const auto found = impl_->programs.find(name);
    if (found == impl_->programs.end()) return false;
    impl_->programs.erase(found);
    return true;
}

std::shared_ptr<const Program> ScriptService::program(std::string_view name) const {
    const auto found = impl_->programs.find(name);
    return found == impl_->programs.end() ? nullptr : found->second;
}

InstanceHandle ScriptService::start(std::string_view name, ecs::Entity owner,
                                    std::span<const Value> arguments, std::string* error) {
    try {
        if (impl_->busy) throw std::logic_error("VM is executing");
        const auto selected = program(name);
        if (!selected) throw std::invalid_argument("program not found");
        auto* registry = impl_->kernel.services().find<ecs::Registry>();
        if (owner != ecs::Entity{} && (!registry || !registry->alive(owner))) throw std::invalid_argument("owner entity is stale");
        const auto& data = selected->data();
        const auto& entry = data.functions[data.entry];
        if (arguments.size() != entry.parameters || !std::all_of(arguments.begin(), arguments.end(),
            [&](const Value& value) { return valid_value(value, limits()); })) throw std::invalid_argument("entry arguments rejected");
        auto instance = std::make_unique<Impl::Instance>();
        instance->program = selected; instance->name = name; instance->owner = owner;
        instance->globals = data.globals; instance->local_slots = entry.locals; instance->last_pc = entry.begin;
        for (const auto& signature : data.imports) {
            const auto host = impl_->hosts.find(signature.name);
            if (host == impl_->hosts.end() || host->second->signature != signature) throw std::invalid_argument("host import missing or signature changed");
            instance->imports.push_back(host->second);
        }
        Impl::Activation activation{data.entry, entry.begin, 0, std::vector<Value>(entry.locals)};
        std::copy(arguments.begin(), arguments.end(), activation.locals.begin());
        instance->calls.push_back(std::move(activation));
        const auto handle = impl_->handles.acquire();
        if (!handle) throw std::length_error("instance capacity exhausted; release finished instances");
        instance->handle = {handle->index, handle->generation};
        const auto result = instance->handle;
        impl_->slots[handle->index - 1] = std::move(instance);
        return result;
    } catch (const std::exception& failure) {
        if (error) *error = failure.what();
        return {};
    }
}

bool ScriptService::attach(ecs::Entity entity, std::string_view name, bool auto_start) {
    if (impl_->busy || !program(name)) return false;
    auto* registry = impl_->kernel.services().find<ecs::Registry>();
    if (!registry || !registry->alive(entity)) return false;
    if (auto* component = registry->get<ScriptComponent>(entity)) {
        if (auto* old = impl_->get(component->instance); old && old->owner == entity) (void)release(old->handle);
    }
    registry->emplace_or_replace<ScriptComponent>(entity, ScriptComponent{std::string(name), {}, true, auto_start, {}});
    return true;
}

bool ScriptService::detach(ecs::Entity entity) {
    if (impl_->busy) return false;
    auto* registry = impl_->kernel.services().find<ecs::Registry>();
    auto* component = registry ? registry->get<ScriptComponent>(entity) : nullptr;
    if (!component) return false;
    if (auto* instance = impl_->get(component->instance); instance && instance->owner == entity) (void)release(instance->handle);
    return registry->remove<ScriptComponent>(entity);
}

bool ScriptService::pause(InstanceHandle handle) {
    auto* instance = impl_->get(handle);
    if (impl_->busy || !instance || terminal(instance->state)) return false;
    if (instance->state != ExecutionState::Paused) {
        instance->before_pause = instance->state;
        instance->state = ExecutionState::Paused;
    }
    instance->auto_paused = false;
    return true;
}

bool ScriptService::resume(InstanceHandle handle) {
    auto* instance = impl_->get(handle);
    if (impl_->busy || !instance || instance->state != ExecutionState::Paused) return false;
    instance->state = instance->before_pause;
    instance->auto_paused = false;
    return true;
}

bool ScriptService::stop(InstanceHandle handle) {
    auto* instance = impl_->get(handle);
    if (impl_->busy || !instance) return false;
    instance->state = ExecutionState::Stopped;
    instance->stack.clear(); instance->calls.clear(); instance->local_slots = 0;
    return true;
}

bool ScriptService::release(InstanceHandle handle) {
    auto* instance = impl_->get(handle);
    if (impl_->busy || !instance) return false;
    if (auto* registry = impl_->kernel.services().find<ecs::Registry>()) {
        if (auto* component = registry->get<ScriptComponent>(instance->owner); component && component->instance == handle) {
            component->instance = {}; component->auto_start = false;
            (void)registry->mark_changed<ScriptComponent>(instance->owner);
        }
    }
    impl_->slots[handle.index - 1].reset();
    return impl_->handles.release({handle.index, handle.generation});
}

void ScriptService::stop_all() noexcept {
    if (impl_->busy) return;
    for (auto& instance : impl_->slots) {
        if (instance) {
            instance->state = ExecutionState::Stopped;
            instance->stack.clear(); instance->calls.clear(); instance->local_slots = 0;
        }
    }
}

std::uint64_t ScriptService::step(InstanceHandle handle, std::uint64_t gas) {
    auto* instance = impl_->get(handle);
    if (impl_->busy || !instance || terminal(instance->state) || gas == 0) return 0;
    const bool paused = instance->state == ExecutionState::Paused;
    if (paused) instance->state = instance->before_pause;
    if (instance->state == ExecutionState::Yielded) instance->state = ExecutionState::Ready;
    const BusyGuard guard(impl_->busy);
    const auto used = impl_->run(*instance, impl_->kernel.frame(), std::min(gas, limits().slice_gas)).gas;
    if (paused && !terminal(instance->state)) {
        instance->before_pause = instance->state;
        instance->state = ExecutionState::Paused;
    }
    return used;
}

void ScriptService::advance(const core::Frame& frame) {
    if (impl_->busy) return;
    auto* registry = impl_->kernel.services().find<ecs::Registry>();
    if (registry) {
        for (auto& instance : impl_->slots) {
            if (!instance || !instance->owner) continue;
            const auto* component = registry->get<ScriptComponent>(instance->owner);
            if (!registry->alive(instance->owner) || (instance->attached &&
                (!component || component->instance != instance->handle))) (void)release(instance->handle);
        }
        for (const auto entity : registry->view<ScriptComponent>(true)) {
            auto* component = registry->get<ScriptComponent>(entity);
            auto* instance = impl_->get(component->instance);
            if (instance && (instance->owner != entity || instance->name != component->program)) {
                if (instance->owner == entity) {
                    (void)release(instance->handle);
                    component->auto_start = true;
                }
                component->instance = {};
                instance = nullptr;
            }
            const bool enabled = component->enabled && registry->enabled(entity);
            if (!instance && enabled && component->auto_start) {
                component->instance = start(component->program, entity, {}, &component->last_error);
                instance = impl_->get(component->instance);
                if (instance) { instance->attached = true; component->last_error.clear(); }
                (void)registry->mark_changed<ScriptComponent>(entity);
            }
            if (!instance || terminal(instance->state)) continue;
            if (!enabled && instance->state != ExecutionState::Paused) {
                (void)pause(instance->handle);
                instance->auto_paused = true;
            } else if (enabled && instance->auto_paused) (void)resume(instance->handle);
        }
    }
    const BusyGuard guard(impl_->busy);
    impl_->stats.frame_gas = 0;
    for (std::size_t visited = 0; visited < impl_->slots.size() && impl_->stats.frame_gas < limits().frame_gas; ++visited) {
        const auto index = impl_->cursor;
        impl_->cursor = (impl_->cursor + 1) % impl_->slots.size();
        if (auto& instance = impl_->slots[index]) {
            const auto budget = std::min(limits().slice_gas, limits().frame_gas - impl_->stats.frame_gas);
            const auto result = impl_->run(*instance, frame, budget);
            impl_->stats.frame_gas += result.gas;
            if (result.budget_blocked && budget < limits().slice_gas && !terminal(instance->state)) {
                // Give a call that cannot fit the final partial slice a full
                // slice next frame instead of repeatedly starving it.
                impl_->cursor = index;
                break;
            }
        }
    }
}

std::optional<InstanceInfo> ScriptService::inspect(InstanceHandle handle) const {
    const auto* instance = impl_->get(handle);
    return instance ? std::optional<InstanceInfo>(impl_->info(*instance)) : std::nullopt;
}

std::vector<InstanceInfo> ScriptService::instances() const {
    std::vector<InstanceInfo> result;
    for (const auto& instance : impl_->slots) if (instance) result.push_back(impl_->info(*instance));
    return result;
}

std::optional<Value> ScriptService::global(InstanceHandle handle, std::size_t index) const {
    const auto* instance = impl_->get(handle);
    if (!instance || index >= instance->globals.size()) return std::nullopt;
    return instance->globals[index];
}

bool ScriptService::set_global(InstanceHandle handle, std::size_t index, Value value) {
    auto* instance = impl_->get(handle);
    if (impl_->busy || !instance || index >= instance->globals.size() || !valid_value(value, limits()) ||
        value.type() != instance->globals[index].type()) return false;
    instance->globals[index] = std::move(value);
    return true;
}

VmStatistics ScriptService::statistics() const {
    auto result = impl_->stats;
    result.programs = impl_->programs.size(); result.instances = impl_->handles.size(); result.runnable = 0;
    for (const auto& instance : impl_->slots) {
        if (instance && !terminal(instance->state) && instance->state != ExecutionState::Paused) ++result.runnable;
    }
    return result;
}

namespace {

ecs::Registry& host_registry(HostContext& context) {
    auto* registry = context.kernel.services().find<ecs::Registry>();
    if (!registry) throw std::runtime_error("shared registry is unavailable");
    return *registry;
}

scene::Scene& host_scene(HostContext& context) {
    auto* scene_value = context.kernel.services().find<scene::Scene>();
    if (!scene_value || &scene_value->registry() != &host_registry(context)) throw std::runtime_error("shared scene is unavailable");
    return *scene_value;
}

physics::PhysicsWorld& host_physics(HostContext& context) {
    auto* world = context.kernel.services().find<physics::PhysicsWorld>();
    if (!world || &world->registry() != &host_registry(context)) throw std::runtime_error("shared physics world is unavailable");
    return *world;
}

void install_engine_hosts(ScriptService& service) {
    auto add = [&](std::string name, std::vector<ValueType> parameters, ValueType result,
                   std::uint64_t cost, std::function<HostResult(HostContext&, std::span<const Value>)> callback) {
        std::string error;
        if (!service.register_host({{std::move(name), std::move(parameters), result}, cost, std::move(callback)}, &error)) {
            throw std::runtime_error(error);
        }
    };
    using T = ValueType;
    add("entity.self", {}, T::Entity, 1, [](HostContext& context, std::span<const Value>) {
        return HostResult{true, Value(context.owner), {}};
    });
    add("entity.find", {T::String}, T::Entity, 32, [](HostContext& context, std::span<const Value> args) {
        return HostResult{true, Value(host_registry(context).find_name(std::get<std::string>(args[0].data))), {}};
    });
    add("entity.alive", {T::Entity}, T::Boolean, 2, [](HostContext& context, std::span<const Value> args) {
        return HostResult{true, Value(host_registry(context).alive(std::get<ecs::Entity>(args[0].data))), {}};
    });
    add("entity.enabled", {T::Entity}, T::Boolean, 2, [](HostContext& context, std::span<const Value> args) {
        return HostResult{true, Value(host_registry(context).enabled(std::get<ecs::Entity>(args[0].data))), {}};
    });
    add("entity.enable", {T::Entity, T::Boolean}, T::Boolean, 4, [](HostContext& context, std::span<const Value> args) {
        return HostResult{true, Value(host_registry(context).set_enabled(std::get<ecs::Entity>(args[0].data), std::get<bool>(args[1].data))), {}};
    });
    add("scene.find", {T::String}, T::Entity, 32, [](HostContext& context, std::span<const Value> args) {
        const auto found = host_scene(context).find_path(std::get<std::string>(args[0].data), context.owner);
        return HostResult{true, Value(found ? found.entity : ecs::Entity{}), {}};
    });
    add("scene.position", {T::Entity}, T::Vector, 32, [](HostContext& context, std::span<const Value> args) {
        const auto transform = host_scene(context).world_transform(std::get<ecs::Entity>(args[0].data));
        if (!transform) return HostResult{false, {}, "entity has no valid world transform"};
        return HostResult{true, Value(transform->position), {}};
    });
    add("scene.local_position", {T::Entity}, T::Vector, 4, [](HostContext& context, std::span<const Value> args) {
        const auto transform = host_scene(context).local_transform(std::get<ecs::Entity>(args[0].data));
        if (!transform) return HostResult{false, {}, "entity has no local transform"};
        return HostResult{true, Value(transform->position), {}};
    });
    add("scene.set_position", {T::Entity, T::Vector}, T::Boolean, 32, [](HostContext& context, std::span<const Value> args) {
        const auto entity = std::get<ecs::Entity>(args[0].data);
        if (host_registry(context).has<physics::BodyComponent>(entity)) return HostResult{false, {}, "physics body requires physics.teleport"};
        return HostResult{true, Value(static_cast<bool>(host_scene(context).set_world_position(entity, std::get<math::Vec3>(args[1].data)))), {}};
    });
    add("scene.translate", {T::Entity, T::Vector}, T::Boolean, 32, [](HostContext& context, std::span<const Value> args) {
        const auto entity = std::get<ecs::Entity>(args[0].data);
        if (host_registry(context).has<physics::BodyComponent>(entity)) return HostResult{false, {}, "physics body requires physics.teleport"};
        return HostResult{true, Value(static_cast<bool>(host_scene(context).translate(entity, std::get<math::Vec3>(args[1].data), scene::TransformSpace::World))), {}};
    });
    add("physics.teleport", {T::Entity, T::Vector, T::Boolean}, T::Boolean, 32, [](HostContext& context, std::span<const Value> args) {
        auto& world = host_physics(context);
        const auto* body = world.get(std::get<ecs::Entity>(args[0].data));
        if (!body) return HostResult{false, {}, "entity has no physics body"};
        auto pose = body->pose;
        pose.position = std::get<math::Vec3>(args[1].data);
        const auto handle = body->handle;
        return HostResult{true, Value(world.set_pose(handle, pose, std::get<bool>(args[2].data))), {}};
    });
    add("physics.impulse", {T::Entity, T::Vector}, T::Boolean, 16, [](HostContext& context, std::span<const Value> args) {
        auto& world = host_physics(context);
        return HostResult{true, Value(world.apply_impulse(world.find(std::get<ecs::Entity>(args[0].data)), std::get<math::Vec3>(args[1].data))), {}};
    });
    add("time.delta", {}, T::Number, 1, [](HostContext& context, std::span<const Value>) {
        return HostResult{true, Value(context.frame.delta_seconds), {}};
    });
    add("time.elapsed", {}, T::Number, 1, [](HostContext& context, std::span<const Value>) {
        return HostResult{true, Value(context.frame.elapsed_seconds), {}};
    });
    add("time.tick", {}, T::Integer, 1, [](HostContext& context, std::span<const Value>) {
        if (context.frame.tick > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) return HostResult{false, {}, "frame index exceeds script integer range"};
        return HostResult{true, Value(static_cast<std::int64_t>(context.frame.tick)), {}};
    });
    add("math.vec3", {T::Number, T::Number, T::Number}, T::Vector, 2, [](HostContext&, std::span<const Value> args) {
        return HostResult{true, Value(math::Vec3{std::get<double>(args[0].data), std::get<double>(args[1].data), std::get<double>(args[2].data)}), {}};
    });
    const std::array<std::string, 3> axes{"math.x", "math.y", "math.z"};
    for (std::size_t axis = 0; axis < axes.size(); ++axis) {
        add(axes[axis], {T::Vector}, T::Number, 1, [axis](HostContext&, std::span<const Value> args) {
            return HostResult{true, Value(std::get<math::Vec3>(args[0].data)[axis]), {}};
        });
    }
    add("math.dot", {T::Vector, T::Vector}, T::Number, 2, [](HostContext&, std::span<const Value> args) {
        return HostResult{true, Value(math::dot(std::get<math::Vec3>(args[0].data), std::get<math::Vec3>(args[1].data))), {}};
    });
    add("math.length", {T::Vector}, T::Number, 3, [](HostContext&, std::span<const Value> args) {
        return HostResult{true, Value(math::length(std::get<math::Vec3>(args[0].data))), {}};
    });
}

InstanceHandle parse_instance(std::string_view text) {
    if (const auto handle = ecs::parse_entity(text)) return {handle->index, handle->generation};
    return {};
}

ecs::Entity command_entity(core::Kernel& kernel, std::string_view text) {
    auto* registry = kernel.services().find<ecs::Registry>();
    if (!registry) return {};
    if (const auto entity = ecs::parse_entity(text)) return registry->alive(*entity) ? *entity : ecs::Entity{};
    return registry->find_name(text);
}

std::string instance_text(const InstanceInfo& info) {
    std::ostringstream stream;
    stream << info.handle.index << ':' << info.handle.generation << ' ' << info.program << ' '
           << state_name(info.state) << " pc=" << info.instruction << " gas=" << info.gas_used
           << " stack=" << info.stack_size << " calls=" << info.call_depth;
    if (info.state == ExecutionState::Sleeping) stream << " wake=" << info.wake_tick;
    if (info.state == ExecutionState::Completed) stream << " result=" << info.result.display();
    if (!info.error.message.empty()) stream << " error=" << info.error.source.line << ':' << info.error.source.column << ' ' << info.error.message;
    return stream.str();
}

} // namespace

void register_vm(core::Kernel& kernel) {
    if (!kernel.register_module({"script.vm", "Bounded typed cooperative bytecode scripting", 1})) return;
    VmLimits configured;
    configured.max_instances = static_cast<std::uint32_t>(std::clamp<std::int64_t>(kernel.config().integer("script.max_instances", 256), 1, 4096));
    configured.slice_gas = static_cast<std::uint64_t>(std::clamp<std::int64_t>(kernel.config().integer("script.slice_gas", 2048), 64, 65536));
    configured.frame_gas = static_cast<std::uint64_t>(std::clamp<std::int64_t>(kernel.config().integer("script.frame_gas", 65536), 65536, 1000000));
    auto& service = kernel.services().emplace<ScriptService>(kernel, configured);
    install_engine_hosts(service);
    if (auto* registry = kernel.services().find<ecs::Registry>()) (void)registry->register_component<ScriptComponent>("Script");
    (void)kernel.add_system({"script.vm.advance", core::Phase::Simulation, -500, {}}, [](core::Kernel& engine, const core::Frame& frame) {
        auto& vm = engine.services().get<ScriptService>();
        vm.advance(frame);
        const auto statistics = vm.statistics();
        engine.metric("script.instances", static_cast<double>(statistics.instances));
        engine.metric("script.frame_gas", static_cast<double>(statistics.frame_gas));
        engine.metric("script.faulted", static_cast<double>(statistics.faulted));
    });
    (void)kernel.add_shutdown("script.vm.stop", [service_ptr = &service] { service_ptr->stop_all(); });
    (void)kernel.add_command("script.load", [](core::Kernel& engine, core::CommandArgs args) -> core::CommandResult {
        if (args.size() < 2 || args.size() > 3 || (args.size() == 3 && args[2] != "binary")) return {false, "usage: script.load NAME VFS_PATH [binary]"};
        auto* filesystem = engine.services().find<io::VirtualFileSystem>();
        if (!filesystem) return {false, "virtual filesystem is unavailable"};
        auto& vm = engine.services().get<ScriptService>();
        try {
            CompileResult result;
            if (args.size() == 3) {
                result = decode_program(filesystem->read(args[1], vm.limits().max_binary_bytes), vm.limits());
                if (result && !vm.install(args[0], result.program, &result.error.message)) result.program.reset();
            } else result = vm.load(args[0], filesystem->read_text(args[1], vm.limits().max_source_bytes));
            if (!result) return {false, std::to_string(result.error.source.line) + ':' + std::to_string(result.error.source.column) + " " + result.error.message};
            return {true, "loaded " + args[0] + " instructions=" + std::to_string(result.program->data().instructions.size())};
        } catch (const std::exception& error) { return {false, error.what()}; }
    }, "Load bounded assembly or GEVM binary through the mounted virtual filesystem");
    (void)kernel.add_command("script.start", [](core::Kernel& engine, core::CommandArgs args) -> core::CommandResult {
        if (args.empty() || args.size() > 2) return {false, "usage: script.start NAME [OWNER_ENTITY]"};
        const auto owner = args.size() == 2 ? command_entity(engine, args[1]) : ecs::Entity{};
        if (args.size() == 2 && !owner) return {false, "owner entity not found"};
        std::string error;
        const auto instance = engine.services().get<ScriptService>().start(args[0], owner, {}, &error);
        return instance ? core::CommandResult{true, ecs::format_entity({instance.index, instance.generation})} : core::CommandResult{false, error};
    }, "Start a loaded program with an optional existing owner entity");
    using Control = bool (ScriptService::*)(InstanceHandle);
    const std::array<std::pair<std::string_view, Control>, 4> controls{{
        {"script.pause", &ScriptService::pause}, {"script.resume", &ScriptService::resume},
        {"script.stop", &ScriptService::stop}, {"script.release", &ScriptService::release}
    }};
    for (const auto& [name, operation] : controls) {
        (void)kernel.add_command(std::string(name), [operation](core::Kernel& engine, core::CommandArgs args) -> core::CommandResult {
            if (args.size() != 1) return {false, "expected INSTANCE_INDEX:GENERATION"};
            const bool success = (engine.services().get<ScriptService>().*operation)(parse_instance(args[0]));
            return {success, success ? "instance updated" : "stale handle or state rejected"};
        }, "Control a generation-checked script instance");
    }
    (void)kernel.add_command("script.step", [](core::Kernel& engine, core::CommandArgs args) -> core::CommandResult {
        if (args.empty() || args.size() > 2) return {false, "usage: script.step HANDLE [GAS]"};
        auto& vm = engine.services().get<ScriptService>();
        const auto handle = parse_instance(args[0]);
        if (!vm.inspect(handle)) return {false, "stale instance handle"};
        try {
            const auto gas = args.size() == 2 ? parse_integer<std::uint64_t>(args[1]) : 1;
            const auto used = vm.step(handle, gas);
            const auto state = vm.inspect(handle);
            return {state && state->state != ExecutionState::Faulted, "gas=" + std::to_string(used) + " " + (state ? instance_text(*state) : "released")};
        } catch (const std::exception& error) { return {false, error.what()}; }
    }, "Execute a bounded debugger slice; sleeping instances still honor their wake frame");
    (void)kernel.add_command("script.state", [](core::Kernel& engine, core::CommandArgs args) -> core::CommandResult {
        auto& vm = engine.services().get<ScriptService>();
        if (args.size() > 1) return {false, "usage: script.state [HANDLE]"};
        if (args.size() == 1) {
            const auto info = vm.inspect(parse_instance(args[0]));
            return info ? core::CommandResult{true, instance_text(*info)} : core::CommandResult{false, "stale instance handle"};
        }
        std::string output;
        for (const auto& info : vm.instances()) output += instance_text(info) + '\n';
        return {true, output.empty() ? "no script instances" : output};
    }, "Inspect execution state, resource use, return values, and source-located faults");
    (void)kernel.add_command("script.attach", [](core::Kernel& engine, core::CommandArgs args) -> core::CommandResult {
        if (args.size() != 2) return {false, "usage: script.attach ENTITY PROGRAM"};
        const bool attached = engine.services().get<ScriptService>().attach(command_entity(engine, args[0]), args[1]);
        return {attached, attached ? "script component attached" : "entity or loaded program unavailable"};
    }, "Attach a script component to an existing registry entity; starts on the next VM frame");
    (void)kernel.add_command("script.detach", [](core::Kernel& engine, core::CommandArgs args) -> core::CommandResult {
        if (args.size() != 1) return {false, "usage: script.detach ENTITY"};
        const bool detached = engine.services().get<ScriptService>().detach(command_entity(engine, args[0]));
        return {detached, detached ? "script component detached" : "script component unavailable"};
    }, "Release the entity-owned instance and remove its script component");
    (void)kernel.add_command("script.unload", [](core::Kernel& engine, core::CommandArgs args) -> core::CommandResult {
        if (args.size() != 1) return {false, "usage: script.unload PROGRAM"};
        const bool unloaded = engine.services().get<ScriptService>().unload(args[0]);
        return {unloaded, unloaded ? "program removed; running instances retain their version" : "program not found"};
    }, "Remove a named program without invalidating active instances");
}

} // namespace ge::script
