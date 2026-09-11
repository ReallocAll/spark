#ifndef ENDSTONE_SPARK_LINUX_ALLOCATION_GATEWAY_ABI_H
#define ENDSTONE_SPARK_LINUX_ALLOCATION_GATEWAY_ABI_H

#include <stddef.h>
#include <stdint.h>

#define SPARK_GATEWAY_ABI_VERSION 2U
#define SPARK_GATEWAY_FAMILY      "endstone.spark.allocation.gateway"
#define SPARK_GATEWAY_SYMBOL      "spark_allocation_gateway_v1"
#define SPARK_GATEWAY_FILENAME    "libspark_allocation_gateway_v1.so"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SparkGatewayCallbacksV1 {
    void *(*malloc_callback)(void *, size_t);
    void *(*calloc_callback)(void *, size_t, size_t);
    void *(*realloc_callback)(void *, void *, size_t);
    void (*free_callback)(void *, void *);
    void *(*reallocarray_callback)(void *, void *, size_t, size_t);
    void *(*aligned_alloc_callback)(void *, size_t, size_t);
    int (*posix_memalign_callback)(void *, void **, size_t, size_t);
    void (*tls_callback)(void *, void *);
} SparkGatewayCallbacksV1;

typedef struct SparkGatewayBindingV1 {
    uint32_t size;
    uint32_t group;
    void *entries[7];
    void (*tls_entry)(void *);
} SparkGatewayBindingV1;

typedef struct SparkGatewayV1 {
    uint32_t size;
    uint32_t version;
    const char *family;
    const char *compatibility;
    uint32_t capacity;
    uint32_t provider_capacity;
    int (*bootstrap)(const char *, const void *, char *, size_t);
    const char *(*installation)(void);
    int (*reserve)(void *const *, const void *, SparkGatewayBindingV1 *, char *, size_t);
    int (*open)(uint32_t, const SparkGatewayCallbacksV1 *, void *, int);
    void (*close)(uint32_t, int);
    uint64_t (*active)(uint32_t, int);
    int (*clear)(uint32_t, int);
    int (*retire)(uint32_t);
    void (*publish)(uint32_t);
    int (*cancel)(uint32_t);
    uint32_t (*used)(void);
    uint32_t (*leases)(void);
    void (*test_gate)(uint32_t, uint32_t, uint32_t, uint64_t *);
} SparkGatewayV1;

typedef const SparkGatewayV1 *(*SparkGatewayQueryV1)(void);

const SparkGatewayV1 *spark_allocation_gateway_v1(void);

#ifdef __cplusplus
}
#endif

#endif
