// Vulkan call tracer for Switch 2 homebrew. A crash leaves no report on this
// runtime (no exception handler, no core dump), so to find a crash in or
// between Vulkan calls: every function the app looks up through
// hen_vktrace_get_instance_proc_addr() is replaced with a trampoline
// (hen_vktrace_thunks.S) that prints the function's name before calling it.
// The last line printed is the call the app died in, or the last one it made.
//
// Slow (a line per call over the log connection): for the first frames only.
// Repeats of the same call on the same thread are counted, not printed.
#include <switch.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#define MAX_FUNCTIONS 2048

// The driver's entry point (OunceVK, linked statically).
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char* name);

extern const char hen_vktrace_thunks[];  // MAX_FUNCTIONS trampolines of 8 bytes

static Mutex s_lock;
static PFN_vkVoidFunction s_real[MAX_FUNCTIONS];
static char s_names[MAX_FUNCTIONS][56];
static int s_count;
static PFN_vkGetDeviceProcAddr s_getDeviceProcAddr;
static int s_lastIndex = -1;
static void* s_lastThread;
static unsigned s_repeats;

static PFN_vkVoidFunction wrap(const char* name, PFN_vkVoidFunction real);

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL traced_GetDeviceProcAddr(VkDevice device, const char* name) {
	return wrap(name, s_getDeviceProcAddr(device, name));
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL hen_vktrace_get_instance_proc_addr(VkInstance instance, const char* name) {
	return wrap(name, vkGetInstanceProcAddr(instance, name));
}

static PFN_vkVoidFunction wrap(const char* name, PFN_vkVoidFunction real) {
	if (real == NULL || name == NULL) {
		return real;
	}
	// Lookups stay traced: the functions found through these are wrapped too.
	if (strcmp(name, "vkGetInstanceProcAddr") == 0) {
		return (PFN_vkVoidFunction) hen_vktrace_get_instance_proc_addr;
	}
	if (strcmp(name, "vkGetDeviceProcAddr") == 0) {
		s_getDeviceProcAddr = (PFN_vkGetDeviceProcAddr) real;
		return (PFN_vkVoidFunction) traced_GetDeviceProcAddr;
	}
	mutexLock(&s_lock);
	int slot = -1;
	for (int i = 0; i < s_count; i++) {
		if (s_real[i] == real && strcmp(s_names[i], name) == 0) {
			slot = i;
			break;
		}
	}
	if (slot < 0 && s_count < MAX_FUNCTIONS) {
		slot = s_count++;
		s_real[slot] = real;
		strncpy(s_names[slot], name, sizeof(s_names[slot]) - 1);
	}
	mutexUnlock(&s_lock);
	return slot < 0 ? real : (PFN_vkVoidFunction) (uintptr_t) (hen_vktrace_thunks + slot * 8);
}

// Called by the trampoline: logs call number index, returns the real function.
PFN_vkVoidFunction hen_vktrace_enter(unsigned index) {
	void* thread = armGetTls();
	mutexLock(&s_lock);
	if ((int) index == s_lastIndex && thread == s_lastThread) {
		s_repeats++;
	} else {
		char line[112];
		int n = 0;
		if (s_repeats > 0) {
			n = snprintf(line, sizeof(line), "vk:   (x%u more)\n", s_repeats);
			s_repeats = 0;
		}
		n += snprintf(line + n, sizeof(line) - n, "vk: %s [core %d]\n", s_names[index], svcGetCurrentProcessorNumber());
		write(STDERR_FILENO, line, n);
		s_lastIndex = (int) index;
		s_lastThread = thread;
	}
	PFN_vkVoidFunction real = s_real[index];
	mutexUnlock(&s_lock);
	return real;
}
