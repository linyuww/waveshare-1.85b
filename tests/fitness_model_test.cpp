#include "fitness_model.hpp"
#include <cassert>
#include <cstdio>

class MemoryStorage final : public fitness::Storage {
public:
    std::vector<uint8_t> bytes;
    bool fail = false;
    bool read_fail = false;
    size_t writes = 0;
    Load read(std::vector<uint8_t> &output) override {
        output = bytes;
        return read_fail ? Load::Failed : bytes.empty() ? Load::Missing : Load::Loaded;
    }
    bool write(const std::vector<uint8_t> &input) override {
        ++writes;
        if (fail) return false;
        bytes = input;
        return true;
    }
};

int main()
{
    using namespace fitness;
    MemoryStorage storage;
    Model model(storage);
    assert(model.load());
    assert(model.state().settings.rest_seconds == 90);
    assert(model.state().settings.plans[0].moves[2] == 13);
    assert(model.state().settings.plans[4].moves[3] == 13);
    assert(Model::today(1, true) == 0);
    assert(Model::today(5, true) == 4);
    assert(Model::today(0, true) == -1 && Model::today(6, true) == -1);
    assert(Model::today(1, false) == -1);
    assert(!model.start(5, 0, 0));
    assert(model.start(0, 0, 100));
    assert(!model.start(1, 0, 100));
    assert(!model.next(100) && !model.undo(100));
    assert(model.countSet(100));
    assert(!model.countSet(101));
    assert(!model.next(101));
    assert(model.remaining(101) == 89999);
    const auto saved_writes = storage.writes;
    for (uint64_t now = 200; now < 50100; now += 100) assert(!model.tick(now));
    assert(storage.writes == saved_writes);
    assert(model.pauseRest(50100));
    assert(model.remaining(999000) == 40000);
    assert(!model.countSet(999000));
    assert(model.resumeRest(999000));
    assert(!model.tick(1038999));
    assert(model.tick(1039000));
    assert(!model.tick(1039001));
    assert(model.countSet(1039001));
    assert(model.undo(1039002));
    assert(model.state().session.record.done[0] == 1);
    assert(model.state().session.rest == Rest::None);
    assert(!model.undo(1039003));
    assert(model.setGoal(0, {1, 8}, 1039003));
    assert(model.setRest(30, 1039003));
    assert(model.state().session.record.goals[0].sets == 5);
    assert(model.state().session.rest_seconds == 90);
    assert(!model.setGoal(0, {0, 1}, 0));
    assert(!model.setGoal(14, {1, 1}, 0));
    assert(!model.setGoal(0, {11, 1}, 0));
    assert(!model.setGoal(0, {1, 51}, 0));
    assert(!model.setRest(15, 0));
    for (int count = 1; count < 5; ++count) {
        assert(model.countSet(1040000));
        assert(model.skipRest(1040000));
    }
    assert(!model.countSet(1040000));
    assert(model.next(1040001));
    assert(model.state().session.index == 1);
    assert(model.undo(1040002));
    assert(model.state().session.index == 0 && model.state().session.record.done[0] == 4);
    assert(model.countSet(1050000));
    assert(model.checkpoint(1060000));
    Model reboot(storage);
    assert(reboot.load());
    assert(reboot.state().session.record.done[0] == 5);
    assert(reboot.state().session.rest == Rest::Paused);
    assert(reboot.remaining(0) == 80000);
    assert(!reboot.countSet(10000000));
    assert(reboot.resumeRest(100));
    assert(reboot.remaining(200) == 79900);
    assert(reboot.skipRest(200));
    assert(reboot.next(200));
    assert(reboot.countSet(200));
    assert(reboot.finish(300));
    assert(!reboot.finish(301));
    assert(reboot.history(0) && !reboot.history(0)->complete);
    assert(reboot.history(0)->started == 0);
    assert(reboot.history(0)->done[0] == 5 && reboot.history(0)->done[1] == 1);
    assert(reboot.history(0)->goals[0].reps == 12);
    assert(!reboot.history(1));
    assert(reboot.setPlan(0, {{{13, 0, no_exercise, no_exercise}}}, 400));
    assert(!reboot.setPlan(0, {{{13, 13, no_exercise, no_exercise}}}, 400));
    assert(!reboot.setPlan(0, {{{no_exercise, 0, no_exercise, no_exercise}}}, 400));
    assert(!reboot.setPlan(0, {{{13, no_exercise, 0, no_exercise}}}, 400));
    assert(!reboot.setPlan(0, {{{14, no_exercise, no_exercise, no_exercise}}}, 400));
    assert(reboot.setGoal(13, {1, 12}, 400));
    assert(reboot.start(0, 1791000000, 400));
    assert(reboot.countSet(400));
    assert(reboot.skipRest(400));
    assert(reboot.next(400));
    assert(reboot.countSet(400));
    assert(!reboot.skipRest(400));
    assert(!reboot.next(400));
    assert(!reboot.state().session.active && reboot.history(0)->complete);
    assert(reboot.history(0)->moves[0] == 13 && reboot.history(0)->goals[1].reps == 8);
    for (int index = 0; index < 35; ++index) {
        assert(reboot.start(0, 1791000000 + index, 500));
        assert(reboot.finish(501));
    }
    assert(reboot.state().history_size == 30);
    assert(reboot.history(0)->started == 1791000034);
    assert(reboot.history(29)->started == 1791000005);
    assert(!reboot.history(30));
    assert(reboot.start(4, 0, 600));
    storage.fail = true;
    assert(reboot.countSet(600));
    assert(reboot.saveFailed() && reboot.state().session.record.done[0] == 1);
    assert(!reboot.countSet(601));
    assert(!reboot.retrySave(602));
    storage.fail = false;
    assert(reboot.retrySave(603) && !reboot.saveFailed());
    Model restored(storage);
    assert(restored.load() && restored.state().session.record.done[0] == 1);
    State roundtrip;
    assert(Model::decode(storage.bytes, roundtrip));
    for (size_t size = 0; size < storage.bytes.size(); ++size) {
        auto truncated = storage.bytes;
        truncated.resize(size);
        assert(!Model::decode(truncated, roundtrip));
    }
    for (size_t index = 0; index < storage.bytes.size(); ++index) {
        auto corrupt = storage.bytes;
        corrupt[index] ^= 0x40;
        assert(!Model::decode(corrupt, roundtrip));
    }
    storage.bytes[3] = 2;
    Model corrupt(storage);
    assert(!corrupt.load() && corrupt.saveFailed());
    storage.read_fail = true;
    Model unavailable(storage);
    assert(!unavailable.load() && unavailable.saveFailed());
    printf("PASS: fitness state, debounce, undo, rest, snapshots, reboot, history ring, corrupt/failing storage; blob=%zu bytes\n", Model::encode(model.state()).size());
}
