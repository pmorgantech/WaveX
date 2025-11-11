/**
 * @file application_context.h
 * @brief Application Context - Owns all system components and manages dependencies
 *
 * This class eliminates global state by owning all major system components
 * and providing dependency injection accessors. It serves as the central
 * dependency container for the entire application.
 */

#pragma once

#include "comm/i_comm_interface.h"
#include "comm/packet_router.h"
#include "comm/statistics.h"

#include <memory>
#include <string>

// Forward declarations to avoid namespace issues
namespace WaveX {
namespace Comm {
class PacketRouter;
class ICommInterface;
}  // namespace Comm
}  // namespace WaveX

namespace WaveX {

class ApplicationContext {
   public:
    /**
     * @brief Construct application context and initialize all components
     */
    ApplicationContext();

    /**
     * @brief Destroy application context and cleanup all components
     */
    ~ApplicationContext() = default;

    // Delete copy/move operations - context should be unique
    ApplicationContext(const ApplicationContext&) = delete;
    ApplicationContext& operator=(const ApplicationContext&) = delete;
    ApplicationContext(ApplicationContext&&) = delete;
    ApplicationContext& operator=(ApplicationContext&&) = delete;

    // Component accessors for dependency injection
    StatisticsManager& getStatistics() { return *statistics_; }
    WaveX::Comm::PacketRouter& getPacketRouter() { return *packet_router_; }
    WaveX::Comm::ICommInterface& getCommInterface() { return *comm_interface_; }

    // Component accessors (const versions)
    const StatisticsManager& getStatistics() const { return *statistics_; }
    const WaveX::Comm::PacketRouter& getPacketRouter() const { return *packet_router_; }
    const WaveX::Comm::ICommInterface& getCommInterface() const { return *comm_interface_; }

   private:
    // Private initialization method
    void initializeLinks();

    // Owned components - no global state
    std::unique_ptr<StatisticsManager> statistics_;
    std::unique_ptr<WaveX::Comm::PacketRouter> packet_router_;
    std::unique_ptr<WaveX::Comm::ICommInterface> comm_interface_;
};

}  // namespace WaveX
