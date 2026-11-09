#include "test_common.h"
#include "unity.h"

void setUp(void)
{
    mock_storage_reset();
    mock_storage2_reset();
    mock_callbacks_reset();
    param_init(&g_mock_storage, NULL);
}
void tearDown(void)
{
    param_deinit();
}

extern void run_test_boundary(void);

int main(void)
{
    UNITY_BEGIN();
    run_test_boundary();
    return UNITY_END();
}
