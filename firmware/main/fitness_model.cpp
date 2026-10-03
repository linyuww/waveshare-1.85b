#include "fitness_model.hpp"
#include <algorithm>

namespace fitness {
const std::array<Exercise, exercise_count> exercises = {{
    {"平板卧推", "肩背贴凳，双脚支撑；杠铃平稳下降，再向上推起。"},
    {"上斜推胸", "背部贴斜凳；双手沿胸部上方向前上方推起。"},
    {"高位下拉", "坐稳并固定大腿；双肘向下拉，缓慢回到上方。"},
    {"坐姿划船", "躯干稳定；双肘向后，手柄拉向腹部，再缓慢回伸。"},
    {"哑铃推肩", "坐稳，手肘在身体两侧；哑铃向上推起，再缓慢下降。"},
    {"哑铃侧平举", "肘部微屈；双臂向两侧抬起，避免摆动躯干。"},
    {"蝴蝶机反向飞鸟", "胸部贴靠垫；双臂向两侧后方打开，再缓慢合拢。"},
    {"杠铃弯举", "上臂保持稳定；屈肘抬起杠铃，再缓慢放下。"},
    {"直杆绳索弯举", "面向低位滑轮；上臂稳定，屈肘拉起直杆。"},
    {"三头绳索下压", "上臂贴近躯干；伸肘向下压绳索，再缓慢回收。"},
    {"杠铃深蹲", "双脚稳定；髋膝一起弯曲，下蹲后平稳站起。"},
    {"坐姿腿屈伸", "背部贴靠垫；膝关节伸展抬起滚垫，再缓慢下降。"},
    {"倒蹬机", "背髋贴靠垫；屈膝回收踏板，再平稳蹬出。"},
    {"仰卧卷腹", "屈膝仰卧；胸部向骨盆方向卷起，再缓慢放下。"},
}};
const std::array<const char *, day_count> day_names = {"周一 · 胸", "周二 · 背", "周三 · 肩", "周四 · 手臂", "周五 · 腿"};

Model::Model(Storage &storage) : storage_(storage)
{
    state_.settings.plans = {{{{0, 1, 13, no_exercise}}, {{2, 3, 13, no_exercise}},
        {{4, 5, 6, 13}}, {{7, 8, 9, 13}}, {{10, 11, 12, 13}}}};
}
bool Model::load()
{
    std::vector<uint8_t> bytes;
    const auto result = storage_.read(bytes);
    if (result == Storage::Load::Missing) return true;
    State loaded;
    if (result == Storage::Load::Failed || !decode(bytes, loaded)) {
        save_failed_ = true;
        return false;
    }
    state_ = loaded;
    if (state_.session.rest == Rest::Running) state_.session.rest = Rest::Paused;
    return true;
}
uint32_t Model::remaining(uint64_t now) const
{
    if (state_.session.rest == Rest::Running)
        return deadline_ > now ? static_cast<uint32_t>(deadline_ - now) : 0;
    return state_.session.remaining_ms;
}
bool Model::persist(uint64_t now)
{
    State stored = state_;
    stored.session.remaining_ms = remaining(now);
    save_failed_ = !storage_.write(encode(stored));
    return !save_failed_;
}
bool Model::retrySave(uint64_t now) { return persist(now); }
bool Model::checkpoint(uint64_t now) { return persist(now); }
bool Model::start(uint8_t day, int64_t synced_epoch, uint64_t now)
{
    if (day >= day_count || state_.session.active) return false;
    Session session;
    session.active = true;
    session.rest_seconds = state_.settings.rest_seconds;
    session.record.day = day;
    session.record.started = std::max<int64_t>(0, synced_epoch);
    for (const auto exercise : state_.settings.plans[day].moves) {
        if (exercise == no_exercise) continue;
        const auto index = session.record.count++;
        session.record.moves[index] = exercise;
        session.record.goals[index] = state_.settings.goals[exercise];
    }
    state_.session = session;
    persist(now);
    return true;
}
bool Model::tick(uint64_t now)
{
    auto &session = state_.session;
    if (session.active && session.rest == Rest::Running && remaining(now) == 0) {
        session.rest = Rest::None;
        session.remaining_ms = 0;
        persist(now);
        return true;
    }
    return false;
}
bool Model::countSet(uint64_t now)
{
    tick(now);
    auto &session = state_.session;
    if (!session.active || session.rest != Rest::None ||
        session.record.done[session.index] >= session.record.goals[session.index].sets) return false;
    ++session.record.done[session.index];
    session.undo_index = static_cast<int8_t>(session.index);
    if (session.index + 1 == session.record.count && session.record.done[session.index] == session.record.goals[session.index].sets)
        return finish(now);
    session.rest = Rest::Running;
    session.remaining_ms = session.rest_seconds * 1000;
    deadline_ = now + session.remaining_ms;
    persist(now);
    return true;
}
bool Model::undo(uint64_t now)
{
    auto &session = state_.session;
    if (!session.active || session.undo_index < 0) return false;
    session.index = static_cast<uint8_t>(session.undo_index);
    --session.record.done[session.index];
    session.undo_index = -1;
    session.rest = Rest::None;
    session.remaining_ms = 0;
    persist(now);
    return true;
}
bool Model::pauseRest(uint64_t now)
{
    tick(now);
    auto &session = state_.session;
    if (!session.active || session.rest != Rest::Running) return false;
    session.remaining_ms = remaining(now);
    session.rest = Rest::Paused;
    persist(now);
    return true;
}
bool Model::resumeRest(uint64_t now)
{
    auto &session = state_.session;
    if (!session.active || session.rest != Rest::Paused) return false;
    deadline_ = now + session.remaining_ms;
    session.rest = Rest::Running;
    persist(now);
    tick(now);
    return true;
}
bool Model::skipRest(uint64_t now)
{
    auto &session = state_.session;
    if (!session.active || session.rest == Rest::None) return false;
    session.rest = Rest::None;
    session.remaining_ms = 0;
    persist(now);
    return true;
}
bool Model::next(uint64_t now)
{
    tick(now);
    auto &session = state_.session;
    if (!session.active || session.rest != Rest::None ||
        session.record.done[session.index] != session.record.goals[session.index].sets) return false;
    if (session.index + 1 == session.record.count) return finish(now);
    ++session.index;
    persist(now);
    return true;
}
bool Model::finish(uint64_t now)
{
    auto &session = state_.session;
    if (!session.active) return false;
    session.record.complete = true;
    for (size_t index = 0; index < session.record.count; ++index)
        if (session.record.done[index] != session.record.goals[index].sets) session.record.complete = false;
    state_.history[state_.history_head] = session.record;
    state_.history_head = (state_.history_head + 1) % history_capacity;
    if (state_.history_size < history_capacity) ++state_.history_size;
    session.active = false;
    session.rest = Rest::None;
    session.remaining_ms = 0;
    session.undo_index = -1;
    persist(now);
    return true;
}
bool Model::setGoal(uint8_t exercise, Goal goal, uint64_t now)
{
    if (exercise >= exercise_count || goal.sets < 1 || goal.sets > 10 || goal.reps < 1 || goal.reps > 50) return false;
    state_.settings.goals[exercise] = goal;
    persist(now);
    return true;
}
bool Model::setRest(uint16_t seconds, uint64_t now)
{
    if (std::find(rest_options.begin(), rest_options.end(), seconds) == rest_options.end()) return false;
    state_.settings.rest_seconds = seconds;
    persist(now);
    return true;
}
bool Model::setPlan(uint8_t day, Plan plan, uint64_t now)
{
    if (day >= day_count || plan.moves[0] == no_exercise) return false;
    bool empty = false;
    std::array<bool, exercise_count> seen{};
    for (const auto exercise : plan.moves) {
        if (exercise == no_exercise) { empty = true; continue; }
        if (empty || exercise >= exercise_count || seen[exercise]) return false;
        seen[exercise] = true;
    }
    state_.settings.plans[day] = plan;
    persist(now);
    return true;
}
const Record *Model::history(size_t newest_index) const
{
    if (newest_index >= state_.history_size) return nullptr;
    return &state_.history[(state_.history_head + history_capacity - 1 - newest_index) % history_capacity];
}
int Model::today(int weekday, bool time_valid) { return !time_valid ? -1 : weekday >= 1 && weekday <= 5 ? weekday - 1 : -1; }

namespace {
uint32_t hash(const std::vector<uint8_t> &bytes, size_t size)
{
    uint32_t value = 2166136261u;
    for (size_t index = 0; index < size; ++index) value = (value ^ bytes[index]) * 16777619u;
    return value;
}
struct Codec {
    std::vector<uint8_t> &bytes;
    size_t offset = 0;
    bool reading = false;
    bool valid = true;
    void number(uint64_t &value, size_t width) {
        if (reading) {
            value = 0;
            if (offset + width > bytes.size()) { valid = false; return; }
            for (size_t index = 0; index < width; ++index) value |= uint64_t(bytes[offset++]) << (index * 8);
        } else {
            for (size_t index = 0; index < width; ++index) bytes.push_back(static_cast<uint8_t>(value >> (index * 8)));
        }
    }
    template<class Type> void field(Type &value, size_t width = 1) {
        uint64_t converted = static_cast<uint64_t>(value);
        number(converted, width);
        if (reading) value = static_cast<Type>(converted);
    }
    void record(Record &record) {
        field(record.started, 8); field(record.day); field(record.count); field(record.complete);
        for (auto &move : record.moves) field(move);
        for (auto &goal : record.goals) { field(goal.sets); field(goal.reps); }
        for (auto &done : record.done) field(done);
    }
    void state(State &state) {
        for (auto &goal : state.settings.goals) { field(goal.sets); field(goal.reps); }
        for (auto &plan : state.settings.plans) for (auto &move : plan.moves) field(move);
        field(state.settings.rest_seconds, 2);
        auto &session = state.session;
        record(session.record); field(session.active); field(session.index); field(session.undo_index);
        field(session.rest_seconds, 2); field(session.rest); field(session.remaining_ms, 4);
        field(state.history_size); field(state.history_head);
        for (auto &entry : state.history) record(entry);
    }
};
bool validRecord(const Record &record)
{
    if (record.count < 1 || record.count > max_moves || record.day >= day_count || record.started < 0) return false;
    std::array<bool, exercise_count> seen{};
    bool complete = true;
    for (size_t index = 0; index < record.count; ++index) {
        const auto move = record.moves[index];
        const auto goal = record.goals[index];
        if (move >= exercise_count || seen[move] || goal.sets < 1 || goal.sets > 10 ||
            goal.reps < 1 || goal.reps > 50 || record.done[index] > goal.sets) return false;
        seen[move] = true;
        if (record.done[index] != goal.sets) complete = false;
    }
    return !record.complete || complete;
}
}
std::vector<uint8_t> Model::encode(const State &state)
{
    std::vector<uint8_t> bytes = {'F', 'I', 'T', 1};
    State copy = state;
    Codec codec{bytes};
    codec.state(copy);
    auto checksum = hash(bytes, bytes.size());
    codec.field(checksum, 4);
    return bytes;
}
bool Model::decode(const std::vector<uint8_t> &bytes, State &state)
{
    if (bytes.size() < 8 || bytes[0] != 'F' || bytes[1] != 'I' || bytes[2] != 'T' || bytes[3] != 1) return false;
    auto copy = bytes;
    State result;
    Codec codec{copy, 4, true};
    codec.state(result);
    uint32_t checksum = 0;
    codec.field(checksum, 4);
    if (!codec.valid || codec.offset != bytes.size() || checksum != hash(bytes, bytes.size() - 4)) return false;
    for (const auto goal : result.settings.goals)
        if (goal.sets < 1 || goal.sets > 10 || goal.reps < 1 || goal.reps > 50) return false;
    for (const auto &plan : result.settings.plans) {
        bool empty = false;
        std::array<bool, exercise_count> seen{};
        if (plan.moves[0] == no_exercise) return false;
        for (const auto move : plan.moves) {
            if (move == no_exercise) { empty = true; continue; }
            if (empty || move >= exercise_count || seen[move]) return false;
            seen[move] = true;
        }
    }
    const auto validRest = [](uint16_t seconds) {
        return std::find(rest_options.begin(), rest_options.end(), seconds) != rest_options.end();
    };
    const auto &session = result.session;
    if (!validRest(result.settings.rest_seconds) || !validRest(session.rest_seconds) ||
        result.history_size > history_capacity || result.history_head >= history_capacity ||
        session.rest > Rest::Paused || session.remaining_ms > session.rest_seconds * 1000u) return false;
    if (session.active && (!validRecord(session.record) || session.index >= session.record.count ||
        session.undo_index < -1 || session.undo_index >= session.record.count ||
        (session.undo_index >= 0 && session.record.done[session.undo_index] == 0))) return false;
    if (!session.active && session.rest != Rest::None) return false;
    for (size_t index = 0; index < result.history_size; ++index)
        if (!validRecord(result.history[(result.history_head + history_capacity - 1 - index) % history_capacity])) return false;
    state = result;
    return true;
}
}
