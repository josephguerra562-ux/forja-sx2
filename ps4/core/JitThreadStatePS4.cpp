// SPDX-License-Identifier: GPL-3.0+
// (AI-assisted) Emitter TLS using pthread keys supported by the PS4 runtime.
#include "common/emitter/x86types.h"

#include <cstdio>
#include <pthread.h>
#include <unistd.h>

namespace
{
	pthread_once_t s_key_once = PTHREAD_ONCE_INIT;
	pthread_key_t s_state_key;

	void DestroyState(void* state)
	{
		delete static_cast<OrbisJitThreadState*>(state);
	}

	void CreateKey()
	{
		pxAssertRel(pthread_key_create(&s_state_key, DestroyState) == 0, "Cannot create PS4 JIT TLS key");
	}
} // namespace

OrbisJitThreadState& GetOrbisJitThreadState()
{
	// 152 in the app: the thread's state, once found, from a thread_local of this file. Every x86Ptr, j8Ptr and j32Ptr of the emitter
	// came here (each instruction a recompiler writes) and paid a pthread_once and a pthread_getspecific; God of War II's VU1 thread
	// paid them at every microprogram start (~76,000 a second: GetOrbisJitThreadState, __wrap_pthread_once and xSetPtr in its samples).
	// A plain pointer: no TLS initialiser, nothing shared between files (the CI's "hilos"). The key below still owns and frees it.
	static thread_local OrbisJitThreadState* t_state = nullptr;
	if (t_state) [[likely]]
		return *t_state;
	pxAssertRel(pthread_once(&s_key_once, CreateKey) == 0, "Cannot initialize PS4 JIT TLS key");
	auto* state = static_cast<OrbisJitThreadState*>(pthread_getspecific(s_state_key));
	if (!state)
	{
		state = new OrbisJitThreadState;
		pxAssertRel(pthread_setspecific(s_state_key, state) == 0, "Cannot assign PS4 JIT TLS state");
		// Log once per compiling thread without sharing libc FILE buffers.
		// Console validation of concurrent EE/VU1 compilation needs proper testing.
		char message[160];
		const int length = std::snprintf(message, sizeof(message), "[PS4 JIT] thread=%p emitter-state=%p\n",
			reinterpret_cast<void*>(pthread_self()), static_cast<void*>(state));
		if (length > 0 && length < static_cast<int>(sizeof(message)))
			write(STDOUT_FILENO, message, static_cast<size_t>(length));
	}
	t_state = state;
	return *state;
}
