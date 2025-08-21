#pragma once

#include <cstddef>
#include <cstdint>

// Decodes protocol packets and routes to subsystems
void ProcessUARTMessage(const uint8_t* buffer, size_t length);


