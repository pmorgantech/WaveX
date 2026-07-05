#ifndef DAISY_SEED_H
#define DAISY_SEED_H

// Mock daisy_seed.h for unit testing. Pulls in the shared mock types
// (daisy::DaisySeed with a printf PrintLine, daisy::System, daisy::I2CHandle)
// so hardware-including sources like daisy_inter_mcu_message_handlers.cpp
// compile on the host.

#include "daisy_mocks.h"

#endif  // DAISY_SEED_H
