#ifndef __AUDIO_SUBSYS_NS_H__
#define __AUDIO_SUBSYS_NS_H__

#include "audio_subsys.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NSLevel_Low,
    NSLevel_Moderate,
    NSLevel_High,
    NSLevel_VeryHigh
} NSLevel;

typedef struct {
    int sample_rate;
    NSLevel level;
} ns_config_t;

#define NS_CONFIG_DEFAULT() {   \
    .sample_rate = 16000,       \
    .level = NSLevel_Moderate,  \
}

extern audio_subsys_module_t audio_subsys_ns;

#ifdef __cplusplus
}
#endif

#endif /* __AUDIO_SUBSYS_NS_H__ */
