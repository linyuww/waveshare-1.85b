#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace fitness {
constexpr size_t exercise_count = 14;
constexpr size_t day_count = 5;
constexpr size_t max_moves = 4;
constexpr size_t history_capacity = 30;
constexpr uint8_t no_exercise = 255;
constexpr std::array<uint16_t, 5> rest_options = {30, 60, 90, 120, 180};
struct Exercise { const char *name; const char *cue; };
extern const std::array<Exercise, exercise_count> exercises;
extern const std::array<const char *, day_count> day_names;
struct Goal { uint8_t sets = 5; uint8_t reps = 12; };
struct Plan { std::array<uint8_t, max_moves> moves = {no_exercise, no_exercise, no_exercise, no_exercise}; };
struct Settings {
    std::array<Goal, exercise_count> goals{};
    std::array<Plan, day_count> plans{};
    uint16_t rest_seconds = 90;
};
struct Record {
    int64_t started = 0;
    uint8_t day = 0;
    uint8_t count = 0;
    bool complete = false;
    std::array<uint8_t, max_moves> moves{};
    std::array<Goal, max_moves> goals{};
    std::array<uint8_t, max_moves> done{};
};
enum class Rest : uint8_t { None, Running, Paused };
struct Session {
    Record record{};
    bool active = false;
    uint8_t index = 0;
    int8_t undo_index = -1;
    uint16_t rest_seconds = 90;
    Rest rest = Rest::None;
    uint32_t remaining_ms = 0;
};
struct State {
    Settings settings{};
    Session session{};
    std::array<Record, history_capacity> history{};
    uint8_t history_size = 0;
    uint8_t history_head = 0;
};
class Storage {
public:
    enum class Load { Missing, Loaded, Failed };
    virtual ~Storage() = default;
    virtual Load read(std::vector<uint8_t> &bytes) = 0;
    virtual bool write(const std::vector<uint8_t> &bytes) = 0;
};
class Model {
public:
    explicit Model(Storage &storage);
    bool load();
    const State &state() const { return state_; }
    bool saveFailed() const { return save_failed_; }
    bool retrySave(uint64_t now);
    bool start(uint8_t day, int64_t synced_epoch, uint64_t now);
    bool countSet(uint64_t now);
    bool undo(uint64_t now);
    bool pauseRest(uint64_t now);
    bool resumeRest(uint64_t now);
    bool skipRest(uint64_t now);
    bool next(uint64_t now);
    bool finish(uint64_t now);
    bool tick(uint64_t now);
    bool checkpoint(uint64_t now);
    uint32_t remaining(uint64_t now) const;
    bool setGoal(uint8_t exercise, Goal goal, uint64_t now);
    bool setRest(uint16_t seconds, uint64_t now);
    bool setPlan(uint8_t day, Plan plan, uint64_t now);
    const Record *history(size_t newest_index) const;
    static int today(int weekday, bool time_valid);
    static std::vector<uint8_t> encode(const State &state);
    static bool decode(const std::vector<uint8_t> &bytes, State &state);
private:
    bool persist(uint64_t now);
    State state_{};
    Storage &storage_;
    uint64_t deadline_ = 0;
    bool save_failed_ = false;
};
}
