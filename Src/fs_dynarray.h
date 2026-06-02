#ifndef FS_DYNARRAY_H
#define FS_DYNARRAY_H

#include <stdlib.h>
#include <string.h>

#define DYNARRAY_DECLARE(type, name) \
    typedef struct { \
        type* data; \
        size_t count; \
        size_t capacity; \
    } name;

#define DYNARRAY_INIT(da) \
    do { \
        (da).data = NULL; \
        (da).count = 0; \
        (da).capacity = 0; \
    } while(0)

#define DYNARRAY_FREE(da) \
    do { \
        if ((da).data) { \
            free((da).data); \
            (da).data = NULL; \
        } \
        (da).count = 0; \
        (da).capacity = 0; \
    } while(0)

#define DYNARRAY_RESERVE(da, new_cap) \
    do { \
        if ((new_cap) > (da).capacity) { \
            size_t cap = (da).capacity == 0 ? 4 : (da).capacity; \
            while (cap < (new_cap)) cap *= 2; \
            void* new_data = realloc((da).data, cap * sizeof(*(da).data)); \
            if (new_data) { \
                (da).data = new_data; \
                (da).capacity = cap; \
            } \
        } \
    } while(0)

#define DYNARRAY_ADD(da, item) \
    do { \
        if ((da).count >= (da).capacity) { \
            size_t new_cap = (da).capacity == 0 ? 4 : (da).capacity * 2; \
            DYNARRAY_RESERVE(da, new_cap); \
        } \
        (da).data[(da).count++] = (item); \
    } while(0)

#define DYNARRAY_CLEAR(da) \
    do { \
        (da).count = 0; \
    } while(0)

#endif // FS_DYNARRAY_H
