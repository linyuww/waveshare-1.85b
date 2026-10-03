#pragma once
#include "battery.h"
#include <cstdio>
#include <cstdint>
// BQ27220 standard-command units; estimated SOC and requested charging values
// are labelled separately from measurements. 0xffff time means unavailable.
inline void formatBatteryInfo(char *text, size_t size, const battery::Sample &sample, uint32_t now_ms)
{
        if (sample.valid) {
            char raw_soc[16], health[16], empty_time[24], full_time[24], desired_voltage[32], desired_current[32];
            snprintf(empty_time, sizeof(empty_time), "%u min", sample.timeToEmptyMin);
            snprintf(full_time, sizeof(full_time), "%u min", sample.timeToFullMin);
            snprintf(desired_voltage, sizeof(desired_voltage), "%u mV", sample.desiredVoltageMv);
            snprintf(desired_current, sizeof(desired_current), "%u mA", sample.desiredCurrentMa);
            snprintf(raw_soc, sizeof(raw_soc), "%d%%", sample.gaugePercent);
            snprintf(health, sizeof(health), "%d%%", sample.healthPercent);
            const unsigned age = (now_ms - sample.sampledAtMs) / 1000;
            snprintf(text, size,
                     "电池电量：%d%%（估算）\n电量计 SOC：%s\n电压：%u mV\n电流：%d mA\n平均电流：%d mA\n平均功率：%d mW\n温度：%.1f °C\n芯片温度：%.1f °C\n剩余容量：%u mAh\n满充容量：%u mAh\n设计容量：%u mAh\n健康度：%s\n循环次数：%u\n外部供电：%s\n预计放空：%s\n预计充满：%s\n建议充电电压：%s\n建议充电电流：%s\n电池状态：0x%04X\n运行状态：0x%04X\n原始库仑计数：%u mAh\n%s · %u 秒前\n\n电流为电量计原始读数。\n建议充电值不是实测值。",
                     sample.percent, sample.gaugePercent >= 0 ? raw_soc : "--", sample.voltageMv, sample.currentMa,
                     sample.averageCurrentMa, sample.averagePowerMw, sample.temperatureDeciC / 10.0,
                     sample.internalTemperatureDeciC / 10.0, sample.remainingCapacityMah, sample.fullChargeCapacityMah,
                     sample.designCapacityMah, sample.healthPercent >= 0 ? health : "--", sample.cycleCount,
                     sample.externalPower ? "检测到" : "未检测到", sample.timeToEmptyMin != 65535 ? empty_time : "--",
                     sample.timeToFullMin != 65535 ? full_time : "--", sample.desiredVoltageMv != 65535 ? desired_voltage : "请求充电器最大值",
                     sample.desiredCurrentMa != 65535 ? desired_current : "请求充电器最大值", sample.statusBits, sample.operationBits,
                     sample.rawCoulombCount, sample.stale ? "读取失败，显示上次数据" : "实时采样", age);
        } else snprintf(text, size, "电池数据暂不可用\n等待 BQ27220 读取");
}
