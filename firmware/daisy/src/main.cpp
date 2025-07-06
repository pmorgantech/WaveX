#include "daisy_seed.h"
#include "daisysp.h"

using namespace daisy;
using namespace daisysp;

// Hardware
DaisySeed hw;

// Audio callback
void AudioCallback(AudioHandle::InputBuffer  in,
                   AudioHandle::OutputBuffer out,
                   size_t                    size)
{
    for (size_t i = 0; i < size; i++)
    {
        // TODO: Process audio samples
        // TODO: Communicate with ESP32 frontend
        // TODO: Handle envelopes, LFOs, modulation
        // TODO: Output CVs via SPI DACs
        
        // For now, just pass through
        out[0][i] = in[0][i];
        out[1][i] = in[1][i];
    }
}

int main(void)
{
    // Initialize Daisy Seed hardware
    hw.Configure();
    hw.Init();
    
    // Initialize audio
    hw.SetAudioBlockSize(64); // 64-sample blocks
    hw.SetAudioSampleRate(SaiHandle::Config::SampleRate::SAI_48KHZ);
    
    // TODO: Initialize SPI communication with ESP32
    // TODO: Initialize SPI DACs for CV output
    // TODO: Initialize sample memory management
    // TODO: Initialize modulation matrix
    
    // Start audio processing
    hw.StartAudio(AudioCallback);
    
    // Main loop
    while(1)
    {
        // TODO: Handle SPI communication with ESP32
        // TODO: Update modulation sources
        // TODO: Manage samples and memory
        
        // Sleep for a bit
        System::Delay(1);
    }
} 