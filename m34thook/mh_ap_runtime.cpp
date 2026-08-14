#include "mh_ap_runtime.hpp"

#include <atomic>
#include <mutex>
#include <cstring>
#include <chrono>
#include <string>
#include <algorithm>
#include <cstdio>
#include <vector>
#include <windows.h>
#include <bcrypt.h>

#include "idtypeinfo.hpp"
#include "gameapi.hpp"
#include "idStr.hpp"
#include "eventdef.hpp"

extern std::string gCurrentCheckpointName;

namespace mh_ap {
namespace {

struct Queue final {
	Request entries[kRequestQueueCapacity]{};
	std::size_t head = 0;
	std::size_t size = 0;
};

struct Completed final {
	Response entries[kCompletedCacheCapacity]{};
	std::size_t next = 0;
	std::size_t size = 0;
};

std::mutex g_mutex;
Queue g_queue;
Completed g_completed;

std::mutex g_snapshot_mutex;
APRuntimeInfo g_runtime_info{};
APRuntimeSnapshot g_runtime_snapshot{};
std::uint64_t g_sequence = 0;
std::uint64_t g_timestamp = 0;

std::atomic<RuntimeState> g_runtime_state{RuntimeState::NotReady};
std::atomic<std::uint64_t> g_capabilities{CapabilityNone};
bool g_snapshot_access_supported = false;
bool g_map_access_supported = false;
std::atomic<bool> g_deathlink_supported{false};
std::atomic<bool> g_event_access_supported{false};
bool g_inventory_contract_valid = false;
char g_inventory_diagnostic[128]{};
classVariableInfo_t* g_health_field = nullptr;
classVariableInfo_t* g_max_health_field = nullptr;
classVariableInfo_t* g_dead_field = nullptr;
classVariableInfo_t* g_map_name_field = nullptr;
idEventDef* g_request_suicide_event = nullptr;

void copy_bounded(char* destination, std::size_t capacity, const char* source);

struct PendingDeathLink final {
	char request_id[64]{};
};

struct CompletedDeathLink final {
	char request_id[64]{};
	APDeathLinkStatus status = AP_DEATHLINK_INVALID;
};

struct EventRing final {
	APEvent entries[kEventCapacity]{};
	std::size_t next = 0;
	std::size_t size = 0;
	std::uint64_t next_sequence = 1;
};

PendingDeathLink g_pending_deathlinks[kDeathLinkCapacity]{};
std::size_t g_pending_deathlink_count = 0;
CompletedDeathLink g_completed_deathlinks[kDeathLinkCapacity]{};
std::size_t g_completed_deathlink_next = 0;
std::size_t g_completed_deathlink_count = 0;
EventRing g_events;
std::mutex g_event_mutex;
std::mutex g_deathlink_mutex;
char g_associated_deathlink_id[64]{};
bool g_has_associated_deathlink = false;
std::uint64_t g_pump_count = 0;
std::uint64_t g_awaiting_deadline = 0;
constexpr std::uint64_t kDeathLinkTransitionTimeoutPumps = 120;

void copy_request_id(char destination[64], const char* source) {
	copy_bounded(destination, 64, source);
}

bool valid_request_id(const char* request_id) {
	return request_id != nullptr && request_id[0] != '\0'
		&& std::strlen(request_id) < 64;
}

bool valid_direct_field(classVariableInfo_t* field) {
	if (field == nullptr || field->offset < 0)
		return false;
	return field->get != nullptr || field->size == 1 || field->size == 2 || field->size == 4 || field->size == 8;
}

bool validate_player_fields() {
	g_health_field = idType::FindClassField("idPlayer", "health");
	g_max_health_field = idType::FindClassField("idPlayer", "maxHealth");
	g_dead_field = idType::FindClassField("idPlayer", "dead");
	if (!valid_direct_field(g_dead_field))
		g_dead_field = idType::FindClassField("idPlayer", "isDead");
	return valid_direct_field(g_health_field)
		&& valid_direct_field(g_max_health_field)
		&& valid_direct_field(g_dead_field);
}

bool validate_map_field() {
	g_map_name_field = idType::FindClassField("idMapFileLocal", "name");
	return g_map_name_field != nullptr && g_map_name_field->type != nullptr
		&& std::strcmp(g_map_name_field->type, "idStr") == 0
		&& g_map_name_field->offset >= 0
		&& g_map_name_field->size == static_cast<int>(sizeof(idStr));
}

bool validate_supported_contract() {
	return validate_player_fields() && validate_map_field();
}

bool validate_deathlink_event() {
	g_request_suicide_event = nullptr;
	if (descan::g_eventreceiver_processeventargs == nullptr)
		return false;
	idEventDefInterfaceLocal* events = idEventDefInterfaceLocal::Singleton();
	if (events == nullptr)
		return false;
	g_request_suicide_event = events->FindEvent("requestSuicide");
	if (g_request_suicide_event == nullptr)
		return false;
	const char* event_name = events->GetEventNameForNum(0x25A);
	return event_name != nullptr
		&& std::strcmp(event_name, "requestSuicide") == 0
		&& events->GetNumEventArgs(0x25A) == 0
		&& g_request_suicide_event->name != nullptr
		&& std::strcmp(g_request_suicide_event->name, "requestSuicide") == 0
		&& g_request_suicide_event->eventnum == 0x25A
		&& g_request_suicide_event->numargs == 0;
}

struct InventoryEventContract final {
	const char* name;
	unsigned event_number;
	unsigned argument_count;
	const char* argument_type;
	char format;
	int return_type;
};

bool validate_inventory_event(const InventoryEventContract& expected, idEventDefInterfaceLocal* events) {
	idEventDef* event = events->FindEvent(expected.name);
	if (event == nullptr || event->eventnum != static_cast<int>(expected.event_number)
		|| event->name == nullptr || std::strcmp(event->name, expected.name) != 0
		|| events->GetEventNameForNum(expected.event_number) == nullptr
		|| std::strcmp(events->GetEventNameForNum(expected.event_number), expected.name) != 0
		|| events->GetNumEventArgs(expected.event_number) != expected.argument_count
		|| event->numargs != static_cast<int>(expected.argument_count)
		|| event->formatspec == nullptr || event->formatspec[0] != expected.format
		|| event->formatspec[1] != '\0' || event->returnType != expected.return_type)
		return false;
	if (expected.argument_count != 0) {
		std::string argument_type;
		if (!event->GetArgTypeName(0, &argument_type) || argument_type != expected.argument_type)
			return false;
	}
	return true;
}

bool validate_inventory_contract() {
	g_inventory_diagnostic[0] = '\0';
	if (descan::g_eventreceiver_processeventargs == nullptr) {
		copy_bounded(g_inventory_diagnostic, sizeof(g_inventory_diagnostic), "event receiver unavailable");
		return false;
	}
	idEventDefInterfaceLocal* events = idEventDefInterfaceLocal::Singleton();
	if (events == nullptr) {
		copy_bounded(g_inventory_diagnostic, sizeof(g_inventory_diagnostic), "event metadata unavailable");
		return false;
	}
	const InventoryEventContract contracts[] = {
		{"numOfItemTypeInInventory", 0x146, 1, "char*", 's', 'i'},
		{"getEquippedWeaponDecl", 0x14F, 0, nullptr, '\0', 'd'},
		{"doesPlayerHavePerk", 0x26C, 1, "char*", 's', 'b'},
		{"isPlayerPerkActive", 0x26D, 1, "char*", 's', 'b'},
	};
	for (const InventoryEventContract& contract : contracts) {
		if (!validate_inventory_event(contract, events)) {
			std::snprintf(g_inventory_diagnostic, sizeof(g_inventory_diagnostic),
				"metadata mismatch: %s", contract.name);
			return false;
		}
	}
	copy_bounded(g_inventory_diagnostic, sizeof(g_inventory_diagnostic), "candidate validated; invocation disabled");
	return true;
}

const char kSupportedExecutableSha256[] =
	"1c4d52fb72fb8b89ff3282bc58e5bbd5c1bf9194805745c3a7de55b591c62a8f";

bool hash_executable_sha256(const char* path, char output[65]) {
	copy_bounded(output, 65, "");
	HANDLE file = INVALID_HANDLE_VALUE;
	BCRYPT_ALG_HANDLE algorithm = nullptr;
	BCRYPT_HASH_HANDLE hash = nullptr;
	std::vector<BYTE> object;
	BYTE digest[32]{};
	ULONG object_bytes = 0;
	ULONG digest_bytes = sizeof(digest);
	ULONG result_bytes = 0;
	bool success = false;

	do {
		file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE)
			break;
		if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
			break;
		if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_bytes),
			sizeof(object_bytes), &result_bytes, 0) < 0 || object_bytes == 0)
			break;
		object.resize(object_bytes);
		if (BCryptCreateHash(algorithm, &hash, object.data(), object_bytes, nullptr, 0, 0) < 0)
			break;
		for (;;) {
			BYTE buffer[64 * 1024];
			DWORD read_bytes = 0;
			if (!ReadFile(file, buffer, sizeof(buffer), &read_bytes, nullptr))
				break;
			if (read_bytes == 0) {
				success = BCryptFinishHash(hash, digest, digest_bytes, 0) >= 0;
				break;
			}
			if (BCryptHashData(hash, buffer, read_bytes, 0) < 0)
				break;
		}
		if (!success)
			break;
		for (std::size_t i = 0; i < sizeof(digest); ++i)
			std::snprintf(output + (i * 2), 3, "%02x", digest[i]);
	} while (false);

	if (hash != nullptr)
		BCryptDestroyHash(hash);
	if (algorithm != nullptr)
		BCryptCloseAlgorithmProvider(algorithm, 0);
	if (file != INVALID_HANDLE_VALUE)
		CloseHandle(file);
	return success;
}

std::string query_version_string(const std::string& data, WORD language, WORD codepage, const char* field) {
	char key[128]{};
	std::snprintf(key, sizeof(key), "\\StringFileInfo\\%04x%04x\\%s", language, codepage, field);
	LPVOID value = nullptr;
	UINT value_size = 0;
	if (!VerQueryValueA(const_cast<char*>(data.data()), key, &value, &value_size)
		|| value == nullptr || value_size == 0)
		return {};
	const char* actual = static_cast<const char*>(value);
	std::size_t length = 0;
	while (length < value_size && actual[length] != '\0')
		++length;
	return std::string(actual, length);
}

bool validate_native_build_identity() {
	HMODULE module = GetModuleHandleA("DOOMEternalx64vk.exe");
	if (module == nullptr)
		return false;
	char path[MAX_PATH]{};
	if (GetModuleFileNameA(module, path, sizeof(path)) == 0)
		return false;
	const char* basename = std::strrchr(path, '\\');
	if (basename == nullptr)
		basename = path;
	else
		++basename;
	if (_stricmp(basename, "DOOMEternalx64vk.exe") != 0)
		return false;
	DWORD ignored = 0;
	DWORD bytes = GetFileVersionInfoSizeA(path, &ignored);
	if (bytes == 0)
		return false;
	std::string data(bytes, '\0');
	if (!GetFileVersionInfoA(path, 0, bytes, &data[0]))
		return false;
	VS_FIXEDFILEINFO* fixed = nullptr;
	UINT fixed_bytes = 0;
	if (!VerQueryValueA(&data[0], "\\", reinterpret_cast<LPVOID*>(&fixed), &fixed_bytes)
		|| fixed == nullptr || fixed_bytes < sizeof(VS_FIXEDFILEINFO)
		|| fixed->dwSignature != 0xFEEF04BD)
		return false;
	struct Translation { WORD language; WORD codepage; };
	LPVOID translations = nullptr;
	UINT translation_bytes = 0;
	if (!VerQueryValueA(&data[0], "\\VarFileInfo\\Translation", &translations, &translation_bytes)
		|| translations == nullptr || translation_bytes < sizeof(Translation))
		return false;
	bool resource_match = false;
	std::string product_name;
	std::string original_filename;
	std::string product_version;
	std::string file_version;
	for (UINT offset = 0; offset + sizeof(Translation) <= translation_bytes; offset += sizeof(Translation)) {
		const Translation translation = static_cast<const Translation*>(translations)[offset / sizeof(Translation)];
		const std::string translated_product_name = query_version_string(data, translation.language, translation.codepage, "ProductName");
		const std::string translated_original_filename = query_version_string(data, translation.language, translation.codepage, "OriginalFilename");
		const std::string translated_product_version = query_version_string(data, translation.language, translation.codepage, "ProductVersion");
		const std::string translated_file_version = query_version_string(data, translation.language, translation.codepage, "FileVersion");
		if (product_name.empty()) product_name = translated_product_name;
		if (original_filename.empty()) original_filename = translated_original_filename;
		if (product_version.empty()) product_version = translated_product_version;
		if (file_version.empty()) file_version = translated_file_version;
		resource_match = translated_product_name == "DOOMEternal"
			&& translated_original_filename == "DOOMEternalx64vk.exe"
			&& translated_product_version == "1.0.0.1"
			&& translated_file_version == "1.0.0.1";
		if (resource_match)
			break;
	}
	char fixed_file[32]{};
	char fixed_product[32]{};
	std::snprintf(fixed_file, sizeof(fixed_file), "%u.%u.%u.%u",
		HIWORD(fixed->dwFileVersionMS), LOWORD(fixed->dwFileVersionMS),
		HIWORD(fixed->dwFileVersionLS), LOWORD(fixed->dwFileVersionLS));
	std::snprintf(fixed_product, sizeof(fixed_product), "%u.%u.%u.%u",
		HIWORD(fixed->dwProductVersionMS), LOWORD(fixed->dwProductVersionMS),
		HIWORD(fixed->dwProductVersionLS), LOWORD(fixed->dwProductVersionLS));
	char sha256[65]{};
	const bool hash_read = hash_executable_sha256(path, sha256);
	std::snprintf(g_runtime_info.game_build, sizeof(g_runtime_info.game_build),
		"DEV;V=%s;SHA256=<unavailable>",
		(product_version == "1.0.0.1" && file_version == "1.0.0.1") ? "1.0.0.1" : "<mismatch>");
	if (hash_read)
		std::snprintf(g_runtime_info.game_build, sizeof(g_runtime_info.game_build),
			"DEV;V=%s;SHA256=%.*s",
			(product_version == "1.0.0.1" && file_version == "1.0.0.1") ? "1.0.0.1" : "<mismatch>",
			static_cast<int>(sizeof(g_runtime_info.game_build) - 22), sha256);
	g_runtime_info.game_build[sizeof(g_runtime_info.game_build) - 1] = '\0';
	return resource_match && hash_read && _stricmp(sha256, kSupportedExecutableSha256) == 0
		&& std::strcmp(fixed_file, "1.0.0.1") == 0
		&& std::strcmp(fixed_product, "1.0.0.1") == 0;
}

void copy_bounded(char* destination, std::size_t capacity, const char* source) {
	if (capacity == 0)
		return;
	if (source == nullptr)
		source = "";
	std::strncpy(destination, source, capacity - 1);
	destination[capacity - 1] = '\0';
}

APDeathLinkStatus find_deathlink_result(const char* request_id);

void append_event(APEventType type, const char* request_id) {
	APEvent event{};
	event.type = type;
	copy_request_id(event.request_id, request_id);
	std::lock_guard<std::mutex> lock(g_event_mutex);
	event.sequence = g_events.next_sequence++;
	g_events.entries[g_events.next] = event;
	g_events.next = (g_events.next + 1) % kEventCapacity;
	if (g_events.size < kEventCapacity)
		++g_events.size;
}

void record_snapshot_transitions(const APRuntimeSnapshot& previous, const APRuntimeSnapshot& current) {
	if (!previous.player_valid || !previous.dead_valid || !current.player_valid || !current.dead_valid
		|| previous.dead == current.dead)
		return;
	if (current.dead) {
		char request_id[64]{};
		{
			std::lock_guard<std::mutex> lock(g_deathlink_mutex);
			if (g_has_associated_deathlink && g_pump_count <= g_awaiting_deadline) {
				copy_request_id(request_id, g_associated_deathlink_id);
				g_has_associated_deathlink = false;
				g_associated_deathlink_id[0] = '\0';
				g_awaiting_deadline = 0;
			}
		}
		append_event(AP_EVENT_PLAYER_DEAD, request_id);
		if (request_id[0] != '\0') {
			std::lock_guard<std::mutex> lock(g_deathlink_mutex);
			copy_request_id(g_completed_deathlinks[g_completed_deathlink_next].request_id, request_id);
			g_completed_deathlinks[g_completed_deathlink_next].status = AP_DEATHLINK_INVOKED;
			g_completed_deathlink_next = (g_completed_deathlink_next + 1) % kDeathLinkCapacity;
			if (g_completed_deathlink_count < kDeathLinkCapacity)
				++g_completed_deathlink_count;
		}
	} else {
		append_event(AP_EVENT_PLAYER_RESPAWNED, "");
	}
}

bool snapshot_materially_changed(const APRuntimeSnapshot& left, const APRuntimeSnapshot& right) {
	return left.runtime_ready != right.runtime_ready
		|| left.player_valid != right.player_valid
		|| left.dead_valid != right.dead_valid
		|| left.dead != right.dead
		|| left.health_valid != right.health_valid
		|| left.max_health_valid != right.max_health_valid
		|| left.health != right.health
		|| left.max_health != right.max_health
		|| left.game_state != right.game_state
		|| left.game_state_valid != right.game_state_valid
		|| left.map_valid != right.map_valid
		|| std::memcmp(left.map, right.map, sizeof(left.map)) != 0
		|| left.checkpoint_valid != right.checkpoint_valid
		|| std::memcmp(left.checkpoint, right.checkpoint, sizeof(left.checkpoint)) != 0;
}

void publish_snapshot() {
	APRuntimeSnapshot snapshot{};
	snapshot.runtime_ready = g_snapshot_access_supported;
	if (g_snapshot_access_supported) {
		void* player = get_local_player();
		snapshot.player_valid = player != nullptr;
		if (player != nullptr) {
			snapshot.health_valid = true;
			snapshot.health = static_cast<float>(get_classfield_int(player, g_health_field));
			snapshot.max_health_valid = true;
			snapshot.max_health = static_cast<float>(get_classfield_int(player, g_max_health_field));
			snapshot.dead_valid = true;
			snapshot.dead = get_classfield_boolean(player, g_dead_field);
		}
		snapshot.checkpoint_valid = !gCurrentCheckpointName.empty();
		if (snapshot.checkpoint_valid)
			copy_bounded(snapshot.checkpoint, sizeof(snapshot.checkpoint), gCurrentCheckpointName.c_str());
	}
	void* level_map = g_map_access_supported ? get_level_map() : nullptr;
	if (level_map != nullptr) {
		const idStr* map_name = reinterpret_cast<const idStr*>(reinterpret_cast<const char*>(level_map) + g_map_name_field->offset);
		if (map_name->data != nullptr && map_name->data[0] != '\0') {
			snapshot.map_valid = true;
			copy_bounded(snapshot.map, sizeof(snapshot.map), map_name->data);
		}
	}
	APRuntimeSnapshot previous{};
	{
	std::lock_guard<std::mutex> lock(g_snapshot_mutex);
	previous = g_runtime_snapshot;
	const std::uint64_t milliseconds = static_cast<std::uint64_t>(
		std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::system_clock::now().time_since_epoch()).count());
	g_timestamp = milliseconds > g_timestamp ? milliseconds : g_timestamp + 1;
	if (snapshot_materially_changed(snapshot, g_runtime_snapshot))
		++g_sequence;
	snapshot.sequence = g_sequence;
	snapshot.timestamp = g_timestamp;
	g_runtime_snapshot = snapshot;
	}
	record_snapshot_transitions(previous, snapshot);
}

APDeathLinkStatus find_deathlink_result(const char* request_id) {
	const std::size_t oldest = (g_completed_deathlink_next + kDeathLinkCapacity
		- g_completed_deathlink_count) % kDeathLinkCapacity;
	for (std::size_t i = 0; i < g_completed_deathlink_count; ++i) {
		const std::size_t index = (oldest + i) % kDeathLinkCapacity;
		if (std::strcmp(g_completed_deathlinks[index].request_id, request_id) == 0)
			return g_completed_deathlinks[index].status;
	}
	for (std::size_t i = 0; i < g_pending_deathlink_count; ++i) {
		if (std::strcmp(g_pending_deathlinks[i].request_id, request_id) == 0)
			return AP_DEATHLINK_QUEUED;
	}
	return AP_DEATHLINK_INVALID;
}

APDeathLinkStatus process_one_deathlink() {
	PendingDeathLink pending{};
	{
		std::lock_guard<std::mutex> lock(g_deathlink_mutex);
		if (g_has_associated_deathlink)
			return AP_DEATHLINK_INVOKED;
		if (g_pending_deathlink_count == 0)
			return AP_DEATHLINK_INVALID;
		pending = g_pending_deathlinks[0];
		for (std::size_t i = 1; i < g_pending_deathlink_count; ++i)
			g_pending_deathlinks[i - 1] = g_pending_deathlinks[i];
		--g_pending_deathlink_count;
	}

	APDeathLinkStatus status = AP_DEATHLINK_ERROR;
	if (!g_deathlink_supported.load()) {
		status = AP_DEATHLINK_UNSUPPORTED;
	} else {
		void* player = get_local_player();
		APRuntimeSnapshot snapshot{};
		get_runtime_snapshot(&snapshot);
		if (player == nullptr || !snapshot.runtime_ready || !snapshot.player_valid
			|| !snapshot.dead_valid || snapshot.dead) {
			status = AP_DEATHLINK_INVALID_PLAYER;
		} else if (descan::g_eventreceiver_processeventargs == nullptr || g_request_suicide_event == nullptr) {
			status = AP_DEATHLINK_ERROR;
		} else {
			idEventArg result{};
			call_as<void>(descan::g_eventreceiver_processeventargs, player, &result,
				g_request_suicide_event, &g_null_eventargs);
			std::lock_guard<std::mutex> lock(g_deathlink_mutex);
			if (g_has_associated_deathlink) {
				status = AP_DEATHLINK_ERROR;
			} else {
				copy_request_id(g_associated_deathlink_id, pending.request_id);
				g_has_associated_deathlink = true;
				g_awaiting_deadline = g_pump_count + kDeathLinkTransitionTimeoutPumps;
				status = AP_DEATHLINK_INVOKED;
			}
		}
	}
	if (status != AP_DEATHLINK_INVOKED) {
		std::lock_guard<std::mutex> lock(g_deathlink_mutex);
		copy_request_id(g_completed_deathlinks[g_completed_deathlink_next].request_id, pending.request_id);
			g_completed_deathlinks[g_completed_deathlink_next].status = status;
			g_completed_deathlink_next = (g_completed_deathlink_next + 1) % kDeathLinkCapacity;
			if (g_completed_deathlink_count < kDeathLinkCapacity)
				++g_completed_deathlink_count;
	}
	return status;
}

Response make_status_response(std::uint64_t id, Status status) {
	Response response{};
	response.id = id;
	response.protocol_version = kProtocolVersion;
	response.status = status;
	response.runtime_state = g_runtime_state.load();
	response.capabilities = g_capabilities.load();
	return response;
}

Response* find_completed(std::uint64_t id) {
	for (std::size_t i = 0; i < g_completed.size; ++i) {
		Response& response = g_completed.entries[i];
		if (response.id == id)
			return &response;
	}
	return nullptr;
}

void cache_completed(const Response& response) {
	if (Response* existing = find_completed(response.id)) {
		*existing = response;
		return;
	}

	g_completed.entries[g_completed.next] = response;
	g_completed.next = (g_completed.next + 1) % kCompletedCacheCapacity;
	if (g_completed.size < kCompletedCacheCapacity)
		++g_completed.size;
}

Status complete_request(const Request& request, Response* response) {
	if (request.protocol_version != kProtocolVersion) {
		*response = make_status_response(request.id, Status::InvalidProtocol);
		return Status::InvalidProtocol;
	}

	if (request.operation != Operation::GetRuntimeStatus || request.requested_capabilities != 0) {
		*response = make_status_response(request.id, Status::Unsupported);
		return Status::Unsupported;
	}

	*response = make_status_response(request.id, Status::Ok);
	return response->status;
}

} // namespace

void initialize() {
	std::lock_guard<std::mutex> lock(g_snapshot_mutex);
	std::memset(&g_runtime_info, 0, sizeof(g_runtime_info));
	copy_bounded(g_runtime_info.runtime_name, sizeof(g_runtime_info.runtime_name), "Meathook-AP");
	copy_bounded(g_runtime_info.runtime_version, sizeof(g_runtime_info.runtime_version), "beta.5-A");
	g_runtime_info.protocol_version = kProtocolVersion;
	copy_bounded(g_runtime_info.game_build, sizeof(g_runtime_info.game_build), "UNKNOWN");
	const bool contract_supported = validate_supported_contract();
	g_snapshot_access_supported = contract_supported;
	g_runtime_state.store(contract_supported ? RuntimeState::Ready : RuntimeState::Unsupported);
	g_capabilities.store(CapabilityRuntimeInfo | (contract_supported ? CapabilityRuntimeSnapshot : CapabilityNone));
	g_runtime_info.build_supported = contract_supported;
	g_runtime_info.hooks_ready = contract_supported;
	g_runtime_info.capability_runtime_snapshot = contract_supported;
	g_map_access_supported = contract_supported;
	g_runtime_info.capability_map = contract_supported;
	g_runtime_info.capability_game_state = false;
	g_runtime_info.capability_checkpoint = contract_supported;
	g_deathlink_supported.store(contract_supported && validate_deathlink_event() && validate_native_build_identity());
	g_event_access_supported.store(contract_supported);
	g_runtime_info.capability_native_deathlink = g_deathlink_supported.load();
	g_runtime_info.capability_native_events = g_event_access_supported.load();
	g_runtime_info.capability_extra_life_telemetry = false;
	g_inventory_contract_valid = validate_inventory_contract();
	g_runtime_info.capability_inventory_read = false;
	g_capabilities.store(CapabilityRuntimeInfo
		| (contract_supported ? CapabilityRuntimeSnapshot : CapabilityNone)
		| (g_deathlink_supported.load() ? CapabilityNativeDeathLink : CapabilityNone)
		| (g_event_access_supported.load() ? CapabilityNativeEvents : CapabilityNone));
	g_sequence = 0;
	g_timestamp = 0;
	g_runtime_snapshot = APRuntimeSnapshot{};
	{
		std::lock_guard<std::mutex> lock(g_deathlink_mutex);
		g_pending_deathlink_count = 0;
		g_completed_deathlink_next = 0;
		g_completed_deathlink_count = 0;
		g_has_associated_deathlink = false;
		g_associated_deathlink_id[0] = '\0';
		g_pump_count = 0;
		g_awaiting_deadline = 0;
	}
	{
		std::lock_guard<std::mutex> lock(g_event_mutex);
		g_events = EventRing{};
	}
}

void pump() {
	for (;;) {
		Request request{};
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			if (g_queue.size == 0)
				break;
			request = g_queue.entries[g_queue.head];
			g_queue.head = (g_queue.head + 1) % kRequestQueueCapacity;
			--g_queue.size;
		}
		Response response{};
		complete_request(request, &response);
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			cache_completed(response);
		}
	}
	publish_snapshot();
	++g_pump_count;
	{
		std::lock_guard<std::mutex> lock(g_deathlink_mutex);
		APRuntimeSnapshot current_snapshot{};
		get_runtime_snapshot(&current_snapshot);
		if (g_has_associated_deathlink && (!current_snapshot.runtime_ready || !current_snapshot.player_valid)) {
			copy_request_id(g_completed_deathlinks[g_completed_deathlink_next].request_id, g_associated_deathlink_id);
			g_completed_deathlinks[g_completed_deathlink_next].status = AP_DEATHLINK_INVALID_PLAYER;
			g_completed_deathlink_next = (g_completed_deathlink_next + 1) % kDeathLinkCapacity;
			if (g_completed_deathlink_count < kDeathLinkCapacity) ++g_completed_deathlink_count;
			g_has_associated_deathlink = false;
			g_associated_deathlink_id[0] = '\0';
		}
		if (g_has_associated_deathlink && g_pump_count > g_awaiting_deadline) {
			copy_request_id(g_completed_deathlinks[g_completed_deathlink_next].request_id, g_associated_deathlink_id);
			g_completed_deathlinks[g_completed_deathlink_next].status = AP_DEATHLINK_ERROR;
			g_completed_deathlink_next = (g_completed_deathlink_next + 1) % kDeathLinkCapacity;
			if (g_completed_deathlink_count < kDeathLinkCapacity) ++g_completed_deathlink_count;
			g_has_associated_deathlink = false;
			g_associated_deathlink_id[0] = '\0';
		}
	}
	process_one_deathlink();
	publish_snapshot();
}

Status query_status(Response* response) {
	if (response == nullptr)
		return Status::InvalidRequest;

	std::lock_guard<std::mutex> lock(g_mutex);
	*response = make_status_response(0, Status::Ok);
	return response->status;
}

Status submit(const Request& request, Response* cached_response) {
	if (request.id == 0) {
		if (cached_response != nullptr)
			*cached_response = make_status_response(0, Status::InvalidRequest);
		return Status::InvalidRequest;
	}

	std::lock_guard<std::mutex> lock(g_mutex);
	if (Response* completed = find_completed(request.id)) {
		if (cached_response != nullptr)
			*cached_response = *completed;
		return Status::Duplicate;
	}

	for (std::size_t i = 0; i < g_queue.size; ++i) {
		const std::size_t index = (g_queue.head + i) % kRequestQueueCapacity;
		if (g_queue.entries[index].id == request.id)
			return Status::Duplicate;
	}

	if (request.protocol_version != kProtocolVersion) {
		if (cached_response != nullptr)
			*cached_response = make_status_response(request.id, Status::InvalidProtocol);
		return Status::InvalidProtocol;
	}

	if (g_queue.size == kRequestQueueCapacity)
		return Status::QueueFull;

	const std::size_t index = (g_queue.head + g_queue.size) % kRequestQueueCapacity;
	g_queue.entries[index] = request;
	++g_queue.size;
	return Status::Accepted;
}

void get_runtime_info(APRuntimeInfo* info) {
	if (info == nullptr)
		return;
	std::lock_guard<std::mutex> lock(g_snapshot_mutex);
	*info = g_runtime_info;
}

void get_runtime_snapshot(APRuntimeSnapshot* snapshot) {
	if (snapshot == nullptr)
		return;
	std::lock_guard<std::mutex> lock(g_snapshot_mutex);
	*snapshot = g_runtime_snapshot;
}

void get_inventory_item_count(const char* decl_name, APInventoryIntResult* result) {
	(void)decl_name;
	if (result == nullptr)
		return;
	std::memset(result, 0, sizeof(*result));
	result->status = AP_INVENTORY_UNSUPPORTED;
	copy_bounded(result->diagnostic, sizeof(result->diagnostic), g_inventory_diagnostic);
}

void has_player_perk(const char* decl_name, APInventoryBoolResult* result) {
	(void)decl_name;
	if (result == nullptr)
		return;
	std::memset(result, 0, sizeof(*result));
	result->status = AP_INVENTORY_UNSUPPORTED;
	copy_bounded(result->diagnostic, sizeof(result->diagnostic), g_inventory_diagnostic);
}

void is_player_perk_active(const char* decl_name, APInventoryBoolResult* result) {
	(void)decl_name;
	if (result == nullptr)
		return;
	std::memset(result, 0, sizeof(*result));
	result->status = AP_INVENTORY_UNSUPPORTED;
	copy_bounded(result->diagnostic, sizeof(result->diagnostic), g_inventory_diagnostic);
}

void get_equipped_weapon(APEquippedWeaponResult* result) {
	if (result == nullptr)
		return;
	std::memset(result, 0, sizeof(*result));
	result->status = AP_INVENTORY_UNSUPPORTED;
	copy_bounded(result->diagnostic, sizeof(result->diagnostic), g_inventory_diagnostic);
}

APDeathLinkStatus apply_deathlink(const char* request_id) {
	if (!valid_request_id(request_id))
		return AP_DEATHLINK_INVALID;
	std::lock_guard<std::mutex> lock(g_deathlink_mutex);
	APDeathLinkStatus existing = find_deathlink_result(request_id);
	if (existing != AP_DEATHLINK_INVALID)
		return existing;
	if (g_has_associated_deathlink && std::strcmp(g_associated_deathlink_id, request_id) == 0)
		return AP_DEATHLINK_INVOKED;
	if (!g_deathlink_supported.load())
		return AP_DEATHLINK_UNSUPPORTED;
	if (g_pending_deathlink_count >= kDeathLinkCapacity)
		return AP_DEATHLINK_ERROR;
	copy_request_id(g_pending_deathlinks[g_pending_deathlink_count].request_id, request_id);
	++g_pending_deathlink_count;
	return AP_DEATHLINK_QUEUED;
}

void get_events_since(std::uint64_t after_sequence, std::uint32_t max_count, APEventBatch* batch) {
	if (batch == nullptr)
		return;
	std::memset(batch, 0, sizeof(*batch));
	std::lock_guard<std::mutex> lock(g_event_mutex);
	if (g_events.size == 0)
		return;
	const std::size_t oldest = (g_events.next + kEventCapacity - g_events.size) % kEventCapacity;
	batch->oldest_sequence = g_events.entries[oldest].sequence;
	batch->latest_sequence = g_events.entries[(g_events.next + kEventCapacity - 1) % kEventCapacity].sequence;
	batch->gap = after_sequence != UINT64_MAX && after_sequence + 1 < batch->oldest_sequence;
	const std::size_t limit = std::min<std::size_t>(max_count, kEventCapacity);
	for (std::size_t i = 0; i < g_events.size && batch->count < limit; ++i) {
		const APEvent& event = g_events.entries[(oldest + i) % kEventCapacity];
		if (event.sequence > after_sequence)
			batch->events[batch->count++] = event;
	}
}

} // namespace mh_ap
