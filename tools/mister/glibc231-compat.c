/* Symbols a bookworm-built libstdc++ needs that the MiSTer's glibc 2.31 has not.
 *
 * tools/mister/Dockerfile.cross-armhf-cxx20 splits the compiler's Debian
 * release (bookworm, for C++20) from the target's glibc (bullseye 2.31, what
 * the device runs). The C compiler and its headers follow that split cleanly.
 * libstdc++.a does not: it is PREBUILT against bookworm's glibc 2.36 and
 * carries references to symbols added after 2.31 no matter what is on the
 * command line. There are exactly two, and both have a correct definition that
 * fits in a few lines.
 *
 * Linked into the MAME core through LDOPTS in tools/build-lrmame.sh. The host
 * binary needs no copy of its own while it is C and pulls in no libstdc++
 * member.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* glibc 2.32. libstdc++ reads it in __cxa_guard_acquire/release/abort, in
 * locale initialisation and in the COW string refcount, to skip the atomic when
 * the process has never created a thread.
 *
 * 0 means "threads may exist", which is the conservative answer and always
 * correct: it selects the atomic path, which is what a 2.31-era libstdc++ took
 * unconditionally. MAME runs its own threads, so the fast path would seldom be
 * taken here in any case.
 */
char __libc_single_threaded = 0;

/* glibc 2.36. Reached by libstdc++'s std::random_device on builds configured
 * for it; /dev/urandom is the fallback it would use otherwise, so this is that
 * fallback written out. The callers are seeding, so no caching.
 */
uint32_t arc4random(void)
{
	uint32_t v = 0;
	FILE *f = fopen("/dev/urandom", "rb");
	if (f) {
		if (fread(&v, sizeof v, 1, f) != 1)
			v = 0;
		fclose(f);
	}
	return v;
}

void arc4random_buf(void *buf, size_t n)
{
	FILE *f = fopen("/dev/urandom", "rb");
	if (f) {
		size_t got = fread(buf, 1, n, f);
		(void)got;
		fclose(f);
	}
}
