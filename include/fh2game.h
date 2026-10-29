/*
 * fh2game.h -- names for the places in Forza Horizon 2's code and data that mods use
 * (MIT licensed, see LICENSE).
 *
 * Azimuth runs one build of the game: forza_durango_release_final.exe of 24 November 2015
 * (TimeDateStamp 0x565501ED, SizeOfImage 0x3A18000), mapped at 0x140000000. Every FH2_GAME_...
 * below is an offset from the game's base, the address wait_for_code() returns, and logs and
 * crash reports write it as game+offset. FH2_GAME_..._BYTES, where there is one, is what the
 * game has there on disk, as a string, for a check before you hook or call it:
 *
 *     char* game = (char*)api->wait_for_code(30000);
 *     if (game && memcmp(game + FH2_GAME_FORZACAR_KIND, FH2_GAME_FORZACAR_KIND_BYTES,
 *                        sizeof FH2_GAME_FORZACAR_KIND_BYTES - 1) == 0)
 *         api->hook_function(mod, "car kind", game + FH2_GAME_FORZACAR_KIND, detour, &original);
 *
 * The platform checks hooks against the exe on disk too, but a check of your own lets you turn
 * off just the feature that needs it, and say so in your log. Names ending in _AT are an
 * instruction inside a function (for hook_instruction or patch_bytes), _CALL a call instruction
 * (for hook_call), _VTABLE a vtable (its bytes are slot 0, as mapped at 0x140000000); the rest are
 * functions, and data without bytes. Each entry says what it is and which released mods use it.
 *
 * To add one: a pull request with the offset, what it is, what the game has there (read it from
 * the exe on disk), and the mod that uses it. Names follow the game's own class and function
 * names, from its RTTI and strings, where there are some.
 *
 * The platform's own hook sites are at the end. Don't hook those.
 */
#ifndef FH2GAME_H
#define FH2GAME_H

/* ---- Cars ---------------------------------------------------------------------------------------- */

/* ForzaCar's vtable (RTTI .?AVForzaCar@@); slot 0 is its deleting destructor, 0x14028B380. */
#define FH2_GAME_FORZACAR_VTABLE 0x2C5CB80
#define FH2_GAME_FORZACAR_VTABLE_BYTES "\x80\xB3\x28\x40\x01\x00\x00\x00"

/* int(car): the car's kind (car+0x4D08): 0 the local player's, 1 a Drivatar, 2 traffic, 3 to 5
 * the same in another player's game. */
#define FH2_GAME_FORZACAR_KIND 0xF62280
#define FH2_GAME_FORZACAR_KIND_BYTES "\x8B\x81\x08\x4D\x00\x00\xC3"

/* void*(car): the car's collidable request set (car+0x4C58). */
#define FH2_GAME_FORZACAR_REQUESTS 0xF60510
#define FH2_GAME_FORZACAR_REQUESTS_BYTES "\x48\x8D\x81\x58\x4C\x00\x00\xC3"

/* ~ForzaCar(car), from the deleting destructor, before the car's memory is freed. */
#define FH2_GAME_FORZACAR_DTOR 0x289320
#define FH2_GAME_FORZACAR_DTOR_BYTES "\x48\x89\x4C\x24\x08\x56\x57\x41\x54\x41\x56\x41"

/* ForzaCar vtable slot 0x80: the per-frame update. */
#define FH2_GAME_FORZACAR_UPDATE 0x2A9400
#define FH2_GAME_FORZACAR_UPDATE_BYTES "\x48\x8B\xC4\x48\x89\x58\x18\x57\x48\x81\xEC\xA0"

/* ForzaCar vtable slot 0x130: works out the car's alpha and hands it to its presentation. */
#define FH2_GAME_FORZACAR_APPLY_COLLIDABLE 0x28D930
#define FH2_GAME_FORZACAR_APPLY_COLLIDABLE_BYTES "\x40\x53\x48\x83\xEC\x20\x8B\x05\x8C\x1E\x22\x03"

/* float(self, ForzaCar**): the car's alpha, from the newest request in its set (or a distance
 * fade). */
#define FH2_GAME_FORZACAR_ALPHA 0x28C590
#define FH2_GAME_FORZACAR_ALPHA_BYTES "\x48\x8B\xC4\x57\x48\x81\xEC\xC0\x00\x00\x00\x48"

/* bool(car, float abs_dt): online anti-griefing's rule for the local player's car, which ghosts
 * it. */
#define FH2_GAME_FORZACAR_SHOULD_START_GRIEFING 0x28D350
#define FH2_GAME_FORZACAR_SHOULD_START_GRIEFING_BYTES "\x48\x8B\xC4\x57\x48\x81\xEC\x90\x00\x00\x00\x48"

/* shared_ptr*(set, shared_ptr* out, const std::string* preset): a request handle for a collidable
 * preset, by name. */
#define FH2_GAME_COLLIDABLE_REQUESTS_MAKE 0xF5B420
#define FH2_GAME_COLLIDABLE_REQUESTS_MAKE_BYTES "\x48\x89\x54\x24\x10\x57\x48\x83\xEC\x50\x48\xC7"

/* uint8_t(set, shared_ptr handle): registers a request (and takes the handle over). */
#define FH2_GAME_COLLIDABLE_REQUESTS_ADD 0xF73080
#define FH2_GAME_COLLIDABLE_REQUESTS_ADD_BYTES "\x48\x89\x54\x24\x10\x56\x57\x41\x56\x48\x83\xEC"

/* uint8_t(set, const std::string* preset): removes the request with that preset. */
#define FH2_GAME_COLLIDABLE_REQUESTS_REMOVE 0xF5B890
#define FH2_GAME_COLLIDABLE_REQUESTS_REMOVE_BYTES "\x40\x53\x48\x83\xEC\x30\x4C\x8B\xCA\x48\x8B\x51"

/* void(car): hides a car (its presentation's HideCar). Only the request set's update calls it,
 * when the newest request's alpha drops to 0. */
#define FH2_GAME_CAR_HIDE 0xF649F0
#define FH2_GAME_CAR_HIDE_BYTES "\x40\x53\x48\x83\xEC\x20\x48\x8B\xD9\xC6\x81\xF1"

/* ---- Entity events ------------------------------------------------------------------------------- */

/* void*(entity, const void* symbol): queues an event for an entity. */
#define FH2_GAME_ENTITY_SEND_EVENT 0x190BF90
#define FH2_GAME_ENTITY_SEND_EVENT_BYTES "\x48\x83\xEC\x48\x48\x8D\x05\xBD\x41\x65\x01\x48"

/* void*(entity, const void* symbol, void* data): the same with data. */
#define FH2_GAME_ENTITY_SEND_EVENT_DATA 0x190BFD0
#define FH2_GAME_ENTITY_SEND_EVENT_DATA_BYTES "\x40\x53\x48\x83\xEC\x40\x48\x8B\xD9\xB9\x20\x00"

/* The event's symbol, a global its static initialiser makes. ARSpeedCamera sends it with data at
 * 0x1406B965E. */
#define FH2_GAME_EVENT_SPEED_ZONE_ACTIVATED 0x34BFF20

/* Sent at 0x1406B8EA8. */
#define FH2_GAME_EVENT_SPEED_ZONE_COMPLETED 0x34BFB10

/* Sent at 0x1406A2AF7. */
#define FH2_GAME_EVENT_SPEED_ZONE_ABORTED 0x34BFDB8

/* Sent with data at 0x1406B8A87, once the trap has been passed. */
#define FH2_GAME_EVENT_SPEED_TRAP_ACTIVATED 0x34BFDD8

/* ---- Collision and the world --------------------------------------------------------------------- */

/* CollisionWorld::GetPolys: the triangles a collision query's box touches. WreckingBall (solid
 * pylons). */
#define FH2_GAME_COLLISIONWORLD_GETPOLYS 0x12CC730
#define FH2_GAME_COLLISIONWORLD_GETPOLYS_BYTES "\x48\x8B\xC4\x44\x89\x40\x18\x48\x89\x48\x08\x55"

/* A collision template's grid query (template, box, max, triangles out, vertices out, flag).
 * WreckingBall. */
#define FH2_GAME_COLLISION_TEMPLATE_QUERY 0x12CC1A0
#define FH2_GAME_COLLISION_TEMPLATE_QUERY_BYTES "\x48\x89\x5C\x24\x20\x57\x41\x54\x41\x55\x41\x56"

/* Makes a collision instance's template available. WreckingBall. */
#define FH2_GAME_COLLISION_INSTANCE_TEMPLATE_LOAD 0x12B2500
#define FH2_GAME_COLLISION_INSTANCE_TEMPLATE_LOAD_BYTES "\x40\x57\x48\x83\xEC\x40\x48\xC7\x44\x24\x20\xFE"

/* A definition, by index (a leaf function: no unwind data). WreckingBall. */
#define FH2_GAME_COLLISION_DEFINITION 0x12CC010
#define FH2_GAME_COLLISION_DEFINITION_BYTES "\x48\x8B\x41\x08\x44\x0F\xB7\xC2\xC1\xEA\x10\x4A"

/* A definition's model, by index (a leaf function). WreckingBall. */
#define FH2_GAME_COLLISION_DEFINITION_MODEL 0x12CD300
#define FH2_GAME_COLLISION_DEFINITION_MODEL_BYTES "\x48\x8B\x41\x08\x44\x0F\xB7\xC2\xC1\xEA\x10\x4E"

/* StreamedPhysics::CollisionTemplateResource's vtable. WreckingBall. */
#define FH2_GAME_COLLISIONTEMPLATERESOURCE_VTABLE 0x2EB43C0
#define FH2_GAME_COLLISIONTEMPLATERESOURCE_VTABLE_BYTES "\x50\xF6\x2A\x41\x01\x00\x00\x00"

/* CWorldCollObjMan::AddCell(mgr, msg). WreckingBall. */
#define FH2_GAME_WORLDCOLLOBJMAN_ADD_CELL 0x36A330
#define FH2_GAME_WORLDCOLLOBJMAN_ADD_CELL_BYTES "\x48\x89\x5C\x24\x08\x57\x48\x81\xEC\x80\x00\x00"

/* CWorldCollObjMan::SpawnCell(mgr, cell). WreckingBall. */
#define FH2_GAME_WORLDCOLLOBJMAN_SPAWN_CELL 0x368D90
#define FH2_GAME_WORLDCOLLOBJMAN_SPAWN_CELL_BYTES "\x48\x89\x54\x24\x10\x55\x56\x57\x41\x54\x41\x55"

/* CWorldCollObjMan::MakeDynamic(mgr, obj). WreckingBall. */
#define FH2_GAME_WORLDCOLLOBJMAN_MAKE_DYNAMIC 0x368C60
#define FH2_GAME_WORLDCOLLOBJMAN_MAKE_DYNAMIC_BYTES "\x48\x89\x5C\x24\x10\x48\x89\x6C\x24\x18\x48\x89"

/* CWorldCollObjMan::BreakIntoPieces. WreckingBall. */
#define FH2_GAME_WORLDCOLLOBJMAN_BREAK_INTO_PIECES 0x36AC80
#define FH2_GAME_WORLDCOLLOBJMAN_BREAK_INTO_PIECES_BYTES "\x48\x8B\xC4\x48\x89\x50\x10\x48\x89\x48\x08\x55"

/* An instruction in BreakIntoPieces, where the loose copy's model is picked. WreckingBall. */
#define FH2_GAME_WORLDCOLLOBJMAN_BREAK_MODEL_AT 0x36AEAC
#define FH2_GAME_WORLDCOLLOBJMAN_BREAK_MODEL_AT_BYTES "\x48\x8D\x4D\xC0\x48\x89\x4C\x24\x20"

/* CWorldCollObjMan's update. WreckingBall. */
#define FH2_GAME_WORLDCOLLOBJMAN_UPDATE 0x384C00
#define FH2_GAME_WORLDCOLLOBJMAN_UPDATE_BYTES "\x48\x8B\xC4\x55\x56\x57\x41\x54\x41\x55\x41\x56"

/* CWorldCollObjMan's shutdown (vtable slot 1, and from its destructor). WreckingBall. */
#define FH2_GAME_WORLDCOLLOBJMAN_SHUTDOWN 0x382F20
#define FH2_GAME_WORLDCOLLOBJMAN_SHUTDOWN_BYTES "\x48\x89\x5C\x24\x08\x48\x89\x74\x24\x10\x57\x48"

/* Takes a slot for a new loose object, recycling the farthest loose piece. WreckingBall. */
#define FH2_GAME_WORLDCOLLOBJMAN_RECYCLE_SLOT 0x37A7D0
#define FH2_GAME_WORLDCOLLOBJMAN_RECYCLE_SLOT_BYTES "\x48\x89\x5C\x24\x08\x48\x89\x74\x24\x10\x57\x48"

/* CWorldCollObj's vtable; slot 0 is OnCollision. WreckingBall. */
#define FH2_GAME_WORLDCOLLOBJ_VTABLE 0x2C6A208
#define FH2_GAME_WORLDCOLLOBJ_VTABLE_BYTES "\x30\x82\x37\x40\x01\x00\x00\x00"

/* TrackPresentation11's destructor: the world (track) going away. WreckingBall. */
#define FH2_GAME_TRACKPRESENTATION11_DTOR 0x1377B50
#define FH2_GAME_TRACKPRESENTATION11_DTOR_BYTES "\x48\x89\x4C\x24\x08\x57\x48\x83\xEC\x30\x48\xC7"

/* Presentation11's render node update. WreckingBall. */
#define FH2_GAME_PRESENTATION11_RENDER_NODE_UPDATE 0x13A41E0
#define FH2_GAME_PRESENTATION11_RENDER_NODE_UPDATE_BYTES "\x48\x8B\xC4\x44\x89\x48\x20\x4C\x89\x40\x18\x48"

/* ---- Procedural models (trees and bushes) -------------------------------------------------------- */

/* ProceduralModelsData::Serialize(data, archive). WreckingBall. */
#define FH2_GAME_PROCEDURALMODELSDATA_SERIALIZE 0x1871B90
#define FH2_GAME_PROCEDURALMODELSDATA_SERIALIZE_BYTES "\x40\x57\x48\x83\xEC\x50\x48\xC7\x44\x24\x20\xFE"

/* vector<Group>::resize(vec, n). WreckingBall. */
#define FH2_GAME_PROCEDURAL_GROUPS_RESIZE 0x1881EC0
#define FH2_GAME_PROCEDURAL_GROUPS_RESIZE_BYTES "\x48\x89\x4C\x24\x08\x57\x48\x83\xEC\x40\x48\xC7"

/* Decodes one procedural instance. WreckingBall. */
#define FH2_GAME_PROCEDURAL_DECODE_INSTANCE 0x1874600
#define FH2_GAME_PROCEDURAL_DECODE_INSTANCE_BYTES "\x48\x83\xEC\x48\xC4\xC1\x7B\x12\x00\xC5\x78\x29"

/* Procedural::ProceduralModels' vtable. WreckingBall. */
#define FH2_GAME_PROCEDURALMODELS_VTABLE 0x2F5A300
#define FH2_GAME_PROCEDURALMODELS_VTABLE_BYTES "\xC0\x4E\x88\x41\x01\x00\x00\x00"

/* ---- Skills -------------------------------------------------------------------------------------- */

/* The Destruction skill's vtable (WreckingBall hooks its Load slot). */
#define FH2_GAME_SKILL_DESTRUCTION_VTABLE 0x2D4BBF0
#define FH2_GAME_SKILL_DESTRUCTION_VTABLE_BYTES "\xA0\x98\xA7\x40\x01\x00\x00\x00"

/* The Lumberjack skill's vtable. WreckingBall. */
#define FH2_GAME_SKILL_LUMBERJACK_VTABLE 0x2D4BA28
#define FH2_GAME_SKILL_LUMBERJACK_VTABLE_BYTES "\xA0\x98\xA7\x40\x01\x00\x00\x00"

/* The Sideswipe skill's vtable. WreckingBall. */
#define FH2_GAME_SKILL_SIDESWIPE_VTABLE 0x2D4B270
#define FH2_GAME_SKILL_SIDESWIPE_VTABLE_BYTES "\xA0\x96\xA7\x40\x01\x00\x00\x00"

/* ---- Achievements and HUD notifications ---------------------------------------------------------- */

/* The EtxEventWrite import slot (telemetry events). Azimuth fills it, so there are no bytes to
 * check. Achievements. */
#define FH2_GAME_IMPORT_ETXEVENTWRITE 0x1DD0060

/* An instruction in SetStat, with the stat's new value in registers. Achievements. */
#define FH2_GAME_SETSTAT_AT 0x2F7977
#define FH2_GAME_SETSTAT_AT_BYTES "\x45\x33\xC9\x45\x8D\x41\x20"

/* An instruction in the HUD notification's constructor. Achievements. */
#define FH2_GAME_HUD_NOTIFICATION_CTOR_AT 0x73E0B1
#define FH2_GAME_HUD_NOTIFICATION_CTOR_AT_BYTES "\x48\x8D\x8B\xA0\x01\x00\x00"

/* An instruction in the HUD notification's destructor. Achievements. */
#define FH2_GAME_HUD_NOTIFICATION_DTOR_AT 0x73F046
#define FH2_GAME_HUD_NOTIFICATION_DTOR_AT_BYTES "\x48\x89\x41\x28\x48\x81\xC1\xA0\x01\x00\x00"

/* An instruction in the AchievementsModule's tick. Achievements. */
#define FH2_GAME_ACHIEVEMENTSMODULE_TICK_AT 0x3FCD84
#define FH2_GAME_ACHIEVEMENTSMODULE_TICK_AT_BYTES "\x8B\x81\xA8\x00\x00\x00"

/* MakeDisplayString. Achievements. */
#define FH2_GAME_MAKE_DISPLAY_STRING 0x323310
#define FH2_GAME_MAKE_DISPLAY_STRING_BYTES "\x40\x57\x48\x83\xEC\x30\x48\xC7\x44\x24\x28\xFE"

/* Shows a HUD notification. Achievements. */
#define FH2_GAME_SHOW_NOTIFICATION 0x755330
#define FH2_GAME_SHOW_NOTIFICATION_BYTES "\x4C\x89\x4C\x24\x20\x4C\x89\x44\x24\x18\x48\x89"

/* ---- Radio (FMOD) -------------------------------------------------------------------------------- */

/* The call to System::createSound in RadioStreamFmod::load (for hook_call). FH2 Universal Radio. */
#define FH2_GAME_RADIOSTREAMFMOD_LOAD_CREATESOUND_CALL 0x1018A13
#define FH2_GAME_RADIOSTREAMFMOD_LOAD_CREATESOUND_CALL_BYTES "\xE8\x28\xED\x97\x00"

/* The call to Sound::getSubSound in RadioStreamFmod::start (for hook_call). FH2 Universal Radio. */
#define FH2_GAME_RADIOSTREAMFMOD_START_GETSUBSOUND_CALL 0x10221F2
#define FH2_GAME_RADIOSTREAMFMOD_START_GETSUBSOUND_CALL_BYTES "\xE8\x19\xD3\x97\x00"

/* ---- The game's runtime -------------------------------------------------------------------------- */

/* The game's operator new (its own heap, through its own TLS: only the game's threads have it set
 * up). */
#define FH2_GAME_OPERATOR_NEW 0x3229C0
#define FH2_GAME_OPERATOR_NEW_BYTES "\x48\x89\x5C\x24\x08\x57\x48\x83\xEC\x20\x8B\x15"

/* The game's operator delete. */
#define FH2_GAME_OPERATOR_DELETE 0x31C7B0
#define FH2_GAME_OPERATOR_DELETE_BYTES "\x48\x85\xC9\x74\x1D\x53\x48\x83\xEC\x20\x48\x8B"

/* Calls MSVCR110's rand (import slot 0x141DD0A90). */
#define FH2_GAME_RAND 0x1259200
#define FH2_GAME_RAND_BYTES "\x48\x83\xEC\x28\xFF\x15\x86\x78\xB7\x00"
/* ---- The platform's own ----------------------------------------------------------------------- */

/* Hooked by the platform for the Mods page, its own pages, the MODS tab and the title-screen
 * entries: AVUI::Type::FindProperty game+0x157FF60; the pause and settings menus' tiles
 * 0xD9FC10, 0xDC8A60, 0xDC6C40, 0xDC6E30 and 0xD9F9F0; the HUD options page 0xB3A3E0 and
 * 0xB0B990; the Marketplace handlers 0xDBD720, 0xDBDD00, 0xDBDD60 and 0xDBFE40; the
 * Achievements tile's calls 0xDBD635 and 0xDBF185; and the title screen's buttons (Splash vtable
 * +0x578, game+0x2E53940). Don't hook them: use the platform's settings, pages and title-screen
 * entries instead. */

#endif /* FH2GAME_H */
