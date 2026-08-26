#ifndef ENDSTONE_SPARK_FUNCHOOK_COMPAT_H
#define ENDSTONE_SPARK_FUNCHOOK_COMPAT_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct funchook funchook_t;

enum {
    FUNCHOOK_ERROR_SUCCESS = 0,
    FUNCHOOK_ERROR_NOT_INSTALLED = 1,
    FUNCHOOK_ERROR_INTERNAL = 2,
};

funchook_t *funchook_create(void);
int funchook_prepare(funchook_t *funchook, void **target_func, void *hook_func);
int funchook_install(funchook_t *funchook, int flags);
int funchook_refresh(funchook_t *funchook);
int funchook_uninstall(funchook_t *funchook, int flags);
int funchook_destroy(funchook_t *funchook);
const char *funchook_error_message(funchook_t *funchook);

#ifdef __cplusplus
}
#endif

#endif  // ENDSTONE_SPARK_FUNCHOOK_COMPAT_H
