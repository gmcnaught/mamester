// license:BSD-3-Clause
// copyright-holders:MAMESTer port
// Derived from drcbex86.cpp (Aaron Giles) and drcbearm64.cpp (windyfairy, Vas Crabb)
#ifndef MAME_CPU_DRCBEARM32_H
#define MAME_CPU_DRCBEARM32_H

#pragma once

#include "drcuml.h"

#include <memory>


namespace drc {

std::unique_ptr<drcbe_interface> make_drcbe_arm32(
		drcuml_state &drcuml,
		device_t &device,
		drc_cache &cache,
		uint32_t flags,
		int modes,
		int addrbits,
		int ignorebits);

} // namespace drc

#endif // MAME_CPU_DRCBEARM32_H
