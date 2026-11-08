#pragma once

// Internal CPU instrumentation. Disabled builds need neither Tracy nor clock reads.
#if defined(REALRHI_ENABLE_TRACY)
	#include <tracy/Tracy.hpp>
	#if !defined(TRACY_ENABLE)
		#error "REALRHI_ENABLE_TRACY requires a Tracy client built with TRACY_ENABLE"
	#endif

	#define REALRHI_CPU_ZONE(name) ZoneScopedN(name)
	#define REALRHI_CPU_ZONE_TEXT(text, size) ZoneText(text, size)
#else
	#define REALRHI_CPU_ZONE(name) ((void)0)
	#define REALRHI_CPU_ZONE_TEXT(text, size) ((void)0)
#endif
