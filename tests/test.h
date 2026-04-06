#ifndef LLMSH_TEST_H
#define LLMSH_TEST_H

#include <stdio.h>
#include <string.h>

static int test_pass = 0;
static int test_fail = 0;

#define ASSERT(expr) do { \
    if (expr) { test_pass++; } \
    else { test_fail++; \
        fprintf(stderr, "  FAIL: %s:%d: %s\n", __FILE__, __LINE__, #expr); } \
} while (0)

#define ASSERT_STR_EQ(a, b) do { \
    const char *_a = (a), *_b = (b); \
    if (_a && _b && strcmp(_a, _b) == 0) { test_pass++; } \
    else { test_fail++; \
        fprintf(stderr, "  FAIL: %s:%d: \"%s\" != \"%s\"\n", \
                __FILE__, __LINE__, _a ? _a : "(null)", _b ? _b : "(null)"); } \
} while (0)

#define ASSERT_NULL(ptr) do { \
    if ((ptr) == NULL) { test_pass++; } \
    else { test_fail++; \
        fprintf(stderr, "  FAIL: %s:%d: expected NULL\n", __FILE__, __LINE__); } \
} while (0)

#define ASSERT_NOT_NULL(ptr) do { \
    if ((ptr) != NULL) { test_pass++; } \
    else { test_fail++; \
        fprintf(stderr, "  FAIL: %s:%d: expected non-NULL\n", __FILE__, __LINE__); } \
} while (0)

#define RUN_TEST(fn) do { \
    fprintf(stderr, "  %s...", #fn); \
    fn(); \
    fprintf(stderr, " ok\n"); \
} while (0)

#define TEST_SUMMARY() do { \
    fprintf(stderr, "\n%d passed, %d failed\n", test_pass, test_fail); \
    return test_fail > 0 ? 1 : 0; \
} while (0)

#endif /* LLMSH_TEST_H */
