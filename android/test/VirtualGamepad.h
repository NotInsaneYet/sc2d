#pragma once

#include <cstdint>
#include <cstddef>
#include <linux/input.h>

class VirtualGamepad {
public:
    VirtualGamepad();
    ~VirtualGamepad() { Destroy(); }

    VirtualGamepad(const VirtualGamepad&) = delete;
    VirtualGamepad& operator=(const VirtualGamepad&) = delete;

    bool Create();
    void Destroy();
    bool IsValid() const { return m_fd >= 0; }

    bool PollFF(uint16_t& outStrong, uint16_t& outWeak);

    void Update(const uint8_t* buf, size_t n);

private:
    void Emit(uint16_t type, uint16_t code, int32_t value);
    void EmitSyn();

    int m_fd = -1;

    uint8_t m_prevButtons[3]{};
    uint8_t m_prevFlags{};
    int32_t m_prevTriggerL{}, m_prevTriggerR{};
    int32_t m_prevLX{}, m_prevLY{}, m_prevRX{}, m_prevRY{};
    int32_t m_prevHatX{}, m_prevHatY{};


    static constexpr int SC2_FF_MAX = 4;
    struct ff_effect m_effects[SC2_FF_MAX]{};
    bool m_ffActive[SC2_FF_MAX]{};
};
