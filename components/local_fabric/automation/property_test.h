#pragma once
#include "cJSON.h"
#include <stdbool.h>

// Operands borrow from the parsed rule for its lifetime. A missing report never matches,
// including for !=. changes is an edge trigger, not a state predicate.
typedef enum { TEST_TRUE, TEST_FALSE, TEST_EQ, TEST_NE, TEST_LT, TEST_LE, TEST_GT, TEST_GE, TEST_CHANGES } test_op_t;
typedef struct {
    const char *type;
    test_op_t op;
    const cJSON *value;
} property_test_t;
bool property_test_parse(property_test_t *out, const cJSON *spec, bool trigger);
bool property_test_supported(const property_test_t *test, const cJSON *property);
