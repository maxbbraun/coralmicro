#ifndef _LIBS_BASE_ANALOG_H_
#define _LIBS_BASE_ANALOG_H_

#include "third_party/nxp/rt1176-sdk/devices/MIMXRT1176/drivers/fsl_lpadc.h"
#include <cstdint>

namespace valiant {
namespace analog {

enum class Device {
    ADC1,
    ADC2,
};

enum class Side {
    A,
    B,
};

struct ADCConfig {
    ADC_Type *device;
    lpadc_conv_command_config_t conv_config;
    lpadc_conv_trigger_config_t trigger_config;
};

void Init(Device device);
void CreateConfig(ADCConfig& config, Device device, int channel, Side primary_side, bool differential);
uint16_t ReadADC(const ADCConfig& config);

}  // namespace analog
}  // namespace valiant

#endif  // _LIBS_BASE_ANALOG_H_