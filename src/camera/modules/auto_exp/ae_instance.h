#ifndef AE_INSTANCE_H
#define AE_INSTANCE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t current_luma;
    uint32_t current_exp;
    float    current_gain;
    uint32_t current_fps;
} ae_frame_input_t;

typedef struct {
    uint32_t target_exposure;
    float    target_gain;
} ae_frame_output_t;

typedef struct ae_instance {
    bool     enable;
    uint8_t  target_luma;
    float    speed;

    float    exposure_smooth;
    uint32_t frame_id;
} ae_instance_t;

void ae_instance_init(ae_instance_t *self);
void ae_instance_set_enable(ae_instance_t *self, bool en);
void ae_instance_set_target_luma(ae_instance_t *self, uint8_t luma);
void ae_instance_set_speed(ae_instance_t *self, float speed);

void ae_instance_run(const ae_instance_t *self,
                     const ae_frame_input_t *in,
                     ae_frame_output_t *out);

#ifdef __cplusplus
}
#endif

#endif /* AE_INSTANCE_H */
