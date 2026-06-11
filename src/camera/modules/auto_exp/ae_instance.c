#include "ae_instance.h"
#include <string.h>

void ae_instance_init(ae_instance_t *self)
{
    memset(self, 0, sizeof(*self));
    self->enable      = true;
    self->target_luma = 128;
    self->speed       = 0.5f;
}

void ae_instance_set_enable(ae_instance_t *self, bool en)
{
    self->enable = en;
}

void ae_instance_set_target_luma(ae_instance_t *self, uint8_t luma)
{
    self->target_luma = luma;
}

void ae_instance_set_speed(ae_instance_t *self, float speed)
{
    self->speed = (speed < 0.01f) ? 0.01f : ((speed > 1.0f) ? 1.0f : speed);
}

void ae_instance_run(const ae_instance_t *self,
                     const ae_frame_input_t *in,
                     ae_frame_output_t *out)
{
    if (!self->enable || in->current_luma == 0) {
        out->target_exposure = in->current_exp;
        out->target_gain     = in->current_gain;
        return;
    }

    float error = (float)self->target_luma - (float)in->current_luma;
    float adj   = error * self->speed;

    if (in->current_gain < 8.0f) {
        out->target_gain     = in->current_gain + adj * 0.05f;
        out->target_exposure = in->current_exp;
    } else {
        out->target_gain     = in->current_gain;
        out->target_exposure = (uint32_t)((float)in->current_exp + adj * 100.0f);
    }

    float max_exp = 990000.0f / (float)in->current_fps;
    if (out->target_exposure > (uint32_t)max_exp)
        out->target_exposure = (uint32_t)max_exp;
}
