#pragma once

#include <cstdint>
#include <cstddef>
#include "../RPCInterface/meathook_interface.h"

namespace mh_ap {

constexpr std::uint32_t kProtocolVersion = MH_AP_PROTOCOL_VERSION;
constexpr std::size_t kRequestQueueCapacity = 64;
constexpr std::size_t kCompletedCacheCapacity = 128;
constexpr std::size_t kDeathLinkCapacity = 128;
constexpr std::size_t kEventCapacity = 128;

enum class RuntimeState : std::uint32_t {
	NotReady = 0,
	Unsupported = 1,
	Ready = 2,
};

enum Capability : std::uint64_t {
	CapabilityNone = 0,
	CapabilityRuntimeInfo = 1ull << 0,
	CapabilityRuntimeSnapshot = 1ull << 1,
	CapabilityNativeDeathLink = 1ull << 2,
	CapabilityNativeEvents = 1ull << 3,
};

enum class Operation : std::uint32_t {
	GetRuntimeStatus = 1,
};

enum class Status : std::uint32_t {
	Ok = 0,
	Accepted = 1,
	Duplicate = 2,
	QueueFull = 3,
	InvalidRequest = 4,
	InvalidProtocol = 5,
	NotReady = 6,
	Unsupported = 7,
};

struct Request final {
	std::uint64_t id;
	std::uint32_t protocol_version;
	Operation operation;
	std::uint64_t requested_capabilities;
};

struct Response final {
	std::uint64_t id;
	std::uint32_t protocol_version;
	Status status;
	RuntimeState runtime_state;
	std::uint64_t capabilities;
};

void initialize();
void pump();

void get_runtime_info(APRuntimeInfo* info);
void get_runtime_snapshot(APRuntimeSnapshot* snapshot);
APDeathLinkStatus apply_deathlink(const char* request_id);
void get_events_since(std::uint64_t after_sequence, std::uint32_t max_count, APEventBatch* batch);
void get_inventory_item_count(const char* decl_name, APInventoryIntResult* result);
void has_player_perk(const char* decl_name, APInventoryBoolResult* result);
void is_player_perk_active(const char* decl_name, APInventoryBoolResult* result);
void get_equipped_weapon(APEquippedWeaponResult* result);

Status query_status(Response* response);
Status submit(const Request& request, Response* cached_response = nullptr);

} // namespace mh_ap
