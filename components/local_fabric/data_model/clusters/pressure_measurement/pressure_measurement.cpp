#include "pressure_measurement.h"
#include "fabric_manager_private.h"
#include <app/data-model/Decode.h>
#include <app/data-model/Nullable.h>
#include <cstring>

namespace data_model::clusters::pressure_measurement {
namespace {
struct measurement {
    bool known;
    int16_t hectopascals; // Matter MeasuredValue is in tenths of a kPa (= hPa).
};
CHIP_ERROR decode(uint32_t id, chip::TLV::TLVReader &reader, cluster_state *state)
{
    if (id != 0)
        return CHIP_ERROR_NOT_FOUND;
    chip::app::DataModel::Nullable<int16_t> value;
    ReturnErrorOnFailure(chip::app::DataModel::Decode(reader, value));
    measurement m = {};
    if (!value.IsNull()) {
        m.known = true;
        m.hectopascals = value.Value();
    }
    memcpy(state->bytes, &m, sizeof(m));
    return CHIP_NO_ERROR;
}
esp_err_t encode(const cluster_state *state, cJSON *properties, cJSON *)
{
    measurement m;
    memcpy(&m, state->bytes, sizeof(m));
    return json::number_property(properties, "pressure_hpa",
                                 m.known ? cJSON_CreateNumber(m.hectopascals) : cJSON_CreateNull(), "hectopascals",
                                 -32767, 32767);
}
} // namespace
const cluster_adapter adapter = {"pressure_measurement", 0x0403, decode, encode, nullptr};
} // namespace data_model::clusters::pressure_measurement
