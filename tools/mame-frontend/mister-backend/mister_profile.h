/*
 * mister_profile.h — SIGPROF PC sampler (see mister_profile.cpp).
 *
 * mister_profile_init() is a no-op unless MISTER_PROFILE is set in the
 * environment; the dump runs from atexit, so callers only need the init.
 */
#ifndef MISTER_PROFILE_H
#define MISTER_PROFILE_H

#ifdef __cplusplus
extern "C" {
#endif

void mister_profile_init(void);
void mister_profile_dump(void);

#ifdef __cplusplus
}
#endif

#endif /* MISTER_PROFILE_H */
