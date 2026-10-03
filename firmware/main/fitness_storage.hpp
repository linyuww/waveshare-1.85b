#pragma once
#include "fitness_model.hpp"

class FitnessStorage final : public fitness::Storage {
public:
    Load read(std::vector<uint8_t> &bytes) override;
    bool write(const std::vector<uint8_t> &bytes) override;
};
