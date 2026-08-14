#include "xbyak/xbyak.h"
#include "mh_defs.hpp"

#include "game_exe_interface.hpp"
#include "doomoffs.hpp"
#include "meathook.h"
#include "cmdsystem.hpp"
#include "idtypeinfo.hpp"
#include "eventdef.hpp"
#include "scanner_core.hpp"
#include "idLib.hpp"
#include "idStr.hpp"
#include "clipboard_helpers.hpp"
#include <vector>
#include "errorhandling_hooks.hpp"
#include "gameapi.hpp"
#include "idmath.hpp"
#include "memscan.hpp"
#include "mh_memmanip_cmds.hpp"
#include "snaphakalgo.hpp"
#include <string.h>
#include "mh_guirender.hpp"
#include "mh_editor_mode.hpp"
#include <string>

void cmd_mh_query_type(idCmdArgs* args) {

	if (args->argc < 2)
		return;
	auto t = idType::FindClassInfo(args->argv[1]);
	std::string clipdata{};
	char scratchbuf[2048];

	if (!t) {

		auto e = idType::FindEnumInfo(args->argv[1]);

		if (!e) {
			idLib::Printf("Type %s was not found.\n", args->argv[1]);
			return;
		}

		for (auto eval = e->values; eval && eval->name && eval->name[0]; eval++) {
			sprintf_s(scratchbuf, "\t%s\n", eval->name);

			clipdata += scratchbuf;

			idLib::Printf(scratchbuf);
		}
		idLib::Printf("Dumped type is a Enum\n");
	}
	else {

		for (auto bleh2 = t->variables; bleh2 && bleh2->name; ++bleh2) {

			sprintf_s(scratchbuf, "\t%s%s %s\n", bleh2->type, bleh2->ops, bleh2->name);

			if (bleh2->comment && bleh2->comment[0]) {
				clipdata += "\n\t//";
				clipdata += bleh2->comment;
				clipdata += "\n";
			}
			clipdata += scratchbuf;
			idLib::Printf(scratchbuf);
		}

		idLib::Printf("\nDumped type is a Class\n");
		if (t->superType && t->superType[0]) {
			idLib::Printf("\nFor more fields this class has, use mh_type on its superclass, %s\n", t->superType);
		}
	}
	set_clipboard_data(clipdata.c_str());
}

void cmd_mh_printentitydef(idCmdArgs* args) {
#if 0
	if (args->argc < 2) {
		idLib::Printf("Not enough args.\n");
		return;
	}

#else
	void* ent = nullptr;

	if (args->argc < 2) {
		idLib::Printf("No entity name provided, using player look target.\n");
		ent = get_player_look_target();
	}
	else {
		idLib::Printf(args->argv[1]);

		ent = find_entity(args->argv[1]);
	}
#endif
	if (!ent) {
		idLib::Printf("Couldn't find entity %s.\n", args->argv[1]);
		return;
	}

	auto entdef_field = idType::FindClassField("idEntity", "entityDef");
	if (!entdef_field) {
		idLib::Printf("Couldn't locate entityDef field!! This is bad.\n");
		return;
	}

	void* edef = *reinterpret_cast<void**>(reinterpret_cast<char*>(ent) + entdef_field->offset);

	if (!edef) {
		idLib::Printf("Entity %s's entitydef is null.\n", args->argv[1]);
		return;

	}

	idStr outstr;
	call_as<void>(descan::g_declentitydef_gettextwithinheritance, edef, &outstr, false);

	idLib::Printf("%s\n", outstr.data);
	set_clipboard_data(outstr.data);


}


std::vector<std::string> gActiveEncounterNames;
void cmd_active_encounter(idCmdArgs* args)
{
#ifndef MH_ETERNAL_V6
	// Following chain: idGameSystemLocal->idSaveGameSerializer->idSerializeFunctor<idMetrics>->idMetrics->idPlayerMetrics[0]
	long long* idSaveGameSerializer = ((long long*)((char*)descan::g_idgamesystemlocal + 0xB0));
	long long* idSerializerFunctor = (long long*)(*(idSaveGameSerializer));
	long long* idMetrics = (long long*)(*(idSerializerFunctor + 0x59)); // 0x2C8
	long long* idPlayerMetrics = (long long*)(*(idMetrics + 1)); // 0x8
	long long* EncounterArray = (long long*)(*(idPlayerMetrics + 5)); // 0x28
	long long* Encounter = (long long*)(*(EncounterArray));

	long long EncounterListSize = *(Encounter - 1) - (4 * 0x8); // This seems to work but looks a bit fragile.
	long long* EncounterManager = Encounter + 1; // Skip first encounter which always points to an instance of idGameChallenge_CampaignSinglePlayer.
	size_t Index = 1;
	while ((long long)Index < (EncounterListSize / 8)) {
		// Active
		if (EncounterManager == 0) {
			break;
		}

		long long* Encounter = ((long long*)(*EncounterManager));
		if (Encounter == nullptr) {
			break;
		}

		int Active = *((int*)(Encounter + 0xC5)); // 0x628 -- Fragile offset for determining if an encounter is active.
		if (Active != 0) {
			char* Name = (char*)*(Encounter + 0x9); // 0x48 -- Encounter manager pointer to name string.
			if (strcmp("player1", Name) == 0) {
				break;
			}

			char String[MAX_PATH];
			sprintf_s(String, "Active name %s\n", Name);
			OutputDebugStringA(String);
			idLib::Printf(String);

			// Name
			gActiveEncounterNames.push_back(Name);
		}
		EncounterManager++;
		Index += 1;
	}


#else
	for (void* encounter = find_next_entity_with_class("idEncounterManager"); encounter; encounter = find_next_entity_with_class("idEncounterManager", encounter)) {

		if (get_classfield_int(encounter, "idBloatedEntity", "thinkFlags")) {//Active != 0) {
			const char* name = get_entity_name(encounter);


			char String[MAX_PATH];
			sprintf_s(String, "Active name %s\n", name);
			OutputDebugStringA(String);
			idLib::Printf(String);

			// Name
			gActiveEncounterNames.push_back(name);
		}
	}
#endif
}

std::string gCurrentCheckpointName;
void cmd_current_checkpoint(idCmdArgs* args)
{
	// Following chain: idGameSystemLocal->idGameSpawnInfo->checkpointName
	auto MapInstanceVar = idType::FindClassField("idGameSystemLocal", "mapInstance");
	char* idMapInstanceLocal = ((char*)descan::g_idgamesystemlocal + MapInstanceVar->offset);

	char String[MAX_PATH * 10];
	// (char*)((*((longlong*)((char*)descan::g_idgamesystemlocal + 0x50))) + 0x1108)
	/*  AIIIIIEEEE */
	char* Name = (char*)"NULL";
	if (idMapInstanceLocal != nullptr) {
		//auto GameSpawnNameVar = idType::FindClassField(MapInstanceVar->type, "gameSpawnInfo");
		//auto CheckPointNameVar = idType::FindClassField(GameSpawnNameVar->type, "checkpointName");
		//Name = (char*)(*((unsigned long long*)(idMapInstanceLocal + CheckPointNameVar->offset + GameSpawnNameVar->offset))); // checkpoint string. 0x1108 byte

		//this is bad and could break
		//todo: scan to extract this offset
		Name = ((char*)(*((long long*)(idMapInstanceLocal)) + 0x1108));
	}

	if (Name == nullptr || Name[0] == 0) {
		Name = (char*)"<no checkpoint loaded>";
	}

	sprintf_s(String, "Current checkpoint name %s\n", Name);
	OutputDebugStringA(String);
	idLib::Printf(String);
	gCurrentCheckpointName = (Name[0] == '<') ? "" : Name;
}

static mh_fieldcached_t<char*> g_field_resourcelist_type{};
static mh_fieldcached_t<char*> g_field_resourcelist_class{};

static mh_fieldcached_t<void*> g_field_resourcelist_next{};

static void mh_list_resource_lists(idCmdArgs* args) {

	void* listofresourcelists = *((void**)descan::g_listOfResourceLists);


	std::string workbuf{};

#define		RESLIST_TABULATION		"\t\t\t\t\t\t\t\t"


	workbuf += "Resource lists\n\n\tCLASSNAME" RESLIST_TABULATION "TYPENAME\n";



	while (listofresourcelists) {
		const char* type = *g_field_resourcelist_type(listofresourcelists, "idResourceList", "typeName");
		const char* classname = *g_field_resourcelist_class(listofresourcelists, "idResourceList", "className");

		workbuf += "\t";
		workbuf += classname;
		workbuf += RESLIST_TABULATION;
		workbuf += type;
		workbuf += "\n";

		listofresourcelists = *g_field_resourcelist_next(listofresourcelists, "idResourceList", "next");
	}

	set_clipboard_data(workbuf.c_str());

	idLib::Printf(workbuf.c_str());
}

static void mh_list_resourcelist_contents(idCmdArgs* args) {

	if (args->argc < 2) {
		idLib::Printf("Expected a resource classname for the first arg\n");
		return;
	}

	void* reslist = resourcelist_for_classname(args->argv[1]);

	if (!reslist) {
		idLib::Printf("Couldn't get resourcelist for %s!\n", args->argv[1]);
		return;
	}

	void* reslistt = idResourceList_to_resourceList_t(reslist);
	if (!reslistt) {
		idLib::Printf("No resourceList_t for idResourceList?? how??\n");
		return;
	}

	unsigned reslen = resourceList_t_get_length(reslistt);

	std::string buildbuf{};


	for (unsigned i = 0; i < reslen; ++i) {

		void* res = resourceList_t_lookup_index(reslistt, i);
		if (!res)
			continue;
		const char* resname = get_resource_name(res);
		if (!resname)
			continue;

		if (args->argc > 2) {
			if (!sh::string::strstr(resname, args->argv[2])) {
				continue;
			}
		}

		buildbuf += resname;
		buildbuf += "\t\t\t";
		buildbuf += *reinterpret_cast<const char**>(reslistt);
		buildbuf += "\n";

	}

	set_clipboard_data(buildbuf.c_str());

	idLib::Printf(buildbuf.c_str());

}
static void mh_kw(idCmdArgs* args) {

	std::string output = idType::keyword_search((const char**)&args->argv[0], args->argc);
	set_clipboard_data(output.c_str());
	idLib::Printf("%s", output.c_str());
}

static void mh_list_identity_subclasses(idCmdArgs* args) {


	auto vv = idType::get_entity_inheritors();
	std::string result = "Entity subclasses:\n";

	for (auto&& memb : *vv) {

		auto typ = from_de_rva<idTypeInfo>(memb);


		if (args->argc > 1) {

			if (!~sh::string::find_string_insens(typ->classname, args->argv[1])) {
				continue;
			}
		}
		result += "\t";
		result += typ->classname;

		result += "\n";

	}

	idLib::Printf("%s\n", result.c_str());
	set_clipboard_data(result.c_str());

}

void install_query_cmds() {
	idCmd::register_command("mh_printentitydef", cmd_mh_printentitydef, "Print the entitydef of the entity with the provided name to the console");
	idCmd::register_command("mh_active_encounter", cmd_active_encounter, "Get the list of active encounter managers");
	idCmd::register_command("mh_current_checkpoint", cmd_current_checkpoint, "Get the current checkpoint name");
	idCmd::register_command("mh_type", cmd_mh_query_type, "Dump fields for provided class");
	idCmd::register_command("mh_list_resource_lists", mh_list_resource_lists, "lists all resource lists by classname/typename, copying the result to the clipboard (the clipboard might not be helpful here)");
	idCmd::register_command("mh_list_resources_of_class", mh_list_resourcelist_contents, "<resourcelist classname> lists all resources in a given list, copying result to clipboard");
	idCmd::register_command("mh_kw", mh_kw, "Searches all types, enums, typedefs, their comments, field names, typename, template args,eventdefs,vtbl names, cvar names, cvar descriptions for the provided keywords");
	idCmd::register_command("mh_list_entity_types", mh_list_identity_subclasses, "<filter> lists the names of all subclasses of idEntity with optional filter");
}
