// SPDX-License-Identifier: MIT

// The console policies of the sound library's model: the steps where nw::snd in NintendoWare for Cafe (the Wii U),
// nn::atk (the Switch), nw::snd for the 3DS and NW4R SND (the Wii) differ. nn::atk is nw::snd on AArch64. The two run
// the same code, built for the Wii U's PowerPC and for the Switch's AArch64. The compilers chose other instructions,
// and the processors round and convert in other ways. The 3DS has an older version of the library, on the ARM11. NW4R
// SND is older still. Each policy holds one console's version of those steps. A comment gives the address of the
// routine a step follows: in Animal Crossing: amiibo Festival's executable for the Wii U, in WarioWare: Get It
// Together!'s main for the Switch, in Pokemon X's code.bin for the 3DS, and for the Wii in the main.dol of the game
// that the comment names.

#pragma once

#include "ntdlib/nwsnd/cafe_policy.h"
#include "ntdlib/nwsnd/ctr_policy.h"
#include "ntdlib/nwsnd/length_rules.h"
#include "ntdlib/nwsnd/nx_policy.h"
#include "ntdlib/nwsnd/revolution_policy.h"
