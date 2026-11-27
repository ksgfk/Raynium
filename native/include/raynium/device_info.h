#pragma once

#include <radray/runtime/gpu_device.h>
#include <raynium/bridge.h>

namespace raynium {
void QueryDeviceInfo(radray::DeviceContext& runtime, rn_device_info& info);
}
